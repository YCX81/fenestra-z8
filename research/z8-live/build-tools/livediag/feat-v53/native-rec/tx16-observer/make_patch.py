from pathlib import Path
import hashlib,difflib
P=Path(__file__).resolve().parent;s=(P/'baseline.h').read_text();assert hashlib.sha256(s.encode()).hexdigest()=='ea130de909985e5073a48efa5d72b77fe4fa5fd74c9a55c8b2852563fba05fa8'
def one(a,b):
 global s
 assert s.count(a)==1,(a,s.count(a));s=s.replace(a,b)
one('    bool paired=(s->serial5_config[0][0]&2)!=0;\n    int lanes','''    bool paired=(s->serial5_config[0][0]&2)!=0;
    /* Private original F REC contract: packed stereo LE16, two request
     * banks27/28 to FIFO104, exactly192000B each. Not a generic sink. */
    bool narrow16=s->serial5_tx16_ring_model;
    if (narrow16 && (paired || s->serial5_audio_output_id)) { return -1; }
    int lanes''')
one('if(request!=27 && !(paired && request==28))','if(request!=27 && !((paired || narrow16) && request==28))')
one('        if(lanes[lane]>=0 || (r[0]&0x40ff0000)', '        if(lanes[lane]>=0 || (narrow16 && length!=192000) || (r[0]&0x40ff0000)')
one('    unsigned lane=paired?s->serial5_tx_drain_phase:0;','''    if (narrow16 && lanes[0]>=0 && lanes[1]>=0 &&
        addresses[0]<addresses[1]+lengths[1] && addresses[1]<addresses[0]+lengths[0]) { return -1; }
    unsigned lane=(paired || narrow16)?s->serial5_tx_drain_phase:0;''')
one('    if(paired) { s->serial5_tx_drain_phase^=1;s->serial5_tx_drain_prefilled=true; }','    if(paired || s->serial5_tx16_ring_model) { s->serial5_tx_drain_phase^=1;s->serial5_tx_drain_prefilled=true; }')
one('((s->serial5_config[0][0]&2) && s->serial5_tx_drain_prefilled ?', '(((s->serial5_config[0][0]&2) || s->serial5_tx16_ring_model) && s->serial5_tx_drain_prefilled ?')
one('(uint64_t)bytes*1000000000/(48000*8)', '(uint64_t)bytes*1000000000/(48000*(s->serial5_tx16_ring_model?4:8))')
one('        "\\"pcm_rate_hz\\":48000,\\"host_output\\":%s,"', '        "\\"pcm_rate_hz\\":48000,\\"pcm_bytes_per_frame\\":%u,\\"private_tx16_ring\\":%s,\\"host_output\\":%s,"')
one('Z8_AUDIO_TX_BUDGET,s->serial5_audio_output_id?', 'Z8_AUDIO_TX_BUDGET,s->serial5_tx16_ring_model?4:8,s->serial5_tx16_ring_model?"true":"false",s->serial5_audio_output_id?')
one(' if(s->lpcm_realized) { error_setg(errp,"immutable audio output backend");return; }',' if(s->lpcm_realized || s->serial5_tx16_ring_model) { error_setg(errp,"immutable audio output backend; private TX16 is observation only");return; }')
s+='''
/* Private opt-in; all old TX behaviour is byte-for-byte unchanged while off. */
static bool get_serial5_tx16_ring_model(Object *obj, Error **errp)
{ return NIKON_Z8_LAB(obj)->serial5_tx16_ring_model; }
static void set_serial5_tx16_ring_model(Object *obj, bool value, Error **errp)
{
    NikonZ8State *s=NIKON_Z8_LAB(obj);
    if ((s->serial5_config[0][0]&0x3000) || s->serial5_tx_drain_jobs ||
        s->serial5_tx_drain_phase || s->serial5_audio_output_id) {
        error_setg(errp,"private TX16 selection immutable after audio traffic; no host output");return;
    }
    s->serial5_tx16_ring_model=value;
}
'''
(P/'nikon-z8-audio-tx.h').write_text(s)
(P/'tx16.patch').write_text(''.join(difflib.unified_diff((P/'baseline.h').read_text().splitlines(True),s.splitlines(True),fromfile='a/hw/arm/nikon-z8-audio-tx.h',tofile='b/hw/arm/nikon-z8-audio-tx.h')))
print(hashlib.sha256(s.encode()).hexdigest())
