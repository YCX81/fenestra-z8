from pathlib import Path
import hashlib,json,subprocess,difflib
P=Path('/path/to/user/fxfix/v53-tx16-counter-merged-v3');P.mkdir(exist_ok=True)
B=Path('/path/to/user/fxfix/v53-tx16-sfx-merged-v2')
s=(B/'nikon-z9-lab.c').read_text();h=(B/'nikon-private-tx16.h').read_text();old=s;oldh=h
assert hashlib.sha256(s.encode()).hexdigest()=='e35c3761eca2f465f1cf26b9ddefe15789680d0f4c10b39d7fa2382ba8d368f2'
assert hashlib.sha256(h.encode()).hexdigest()=='7fe2140a2d233a7eddae577c3ee6ee18dd8c3bd6954cca9db2b24c9c97e3b77b'
def rep(text,a,b):
 assert text.count(a)==1,(a,text.count(a));return text.replace(a,b)
s=rep(s,'bool serial5_tx_drain_model, serial5_tx16_ring_model, serial5_tx16_sfx_model;', 'bool serial5_tx_drain_model, serial5_tx16_ring_model, serial5_tx16_sfx_model, serial5_tx_counter_clock;')
a='    object_class_property_add_bool(oc,"serial5-tx16-sfx-model",get_serial5_tx16_sfx_model,set_serial5_tx16_sfx_model);'
s=rep(s,a,a+'\n    object_class_property_add_bool(oc,"serial5-tx-counter-clock",get_serial5_tx_counter_clock,set_serial5_tx_counter_clock);')
h=rep(h,'static void serial5_tx_drain_schedule(NikonZ9State *s)', '''/* Optional counter-domain peripheral clock. The pinned24MHz ARM model uses
 * integer41ns ticks; native1us=24ticks=984QOMns. No ARM timer or guest deadline
 * is altered. All admitted delays are <=1second, so this product is bounded. */
static uint64_t serial5_tx_clock_delay(NikonZ9State*s,uint64_t ns)
{ return s->serial5_tx_counter_clock?ns*984u/1000u:ns; }
static void serial5_tx_drain_schedule(NikonZ9State *s)''')
h=rep(h,'                  (s->serial5_tx16_sfx_model && bytes==4800 ? UINT64_C(25000000) :','                  serial5_tx_clock_delay(s,(s->serial5_tx16_sfx_model && bytes==4800 ? UINT64_C(25000000) :')
h=rep(h,'(48000*(s->serial5_tx16_ring_model?4:8))) : 1000000));','(48000*(s->serial5_tx16_ring_model?4:8))) : 1000000)));')
h=rep(h,'        "\\"budget_bytes\\":%u,\\"prefill_delay_ns\\":1000000,"','''        "\\"budget_bytes\\":%u,\\"prefill_delay_ns\\":%"PRIu64","
        "\\"counter_clock\\":%s,\\"clock_domain\\":\\"%s\\","''')
h=rep(h,'Z8_AUDIO_TX_BUDGET,(s->serial5_tx16_ring_model||s->serial5_tx16_sfx_model)?4:8,','''Z8_AUDIO_TX_BUDGET,serial5_tx_clock_delay(s,1000000),s->serial5_tx_counter_clock?"true":"false",
        s->serial5_tx_counter_clock?"guest24MHz-counter":"QOM-virtual",
        (s->serial5_tx16_ring_model||s->serial5_tx16_sfx_model)?4:8,''')
h+='''
static bool get_serial5_tx_counter_clock(Object *obj, Error **errp)
{ return NIKON_Z9_LAB(obj)->serial5_tx_counter_clock; }
static void set_serial5_tx_counter_clock(Object *obj, bool value, Error **errp)
{
    NikonZ9State *s=NIKON_Z9_LAB(obj);
    if((s->serial5_config[0][0]&0x3000) || s->serial5_tx_drain_jobs || s->serial5_tx_drain_phase) {
        error_setg(errp,"TX clock domain immutable after audio traffic");return;
    }
    s->serial5_tx_counter_clock=value;
}
'''
(P/'nikon-z9-lab.c').write_text(s);(P/'nikon-private-tx16.h').write_text(h)
(P/'counter-clock.patch').write_text(''.join(difflib.unified_diff(old.splitlines(True),s.splitlines(True),fromfile='a/nikon-z9-lab.c',tofile='b/nikon-z9-lab.c'))+''.join(difflib.unified_diff(oldh.splitlines(True),h.splitlines(True),fromfile='a/nikon-private-tx16.h',tofile='b/nikon-private-tx16.h')))
cmd=[x.replace(str(B),str(P)) for x in json.loads((B/'private-compile.json').read_text())];(P/'private-compile.json').write_text(json.dumps(cmd,indent=2))
build=Path('/path/to/user/.local/share/nikon-z8-qemu/merged/work/build')
with (P/'compile.log').open('w') as f:r=subprocess.run(cmd,cwd=build,stdout=f,stderr=subprocess.STDOUT)
print('compile',r.returncode,flush=True)
if r.returncode:print((P/'compile.log').read_text()[-12000:]);raise SystemExit(r.returncode)
rsp=(B/'private-link.rsp').read_text().replace(str(B),str(P));(P/'private-link.rsp').write_text(rsp)
with (P/'link.log').open('w') as f:r=subprocess.run(['cc','-m64','@'+str(P/'private-link.rsp')],cwd=build,stdout=f,stderr=subprocess.STDOUT)
print('link',r.returncode,flush=True)
if r.returncode:print((P/'link.log').read_text()[-12000:]);raise SystemExit(r.returncode)
d={n:hashlib.sha256((P/n).read_bytes()).hexdigest() for n in ['nikon-z9-lab.c','nikon-private-tx16.h','qemu-system-aarch64','counter-clock.patch']};(P/'hashes.json').write_text(json.dumps(d,indent=2));print(json.dumps(d,indent=2))
