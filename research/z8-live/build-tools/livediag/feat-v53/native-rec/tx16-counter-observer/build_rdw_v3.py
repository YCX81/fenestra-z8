from pathlib import Path
import hashlib,json,subprocess,difflib
P=Path('/path/to/user/fxfix/v53-tx16-counter-rdw-v3');P.mkdir(exist_ok=True)
B=Path('/path/to/user/fxfix/v53-tx16');M=Path('/path/to/user/fxfix/v53-tx16-counter-merged-v3')
s=(B/'nikon-z8-lab.c').read_text();old=s
assert hashlib.sha256(s.encode()).hexdigest()=='631aa798f15747860ac95fc9e7283f86856c79f5f503d56205cb67e10d587705'
h=(P/'reviewed-sfx-header.h').read_text();assert hashlib.sha256(h.encode()).hexdigest()=='44b29de0baf9b4fbad89501ec6576f08f3d813f950824c1ac028f1cc9b2ca70d'
mh=(M/'nikon-private-tx16.h').read_text().replace('NikonZ9State','NikonZ8State').replace('NIKON_Z9_LAB','NIKON_Z8_LAB')
def rep(text,a,b):
 assert text.count(a)==1,(a,text.count(a));return text.replace(a,b)
def fun(text,signature):
 start=text.index(signature);p=text.index('{',start);i=p+1;n=1
 while n:n+=(text[i]=='{')-(text[i]=='}');i+=1
 return text[start:i]
h=rep(h,fun(h,'static void serial5_tx_drain_schedule('),fun(mh,'static uint64_t serial5_tx_clock_delay(')+'\n'+fun(mh,'static void serial5_tx_drain_schedule('))
getter=fun(mh,'static char *get_serial5_tx_drain_state(').replace('UINT64_C(0),0u','s->serial5_host_pcm_bytes,s->serial5_host_pcm?s->serial5_host_pcm->len:0')
h=rep(h,fun(h,'static char *get_serial5_tx_drain_state('),getter)
h+='\n'+fun(mh,'static bool get_serial5_tx_counter_clock(')+'\n'+fun(mh,'static void set_serial5_tx_counter_clock(')+'\n'
s=rep(s,'    bool serial5_tx16_ring_model;','''    bool serial5_tx16_ring_model,serial5_tx16_sfx_model,serial5_tx_counter_clock;
    uint64_t serial5_tx16_sfx_jobs,serial5_tx16_ring_jobs;
    int serial5_tx_scheduled_ch;
    uint64_t serial5_tx_scheduled_source;
    unsigned serial5_tx_scheduled_bytes;
    uint32_t serial5_tx_scheduled_global,serial5_tx_scheduled_dma[7],serial5_tx_scheduled_config[8];''')
s=rep(s,'    r[word] = value;\n    pdma_irq_update(s, ch);\n    serial5_tx_drain_schedule(s);','    r[word] = value;\n    pdma_irq_update(s, ch);\n    serial5_tx_drain_note_write(s,ch,word);\n    serial5_tx_drain_schedule(s);')
a='    object_class_property_add_bool(oc,"serial5-tx16-ring-model",get_serial5_tx16_ring_model,set_serial5_tx16_ring_model);'
s=rep(s,a,a+'\n    object_class_property_add_bool(oc,"serial5-tx16-sfx-model",get_serial5_tx16_sfx_model,set_serial5_tx16_sfx_model);\n    object_class_property_add_bool(oc,"serial5-tx-counter-clock",get_serial5_tx_counter_clock,set_serial5_tx_counter_clock);')
(P/'nikon-z8-lab.c').write_text(s);(P/'nikon-z8-audio-tx.h').write_text(h)
(P/'rdw-v3.patch').write_text(''.join(difflib.unified_diff(old.splitlines(True),s.splitlines(True),fromfile='a/nikon-z8-lab.c',tofile='b/nikon-z8-lab.c'))+''.join(difflib.unified_diff((B/'nikon-z8-audio-tx.h').read_text().splitlines(True),h.splitlines(True),fromfile='a/nikon-z8-audio-tx.h',tofile='b/nikon-z8-audio-tx.h')))
cmd=[x.replace(str(B),str(P)) for x in json.loads((B/'private-compile.json').read_text())];(P/'private-compile.json').write_text(json.dumps(cmd,indent=2))
build=Path('/path/to/user/.local/share/nikon-z8-qemu/rdw/build')
with (P/'compile.log').open('w') as f:r=subprocess.run(cmd,cwd=build,stdout=f,stderr=subprocess.STDOUT)
print('compile',r.returncode,flush=True)
if r.returncode:print((P/'compile.log').read_text()[-10000:]);raise SystemExit(r.returncode)
rsp=(B/'private-link.rsp').read_text().replace(str(B),str(P));(P/'private-link.rsp').write_text(rsp)
with (P/'link.log').open('w') as f:r=subprocess.run(['c++','-m64','@'+str(P/'private-link.rsp')],cwd=build,stdout=f,stderr=subprocess.STDOUT)
print('link',r.returncode,flush=True)
if r.returncode:print((P/'link.log').read_text()[-10000:]);raise SystemExit(r.returncode)
d={n:hashlib.sha256((P/n).read_bytes()).hexdigest() for n in ['nikon-z8-lab.c','nikon-z8-audio-tx.h','qemu-system-aarch64','rdw-v3.patch']};(P/'hashes.json').write_text(json.dumps(d,indent=2));print(json.dumps(d,indent=2))
