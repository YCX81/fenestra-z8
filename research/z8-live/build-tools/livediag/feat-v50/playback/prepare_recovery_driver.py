from pathlib import Path
import shutil
H=Path.home();B=H/'.local/share/nikon-z8-qemu';D=H/'fxfix/v50-playback/recovery-driver'
source=B/'live-audio/regress/rerun-v41';assert not D.exists();shutil.copytree(source,D)
p=D/'retry-driver/run_live.py';s=p.read_text()
old="""                    lookup_error=rd(pb+68,4),frames=rd(pb+72,4),playing=rd(pb+76,4),native_path=cstr(pb+352),directory=cstr(pb+416))"""
new=old+"""
                meta=g.hook_symbols['pb_runtime_metadata']
                assert rd(meta,4)==0x50425549 and rd(meta+4,4)==1
                s['pb']['consumed_rec']=rd(pb+rd(meta+33*4,4),4)
                s['pb']['error']=rd(pb+rd(meta+5*4,4),4)"""
assert s.count(old)==1;s=s.replace(old,new)
old="""class Peer(probe.CapturePeer):
    def __init__(self,*args,**kw):super().__init__(*args,**kw);self.trace_points={}
probe.CapturePeer=Peer"""
new="""class Peer(probe.CapturePeer):
    def __init__(self,*args,**kw):
        super().__init__(*args,**kw);self.trace_points={};self.rec_before=None
    def begin(self,label,body,expected):
        if label=='pb-play-trigger':
            info=snapshot(self.g,'rec-ownership-before')
            pb=info.get('pb',{})
            assert pb.get('cache_valid')==1 and pb.get('lookup_error')==0 and pb.get('native_path'), 'No verified Live photo owner for REC'
            assert body[9]==1 and int.from_bytes(expected[4:8],'little')==0x2000
            self.rec_before=pb['consumed_rec']
            # pb_key_decode_wrap deliberately consumes REC before original key consumer.
            # Retain an exact 44-byte check and verify the owning hook counter on return.
            expected=expected[:4]+bytes(4)+expected[8:]
            self.g.data['rec_owned_expectation']={'before':self.rec_before,'expected_hex':expected.hex(),'body_hex':body.hex()}
        return super().begin(label,body,expected)
    def handle(self):
        active=self.active_contact
        result=super().handle()
        if active and active['label']=='pb-play-trigger' and self.active_contact is None:
            info=snapshot(self.g,'rec-ownership-after');pb=info.get('pb',{})
            assert active['entered'] and pb.get('consumed_rec',0)==self.rec_before+1, 'REC was not consumed exactly once by playback owner'
            self.g.data['rec_owned_expectation']['after']=pb['consumed_rec']
        return result
probe.CapturePeer=Peer"""
assert s.count(old)==1;s=s.replace(old,new)
old="""        key('pb-delete-prompt','delete',2)
        key('pb-delete-confirm','delete',8)
        shutter('pb-delete-complete',gap=5)
        key('pb-return-shooting','playback',4)
        shutter('pb-shooting',gap=2)"""
new="""        shutter('pb-completion-wait',gap=8)
        shutter('pb-return-half',True,gap=.5)
        shutter('pb-return-release',gap=10)
        shutter('pb-recovered',gap=2)"""
assert s.count(old)==1;s=s.replace(old,new)
needle="""                adaptive_contact(g,path.stem,info)
"""
extra="""                adaptive_contact(g,path.stem,info)
                if path.stem.endswith('-pb-return-half'):
                    pb=info.get('pb',{});r=info.get('runtime',{})
                    assert pb.get('frames',0)>0 and pb.get('playing')==0 and pb.get('error')==0 and pb.get('lookup_error')==0, 'Preview did not complete inside bounded wait'
                    assert g.data.get('rec_owned_expectation',{}).get('after') is not None, 'REC ownership was not observed'
                    g.data['pb_recovery_before']={'irq':r.get('capture.irq_seq',0),'encoded':r.get('stream.encoded',0),'preview_frames':pb['frames']}
                if path.stem.endswith('-pb-recovered'):
                    before=g.data['pb_recovery_before'];r=info.get('runtime',{})
                    assert info.get('photo_movie')==0 and r.get('requested')==1, 'Live did not become requested in photo mode'
                    assert r.get('capture.irq_seq',0)>before['irq'] and r.get('stream.encoded',0)>before['encoded'], 'No new Live frames/encoding after half-press return'
                    assert r.get('stream.error')==0 and r.get('stream.job')==0, 'Recovered Live stream has an error or unfinished clip'
                    g.data['pb_recovery_after']={'irq':r['capture.irq_seq'],'encoded':r['stream.encoded'],'phase':r.get('capture.phase'),'preview_frames':info['pb']['frames']}
"""
assert s.count(needle)==1;s=s.replace(needle,extra)
p.write_text(s)
print(p)
