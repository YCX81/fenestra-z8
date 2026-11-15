from pathlib import Path
import shutil
p=Path(__file__).resolve().parent;d=p/'driver';f=d/'retry-driver/run_live.py';s=f.read_text()
s=s.replace("extra+=['serial5-audio-config-model=on'", "extra+=['codec-spi-register-peer=on','serial5-audio-config-model=on'")
s=s.replace("f'{TASK}/rx_observer.so,out={self.out}/rx.jsonl'", "f'{TASK}/rx_observer.so,{self.out}/audio-events.jsonl'")
s=s.replace("self.next_space_check=0", "self.next_space_check=0\n        pcm=(TASK/'input.s16le').read_bytes();pcm_due=None;pcm_index=0;pcm_ready=False;audio_second=-1")
s=s.replace("def command(name,**kw):\n", "def command(name,**kw):\n            nonlocal pcm_due,pcm_index,pcm_ready,audio_second\n")
needle="            result=original(name,**kw)\n"
addition='''            if name=='query-status' and result.get('running'):
                ns=json.loads(original('qom-get',path='/machine',property='runtime-clock-state'))['virtual_ns']
                ps=json.loads(original('qom-get',path='/machine',property='serial5-pcm16-state'))
                if ps['ready']:
                    if not pcm_ready:pcm_due=ns+50000000
                    if ns>=pcm_due:
                        at=(pcm_index*9600)%len(pcm);block=pcm[at:at+9600];assert len(block)==9600 and ps['words']==2400
                        original('qom-set',path='/machine',property='serial5-pcm16-input',value=f"{ps['phase']}:{ps['local']:x}:960:{block.hex()}")
                        with(g.out/'pcm-input.jsonl').open('a')as fp:fp.write(json.dumps(dict(virtual_ns=ns,index=pcm_index,state=ps,sha256=hashlib.sha256(block).hexdigest(),late_ns=ns-pcm_due))+'\\n')
                        pcm_index+=1;pcm_due+=50000000
                        if ns-pcm_due>=50000000:raise RuntimeError('PCM feeder missed an entire50ms block')
                else:pcm_due=None
                pcm_ready=ps['ready']
                if ns//1000000000!=audio_second:
                    audio_second=ns//1000000000
                    state=json.loads(original('qom-get',path='/machine',property='codec-spi-register-state'))
                    with(g.out/'audio-peer.jsonl').open('a')as fp:fp.write(json.dumps(dict(virtual_ns=ns,codec=state,rx=ps))+'\\n')
'''
assert needle in s;s=s.replace(needle,needle+addition)
s=s.replace("info=snapshot(g,path.stem)","info=snapshot(g,path.stem)\n                if path.stem.endswith('-full-press'):\n                    r=info.get('runtime',{});assert r.get('audio_warm')==0 and r.get('audio.active')==1 and r.get('stream.encoded',0)>40, 'Refuse photo before actual PCM+video Ready'")
s=s.replace("shutter('post-encode-4s',gap=4)","shutter('post-encode-30s',gap=30)").replace("shutter('post-complete',gap=4)","shutter('post-complete',gap=15)")
f.write_text(s);shutil.copy2(p.parent/'final-native/input.s16le',d/'input.s16le')
s=(p.parent/'final-native/observer2.c').read_text().replace('static void trans(struct qemu_plugin_tb*tb,void*ud){(void)ud;', 'static void trans(qemu_plugin_id_t id,struct qemu_plugin_tb*tb){(void)id;').replace('static void init(unsigned cpu,void*ud){(void)ud;', 'static void init(qemu_plugin_id_t id,unsigned cpu){(void)id;').replace('qemu_plugin_register_vcpu_init_cb(id,init,0);qemu_plugin_register_vcpu_tb_trans_cb(id,trans,0);','qemu_plugin_register_vcpu_init_cb(id,init);qemu_plugin_register_vcpu_tb_trans_cb(id,trans);')
(d/'rx_observer.c').write_text(s)
