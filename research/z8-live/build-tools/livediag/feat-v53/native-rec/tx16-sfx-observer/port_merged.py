from pathlib import Path
import hashlib,json,difflib,subprocess
P=Path('/path/to/user/fxfix/v53-tx16-sfx-merged-v2');P.mkdir(exist_ok=True)
B=Path('/path/to/user/fxfix/v53-codec-spi-merged')
s=(B/'nikon-z9-lab.c').read_text();old=s
assert hashlib.sha256(s.encode()).hexdigest()=='b0a205506dc9c19c4504af643249b5f74e091d91c1d9b0a48842a507102decd1'
def one(a,b):
 global s
 assert s.count(a)==1,(a,s.count(a));s=s.replace(a,b)
h=(P/'nikon-z8-audio-tx.h').read_text()
# The merged core has no host audio backend. Port only the admitted 16bit
# observer; do not introduce rdw's general32bit mode or host-output API.
a=h.index('/* Optional host output.');b=h.index('/* A deadline belongs')
h=h[:a]+'''static bool serial5_host_audio_enqueue(NikonZ8State*s,const uint8_t*d,unsigned n)
{ return s->serial5_audio_output_id==NULL; }
'''+h[b:]
a=h.index('static char *get_serial5_audio_output(');b=h.index('/* Private opt-in;',a)
h=h[:a]+h[b:]
h=h.replace('!s->serial5_tx_drain_model || !s->bootstrap_fixtures ||','!s->serial5_tx_drain_model || !(s->serial5_tx16_ring_model || s->serial5_tx16_sfx_model) || !s->bootstrap_fixtures ||',1)
h=h.replace('s->serial5_host_pcm_bytes,s->serial5_host_pcm?s->serial5_host_pcm->len:0','UINT64_C(0),0u')
h=h.replace('NikonZ8State','NikonZ9State').replace('NIKON_Z8_LAB','NIKON_Z9_LAB')
(P/'nikon-private-tx16.h').write_text(h)
fields='''    /* Private observation only: original armedTX -> RAM read -> DMAIRQ. */
    bool serial5_tx_drain_model, serial5_tx16_ring_model, serial5_tx16_sfx_model;
    unsigned serial5_tx_drain_phase;
    bool serial5_tx_drain_prefilled;
    GByteArray *serial5_pcm_capture;
    char *serial5_audio_output_id; /* AlwaysNULL: no host-output property. */
    QEMUTimer *serial5_tx_drain_timer;
    GChecksum *serial5_tx_drain_hash;
    uint64_t serial5_tx_drain_bytes, serial5_tx_drain_jobs;
    uint64_t serial5_tx_drain_last_source, serial5_tx_drain_empty_reads;
    uint64_t serial5_tx16_sfx_jobs, serial5_tx16_ring_jobs;
    int serial5_tx_scheduled_ch;
    uint64_t serial5_tx_scheduled_source;
    unsigned serial5_tx_scheduled_bytes;
    uint32_t serial5_tx_scheduled_global,serial5_tx_scheduled_dma[7],serial5_tx_scheduled_config[8];
    const char *serial5_tx_drain_fault;
'''
one('    unsigned lpcm_tx_slot, serial5_pcm_phase[5];','    unsigned lpcm_tx_slot, serial5_pcm_phase[5];\n'+fields)
one('static void pdma_progress(NikonZ9State *s, unsigned ch, unsigned remaining)','static void serial5_tx_drain_schedule(NikonZ9State *s);\n\nstatic void pdma_progress(NikonZ9State *s, unsigned ch, unsigned remaining)')
one('static bool serial_refill(NikonZ9State *s, unsigned ch)','#include "nikon-private-tx16.h"\n\nstatic bool serial_refill(NikonZ9State *s, unsigned ch)')
# Preserving existing validation and status write semantics; schedule only
# re-examines the complete tuple and never completes an unarmed descriptor.
one('    r[word] = value;\n    pdma_irq_update(s, ch);\n    return true;', '    r[word] = value;\n    pdma_irq_update(s, ch);\n    serial5_tx_drain_note_write(s,ch,word);\n    serial5_tx_drain_schedule(s);\n    return true;')
one('    uint32_t *serial_config = serial5_config(s, addr, size);\n    if (serial_config) {\n        return *serial_config;', '    uint64_t tx16_value;\n    if(serial5_tx_drain_read(s,addr,&tx16_value,size)) { return tx16_value; }\n    uint32_t *serial_config = serial5_config(s, addr, size);\n    if (serial_config) {\n        return *serial_config;')
one('            s->serial5_pcm_phase[(addr - 0x0e200100) / 0x100] = 0;\n        }\n        return;', '            s->serial5_pcm_phase[(addr - 0x0e200100) / 0x100] = 0;\n        }\n        serial5_tx_drain_schedule(s);\n        return;')
one('        s->peripheral_global[(addr - 0x0e100000) >> 16] = value;\n        return;', '        s->peripheral_global[(addr - 0x0e100000) >> 16] = value;\n        serial5_tx_drain_schedule(s);\n        return;')
anchor='    object_class_property_add_str(oc,"serial5-pcm16-state",get_serial5_pcm16_state,NULL);'
one(anchor,anchor+'''
    object_class_property_add_bool(oc,"serial5-tx-drain-model",get_serial5_tx_drain_model,set_serial5_tx_drain_model);
    object_class_property_add_bool(oc,"serial5-tx16-ring-model",get_serial5_tx16_ring_model,set_serial5_tx16_ring_model);
    object_class_property_add_bool(oc,"serial5-tx16-sfx-model",get_serial5_tx16_sfx_model,set_serial5_tx16_sfx_model);
    object_class_property_add_str(oc,"serial5-tx-drain-state",get_serial5_tx_drain_state,NULL);
    object_class_property_add_str(oc,"serial5-tx-pcm-capture",get_serial5_pcm_capture,NULL);''')
one('static void nikon_z9_finalize(Object *obj)\n{\n    NikonZ9State *s = NIKON_Z9_LAB(obj);', '''static void nikon_z9_finalize(Object *obj)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    if(s->serial5_tx_drain_timer) { timer_free(s->serial5_tx_drain_timer); }
    if(s->serial5_tx_drain_hash) { g_checksum_free(s->serial5_tx_drain_hash); }
    if(s->serial5_pcm_capture) { g_byte_array_unref(s->serial5_pcm_capture); }''')
(P/'baseline.c').write_text(old);(P/'nikon-z9-lab.c').write_text(s)
(P/'merged-tx16.patch').write_text(''.join(difflib.unified_diff(old.splitlines(True),s.splitlines(True),fromfile='a/nikon-z9-lab.c',tofile='b/nikon-z9-lab.c')))
cmd=json.loads((B/'private-compile.json').read_text())
cmd=[x.replace(str(B),str(P)) for x in cmd]
(P/'private-compile.json').write_text(json.dumps(cmd,indent=2))
build=Path('/path/to/user/.local/share/nikon-z8-qemu/merged/work/build')
with (P/'compile.log').open('w') as f:r=subprocess.run(cmd,cwd=build,stdout=f,stderr=subprocess.STDOUT)
print('compile',r.returncode,flush=True)
if r.returncode: print((P/'compile.log').read_text()[-10000:]);raise SystemExit(r.returncode)
rsp=(B/'private-link.rsp').read_text().replace(str(B),str(P));(P/'private-link.rsp').write_text(rsp)
with (P/'link.log').open('w') as f:r=subprocess.run(['cc','-m64','@'+str(P/'private-link.rsp')],cwd=build,stdout=f,stderr=subprocess.STDOUT)
print('link',r.returncode,flush=True)
if r.returncode: print((P/'link.log').read_text()[-10000:]);raise SystemExit(r.returncode)
d={x:hashlib.sha256((P/x).read_bytes()).hexdigest() for x in ['baseline.c','nikon-z9-lab.c','nikon-private-tx16.h','qemu-system-aarch64']}
(P/'hashes.json').write_text(json.dumps(d,indent=2));print(json.dumps(d,indent=2))
