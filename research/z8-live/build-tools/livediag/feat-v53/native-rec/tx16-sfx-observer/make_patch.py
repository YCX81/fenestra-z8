from pathlib import Path
import hashlib,difflib
P=Path(__file__).resolve().parent
s=(P/'baseline.h').read_text()
assert hashlib.sha256(s.encode()).hexdigest()=='de2963693b04b0e1240720e1819c69729b48297fa181b4f19ed738798b0914d2'
def one(a,b):
 global s
 assert s.count(a)==1,(a,s.count(a));s=s.replace(a,b)
one('    bool narrow16=s->serial5_tx16_ring_model;','''    bool narrow16=s->serial5_tx16_ring_model || s->serial5_tx16_sfx_model;''')
one('    if (narrow16 && (paired || s->serial5_audio_output_id)) { return -1; }','''    /* Actual Ready2/3 SFX serial108=450a2000. Reject unknown format/clock
     * controls instead of interpreting arbitrary TX enable as stereoS16. */
    if (narrow16 && (s->serial5_config[0][0]!=0x450a2000 || s->serial5_audio_output_id)) { return -1; }''')
one('(narrow16 && length!=192000)', '(narrow16 && !((s->serial5_tx16_ring_model && length==192000) || (s->serial5_tx16_sfx_model && request==27 && length==4800)))')
one('    unsigned lane=(paired || narrow16)?s->serial5_tx_drain_phase:0;','''    /* SFX is one original request27 only. Reject mixed owners/bank lengths;
     * a zero-length native descriptor underflows to262144 and stays rejected. */
    bool sfx=narrow16 && lanes[0]>=0 && lengths[0]==4800;
    if (sfx && lanes[1]>=0) { return -1; }
    unsigned lane=sfx?0:(paired || narrow16)?s->serial5_tx_drain_phase:0;''')
one('    bool paired=(s->serial5_config[0][0]&2)!=0;\n    pdma_progress', '''    bool paired=(s->serial5_config[0][0]&2)!=0;
    bool sfx=s->serial5_tx16_sfx_model && bytes==4800;
    if(sfx) { s->serial5_tx16_sfx_jobs++; }
    else if(s->serial5_tx16_ring_model) { s->serial5_tx16_ring_jobs++; }
    fprintf(stderr,"Z8_PRIVATE_TX16 job=%"PRIu64" kind=%s ns=%"PRId64" source=%"PRIx64" bytes=%u channel=%d\\n",
        s->serial5_tx_drain_jobs,sfx?"sfx4800":"ring192000",qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),source,bytes,channel);
    pdma_progress''')
one('    if(paired || s->serial5_tx16_ring_model) { s->serial5_tx_drain_phase^=1;s->serial5_tx_drain_prefilled=true; }','    if(!sfx && (paired || s->serial5_tx16_ring_model)) { s->serial5_tx_drain_phase^=1;s->serial5_tx_drain_prefilled=true; }')
one('static void serial5_tx_drain_tick(void *opaque)', '''/* A deadline belongs to the exact admitted request. Guest reprogramming
 * must start a fresh deadline, never consume a different owner early. */
static bool serial5_tx_drain_same(NikonZ8State *s,int ch,uint64_t source,unsigned bytes)
{
    return s->serial5_tx_scheduled_ch==ch && s->serial5_tx_scheduled_source==source &&
        s->serial5_tx_scheduled_bytes==bytes &&
        s->serial5_tx_scheduled_global==s->peripheral_global[ch/8] &&
        !memcmp(s->serial5_tx_scheduled_dma,s->peripheral24[ch],sizeof(s->serial5_tx_scheduled_dma)) &&
        !memcmp(s->serial5_tx_scheduled_config,s->serial5_config[0],sizeof(s->serial5_tx_scheduled_config));
}
static void serial5_tx_drain_note_write(NikonZ8State *s,unsigned ch,unsigned word)
{
    if((s->serial5_tx16_ring_model || s->serial5_tx16_sfx_model) && !word &&
       s->serial5_tx_drain_timer && timer_pending(s->serial5_tx_drain_timer) &&
       s->serial5_tx_scheduled_ch==(int)ch) { timer_del(s->serial5_tx_drain_timer); }
}
static void serial5_tx_drain_tick(void *opaque)''')
one('    if (bytes > Z8_AUDIO_TX_BUDGET - s->serial5_tx_drain_bytes ||','''    if ((s->serial5_tx16_ring_model || s->serial5_tx16_sfx_model) &&
        !serial5_tx_drain_same(s,channel,source,bytes)) {
        s->serial5_tx_drain_fault="pending-tuple-changed";return;
    }
    if (bytes > Z8_AUDIO_TX_BUDGET - s->serial5_tx_drain_bytes ||''')
one('''    if (s->serial5_tx_drain_fault ||
        serial5_tx_drain_request(s, &source, &bytes) < 0) {''','''    int channel=serial5_tx_drain_request(s, &source, &bytes);
    bool narrow16=s->serial5_tx16_ring_model || s->serial5_tx16_sfx_model;
    if(narrow16 && channel>=0 && timer_pending(s->serial5_tx_drain_timer) &&
       !serial5_tx_drain_same(s,channel,source,bytes)) { timer_del(s->serial5_tx_drain_timer); }
    if (s->serial5_tx_drain_fault || channel < 0) {''')
one('''    } else if (!timer_pending(s->serial5_tx_drain_timer)) {
        /* Declared''','''    } else if (!timer_pending(s->serial5_tx_drain_timer)) {
        s->serial5_tx_scheduled_ch=channel;
        s->serial5_tx_scheduled_source=source;s->serial5_tx_scheduled_bytes=bytes;
        s->serial5_tx_scheduled_global=s->peripheral_global[channel/8];
        memcpy(s->serial5_tx_scheduled_dma,s->peripheral24[channel],sizeof(s->serial5_tx_scheduled_dma));
        memcpy(s->serial5_tx_scheduled_config,s->serial5_config[0],sizeof(s->serial5_tx_scheduled_config));
        /* Declared''')
one('                  (((s->serial5_config[0][0]&2) || s->serial5_tx16_ring_model) && s->serial5_tx_drain_prefilled ?', '                  (s->serial5_tx16_sfx_model && bytes==4800 ? UINT64_C(25000000) :\n                   ((s->serial5_config[0][0]&2) || s->serial5_tx16_ring_model) && s->serial5_tx_drain_prefilled ?')
one('        "\\"pcm_rate_hz\\":48000,\\"pcm_bytes_per_frame\\":%u,\\"private_tx16_ring\\":%s,\\"host_output\\":%s,"','''        "\\"pcm_rate_hz\\":48000,\\"pcm_bytes_per_frame\\":%u,\\"private_tx16_ring\\":%s,\\"host_output\\":%s,"
        "\\"private_tx16_sfx\\":%s,\\"ring_jobs\\":%"PRIu64",\\"sfx_jobs\\":%"PRIu64","''')
one('Z8_AUDIO_TX_BUDGET,s->serial5_tx16_ring_model?4:8,s->serial5_tx16_ring_model?"true":"false",s->serial5_audio_output_id?"true":"false",','''Z8_AUDIO_TX_BUDGET,(s->serial5_tx16_ring_model||s->serial5_tx16_sfx_model)?4:8,s->serial5_tx16_ring_model?"true":"false",s->serial5_audio_output_id?"true":"false",
        s->serial5_tx16_sfx_model?"true":"false",s->serial5_tx16_ring_jobs,s->serial5_tx16_sfx_jobs,''')
one('if(s->lpcm_realized || s->serial5_tx16_ring_model)', 'if(s->lpcm_realized || s->serial5_tx16_ring_model || s->serial5_tx16_sfx_model)')
s+='''
static bool get_serial5_tx16_sfx_model(Object *obj, Error **errp)
{ return NIKON_Z8_LAB(obj)->serial5_tx16_sfx_model; }
static void set_serial5_tx16_sfx_model(Object *obj, bool value, Error **errp)
{
    NikonZ8State *s=NIKON_Z8_LAB(obj);
    if ((s->serial5_config[0][0]&0x3000) || s->serial5_tx_drain_jobs ||
        s->serial5_tx_drain_phase || s->serial5_audio_output_id) {
        error_setg(errp,"private SFX selection immutable after audio traffic; no host output");return;
    }
    s->serial5_tx16_sfx_model=value;
}
'''
(P/'nikon-z8-audio-tx.h').write_text(s)
(P/'sfx.patch').write_text(''.join(difflib.unified_diff((P/'baseline.h').read_text().splitlines(True),s.splitlines(True),fromfile='a/nikon-z8-audio-tx.h',tofile='b/nikon-z8-audio-tx.h')))
print(hashlib.sha256(s.encode()).hexdigest())
