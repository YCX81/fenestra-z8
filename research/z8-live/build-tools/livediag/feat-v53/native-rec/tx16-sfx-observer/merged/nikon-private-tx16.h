/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 RdWing
 * Bounded channel-0 PCM output. Mapped 32-bit single descriptors and the
 * explicit paired stereo route retain separate ownership. Source reads and
 * optional host submission precede DMA status/IRQ; no physical DAC/rate claim.
 */
#define Z8_AUDIO_TX_BUDGET (16u * 1024u * 1024u)

static int serial5_tx_drain_request(NikonZ9State *s, uint64_t *physical,
                                   unsigned *bytes)
{
    if (!s->serial5_tx_drain_model || !(s->serial5_tx16_ring_model || s->serial5_tx16_sfx_model) || !s->bootstrap_fixtures ||
        !s->bus_window_fixture || !s->serial_fixture ||
        !(s->serial5_config[0][0] & 0x2000)) {
        return -1;
    }
    bool paired=(s->serial5_config[0][0]&2)!=0;
    /* Private original F REC contract: packed stereo LE16, two request
     * banks27/28 to FIFO104, exactly192000B each. Not a generic sink. */
    bool narrow16=s->serial5_tx16_ring_model || s->serial5_tx16_sfx_model;
    if (narrow16 && (paired || s->serial5_audio_output_id)) { return -1; }
    int lanes[2]={-1,-1};uint64_t addresses[2]={0};unsigned lengths[2]={0};
    for (unsigned ch=0;ch<24;ch++) {
        uint32_t *r=s->peripheral24[ch];
        if(!(r[0]&0x80000000) || r[6]!=(paired?0x0e20013c:0x0e200104)) { continue; }
        unsigned request=(r[0]>>24)&0x3f;
        if(request!=27 && !((paired || narrow16) && request==28)) { return -1; }
        unsigned lane=request-27,length=((r[0]&0xffff)+1)*4;uint64_t source;
        if(lanes[lane]>=0 || (narrow16 && !((s->serial5_tx16_ring_model && length==192000) || (s->serial5_tx16_sfx_model && request==27 && length==4800))) || (r[0]&0x40ff0000) || (r[1]&0x70000) ||
            (r[1]&~0x001f0000u)!=0x29e00000 || (r[2]&~0x404u) || r[3] ||
            !(s->peripheral_global[ch/8]&0x80000000) || (r[4]&3) ||
            !pdma_ram_address(s,0,r[4],length,&source)) { return -1; }
        lanes[lane]=ch;addresses[lane]=source;lengths[lane]=length;
    }
    if (narrow16 && lanes[0]>=0 && lanes[1]>=0 &&
        addresses[0]<addresses[1]+lengths[1] && addresses[1]<addresses[0]+lengths[0]) { return -1; }
    /* SFX is one original request27 only. Reject mixed owners/bank lengths;
     * a zero-length native descriptor underflows to262144 and stays rejected. */
    bool sfx=narrow16 && lanes[0]>=0 && lengths[0]==4800;
    if (sfx && lanes[1]>=0) { return -1; }
    unsigned lane=sfx?0:(paired || narrow16)?s->serial5_tx_drain_phase:0;
    if(lanes[lane]<0) { return -1; }
    *physical=addresses[lane];*bytes=lengths[lane];return lanes[lane];
}

static bool serial5_host_audio_enqueue(NikonZ9State*s,const uint8_t*d,unsigned n)
{ return s->serial5_audio_output_id==NULL; }
static void serial5_tx_drain_tick(void *opaque)
{
    NikonZ9State *s = opaque;
    uint64_t source = 0;
    unsigned bytes = 0;
    int channel = serial5_tx_drain_request(s, &source, &bytes);
    if (channel < 0 || s->serial5_tx_drain_fault) {
        return;
    }
    if (bytes > Z8_AUDIO_TX_BUDGET - s->serial5_tx_drain_bytes ||
        s->serial5_tx_drain_jobs >= 1024) {
        s->serial5_tx_drain_fault = "bounded-tx-budget-exhausted";
        return;
    }
    g_autofree uint8_t *data = g_malloc(bytes);
    if (address_space_read(&address_space_memory, source, MEMTXATTRS_UNSPECIFIED,
                           data, bytes) != MEMTX_OK) {
        s->serial5_tx_drain_fault = "mapped-source-read-failed";
        return;
    }
    if(!serial5_host_audio_enqueue(s,data,bytes)) { s->serial5_tx_drain_fault="host-audio-output-unavailable";return; }
    if(!s->serial5_pcm_capture) { s->serial5_pcm_capture=g_byte_array_new(); }
    g_byte_array_append(s->serial5_pcm_capture,data,bytes);
    g_checksum_update(s->serial5_tx_drain_hash, data, bytes);
    s->serial5_tx_drain_bytes += bytes;
    s->serial5_tx_drain_jobs++;
    s->serial5_tx_drain_last_source = source;
    bool paired=(s->serial5_config[0][0]&2)!=0;
    bool sfx=s->serial5_tx16_sfx_model && bytes==4800;
    if(sfx) { s->serial5_tx16_sfx_jobs++; }
    else if(s->serial5_tx16_ring_model) { s->serial5_tx16_ring_jobs++; }
    fprintf(stderr,"Z8_PRIVATE_TX16 job=%"PRIu64" kind=%s ns=%"PRId64" source=%"PRIx64" bytes=%u channel=%d\n",
        s->serial5_tx_drain_jobs,sfx?"sfx4800":"ring192000",qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),source,bytes,channel);
    pdma_progress(s, channel, 0);
    if(!sfx && (paired || s->serial5_tx16_ring_model)) { s->serial5_tx_drain_phase^=1;s->serial5_tx_drain_prefilled=true; }
    /* A second descriptor, if any, is a separate transfer and deadline. */
    serial5_tx_drain_schedule(s);
}

static void serial5_tx_drain_schedule(NikonZ9State *s)
{
    if (!s->serial5_tx_drain_timer) {
        return;
    }
    uint64_t source = 0;
    unsigned bytes = 0;
    if(!(s->serial5_config[0][0]&0x2000)) { s->serial5_tx_drain_phase=0;s->serial5_tx_drain_prefilled=false; }
    if (s->serial5_tx_drain_fault ||
        serial5_tx_drain_request(s, &source, &bytes) < 0) {
        timer_del(s->serial5_tx_drain_timer);
    } else if (!timer_pending(s->serial5_tx_drain_timer)) {
        /* Declared functional stereo LE32/48k clock with one prefilled
         * block. This is neither measured FIFO depth nor silicon timing. */
        timer_mod(s->serial5_tx_drain_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (s->serial5_tx16_sfx_model && bytes==4800 ? UINT64_C(25000000) :
                   ((s->serial5_config[0][0]&2) || s->serial5_tx16_ring_model) && s->serial5_tx_drain_prefilled ?
                   MAX(UINT64_C(1000000),(uint64_t)bytes*1000000000/(48000*(s->serial5_tx16_ring_model?4:8))) : 1000000));
    }
}

static bool serial5_tx_drain_read(NikonZ9State *s, hwaddr addr,
                                  uint64_t *value, unsigned size)
{
    if (!s->serial5_tx_drain_model || !s->serial5_tx_drain_jobs ||
        s->serial5_tx_drain_fault || size != 4 || addr != 0x0e20011c) {
        return false;
    }
    /* Only the empty FIFO after a consumed descriptor is covered. An armed
     * replacement, including an unsupported descriptor, is not empty. */
    for (unsigned ch = 0; ch < 24; ch++) {
        if ((s->peripheral24[ch][0] & 0x80000000) &&
            (s->peripheral24[ch][6] == 0x0e200104 || s->peripheral24[ch][6] == 0x0e20013c)) {
            return false;
        }
    }
    *value = 0;
    s->serial5_tx_drain_empty_reads++;
    return true;
}

static bool get_serial5_tx_drain_model(Object *obj, Error **errp)
{
    return NIKON_Z9_LAB(obj)->serial5_tx_drain_model;
}

static void set_serial5_tx_drain_model(Object *obj, bool value, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    if (s->serial5_config[0][0] & 0x3000 || s->serial5_tx_drain_jobs) {
        error_setg(errp, "TX drain selection is immutable after a stream starts");
        return;
    }
    if (value && (!s->bootstrap_fixtures || !s->bus_window_fixture ||
                  !s->serial_fixture)) {
        error_setg(errp, "TX drain requires explicit register, bus and serial fixtures");
        return;
    }
    if (value && !s->serial5_tx_drain_timer) {
        s->serial5_tx_drain_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                               serial5_tx_drain_tick, s);
        s->serial5_tx_drain_hash = g_checksum_new(G_CHECKSUM_SHA256);
    }
    s->serial5_tx_drain_model = value;
    serial5_tx_drain_schedule(s);
}

static char *get_serial5_tx_drain_state(Object *obj, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    uint64_t source = 0;
    unsigned bytes = 0;
    int channel = serial5_tx_drain_request(s, &source, &bytes);
    char digest[65] = "";
    if (s->serial5_tx_drain_hash) {
        GChecksum *copy = g_checksum_copy(s->serial5_tx_drain_hash);
        g_strlcpy(digest, g_checksum_get_string(copy), sizeof(digest));
        g_checksum_free(copy);
    }
    return g_strdup_printf("{\"enabled\":%s,\"dma_channel\":%d,"
        "\"pending_bytes\":%u,\"pending_source\":%"PRIu64","
        "\"jobs\":%"PRIu64",\"bytes\":%"PRIu64",\"last_source\":%"PRIu64","
        "\"sha256\":\"%s\",\"empty_reads\":%"PRIu64",\"fault\":\"%s\","
        "\"budget_bytes\":%u,\"prefill_delay_ns\":1000000,"
        "\"pcm_rate_hz\":48000,\"pcm_bytes_per_frame\":%u,\"private_tx16_ring\":%s,\"host_output\":%s,"
        "\"private_tx16_sfx\":%s,\"ring_jobs\":%"PRIu64",\"sfx_jobs\":%"PRIu64","
        "\"host_pcm_bytes\":%"PRIu64",\"host_queue_bytes\":%u,"
        "\"dac_output\":false,\"physical_timing_verified\":false}",
        s->serial5_tx_drain_model ? "true" : "false", channel,
        bytes, source, s->serial5_tx_drain_jobs, s->serial5_tx_drain_bytes,
        s->serial5_tx_drain_last_source, digest, s->serial5_tx_drain_empty_reads,
        s->serial5_tx_drain_fault ? s->serial5_tx_drain_fault : "",
        Z8_AUDIO_TX_BUDGET,(s->serial5_tx16_ring_model||s->serial5_tx16_sfx_model)?4:8,s->serial5_tx16_ring_model?"true":"false",s->serial5_audio_output_id?"true":"false",
        s->serial5_tx16_sfx_model?"true":"false",s->serial5_tx16_ring_jobs,s->serial5_tx16_sfx_jobs,
        UINT64_C(0),0u);
}

static char *get_serial5_pcm_capture(Object *obj,Error **errp)
{
 NikonZ9State*s=NIKON_Z9_LAB(obj);
 return s->serial5_pcm_capture?g_base64_encode(s->serial5_pcm_capture->data,s->serial5_pcm_capture->len):g_strdup("");
}

/* Private opt-in; all old TX behaviour is byte-for-byte unchanged while off. */
static bool get_serial5_tx16_ring_model(Object *obj, Error **errp)
{ return NIKON_Z9_LAB(obj)->serial5_tx16_ring_model; }
static void set_serial5_tx16_ring_model(Object *obj, bool value, Error **errp)
{
    NikonZ9State *s=NIKON_Z9_LAB(obj);
    if ((s->serial5_config[0][0]&0x3000) || s->serial5_tx_drain_jobs ||
        s->serial5_tx_drain_phase || s->serial5_audio_output_id) {
        error_setg(errp,"private TX16 selection immutable after audio traffic; no host output");return;
    }
    s->serial5_tx16_ring_model=value;
}

static bool get_serial5_tx16_sfx_model(Object *obj, Error **errp)
{ return NIKON_Z9_LAB(obj)->serial5_tx16_sfx_model; }
static void set_serial5_tx16_sfx_model(Object *obj, bool value, Error **errp)
{
    NikonZ9State *s=NIKON_Z9_LAB(obj);
    if ((s->serial5_config[0][0]&0x3000) || s->serial5_tx_drain_jobs ||
        s->serial5_tx_drain_phase || s->serial5_audio_output_id) {
        error_setg(errp,"private SFX selection immutable after audio traffic; no host output");return;
    }
    s->serial5_tx16_sfx_model=value;
}
