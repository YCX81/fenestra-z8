"""Private exact-F native test: explicit PCM device input, read-only runtime receipts.
No guest RAM/reg edits, forced callbacks, fake Ready or widened MMIO tolerances.
"""
import sys,json,hashlib,runpy,os,struct,math
from pathlib import Path
D=Path(__file__).resolve().parent;T=Path('/path/to/Nikon/rdwing-z8-qemu/tools');sys.path.insert(0,str(T))
expected='b0cd7c40c3c6ffdf07b87fcbab20b98fbe9ed0c2a848550855d989b9e64fd8b0'
fw=Path(sys.argv[sys.argv.index('--firmware')+1]).resolve();assert hashlib.sha256(fw.read_bytes()).hexdigest()==expected
import runtime
source_manifest=json.loads((runtime.ROOT/'source-manifest.json').read_text())
def verify(path):
 path=path.resolve();assert path==fw and hashlib.sha256(path.read_bytes()).hexdigest()==expected
 return path,source_manifest
runtime.verify=verify
import full_guest
full_guest.MAIN_SHA=expected
init0=full_guest.Guest.__init__;enter0=full_guest.Guest.__enter__;exit0=full_guest.Guest.__exit__
PCM=os.environ.get('AUDIO_PCM')=='1';SHOOT=os.environ.get('AUDIO_SHOOT')=='1'
OFF=json.loads((D/'runtime-offsets.json').read_text());WIDTH=json.loads((D/'runtime-widths.json').read_text());assert OFF['struct_bytes']==23744
fixture=(D/'input.s16le').read_bytes();assert len(fixture)%9600==0

def snap(g):
 rd=lambda a,n:int.from_bytes(g.read_bytes(a,n),'little')
 valid=lambda a:0xa00000000<=a<0xa40000000
 a=rd(0xa032b50d8,8)
 for o in (663*96+80,2528,8,8):
  if not valid(a):return {}
  a=rd(a+o,8)
 if not valid(a):return {}
 b=g.read_bytes(a,23744)
 if struct.unpack_from('<II',b)!=(0x4c415231,23744):return {}
 keys=['poll_count','capture.phase','capture.irq_seq','capture.irq_timestamp','stream.encoded','stream.job','stream.error','writer_returns','jpeg_returns','transaction_count','transaction_error','audio.active','audio.cancelled','audio_z8.phase','audio_z8.unmuted','audio_z8.blocks','audio_z8.head','audio_z8.tail','audio_z8.dropped','audio_z8.frames','audio_z8.stop_ack','audio_z8.stop_timeouts','audio_warm','audio_error','audio_starts','audio_tracks','audio.first_index','audio.next_index','shutter.phase','shutter.captured','shutter.played','shutter.completed','shutter.cancelled','shutter.errors']
 r={k:int.from_bytes(b[OFF[k]:OFF[k]+WIDTH[k]],'little')for k in keys};r['stock_mute']=rd(0xa03bc7008,1);r['driver']=rd(0xa02d4cb48,4);return r

def init(self,*args,**kwargs):
 init0(self,*args,**kwargs)
 self.command += ['-plugin',f'{D}/observer.so,{self.out}/messages.jsonl']
 # One opt-in private model flag; exact name frozen with model patch.
 prop=os.environ['CODEC_PEER_PROPERTY'];assert '=' in prop
 self.command[self.command.index('-M')+1]+=','+prop
 self.data['audio_fixture']={'pcm_enabled':PCM,'capture_contacts':SHOOT,'pcm_sha256':hashlib.sha256(fixture).hexdigest(),'main_sha256':expected,'no_guest_ram_writes':True,'physical_audio_claim':False}
full_guest.Guest.__init__=init

def enter(self):
 g=enter0(self);original=g.qmp.command;due=None;index=0;last_ready=False;last_sample=-1;shot=None;phase=0
 def command(name,**kw):
  nonlocal due,index,last_ready,last_sample,shot,phase
  result=original(name,**kw)
  if name!='query-status' or not result.get('running'):return result
  if (g.out/'stop.request').exists():raise RuntimeError('Owned test stop requested after completedQMP command')
  now=json.loads(original('qom-get',path='/machine',property='runtime-clock-state'))['virtual_ns']
  if PCM:
   state=json.loads(original('qom-get',path='/machine',property='serial5-pcm16-state'))
   if state['ready']:
    if not last_ready:due=now+50000000
    if now>=due:
     assert state['words']==2400
     block=fixture[(index*9600)%len(fixture):((index*9600)%len(fixture))+9600];assert len(block)==9600
     original('qom-set',path='/machine',property='serial5-pcm16-input',value=f"{state['phase']}:{state['local']:x}:960:{block.hex()}")
     row=dict(virtual_ns=now,index=index,state=state,source_block_sha256=hashlib.sha256(block).hexdigest(),late_ns=now-due)
     with (g.out/'pcm-input.jsonl').open('a')as f:f.write(json.dumps(row)+'\n')
     index+=1;due+=50000000
     if now-due>=50000000:raise RuntimeError('PCM feeder missed a whole50ms block; no fake continuous capture')
   else:due=None
   last_ready=state['ready']
  if now//1000000000!=last_sample:
   last_sample=now//1000000000
   peer=json.loads(original('qom-get',path='/machine',property=os.environ.get('CODEC_PEER_STATE','codec-spi-register-state')))
   with (g.out/'codec-state.jsonl').open('a')as f:f.write(json.dumps(dict(virtual_ns=now,state=peer))+'\n')
   r=snap(g)
   if 12000000000<now<17000000000:
    raw={hex(a):g.read_bytes(a,n).hex()for a,n in [(0xa02813f00,24),(0xa02808b90,0x68),(0xa02c96da8,8),(0xa02d4c908,40)]}
    with (g.out/'codec-raw.jsonl').open('a')as f:f.write(json.dumps(dict(virtual_ns=now,raw=raw))+'\n')
   with (g.out/'audio-runtime.jsonl').open('a')as f:f.write(json.dumps(dict(virtual_ns=now,runtime=r))+'\n')
   if SHOOT and shot is None and now>18000000000 and r.get('audio_warm')==0 and r.get('audio.active')==1:
    shot=now
  if shot is not None:
   actions=[(0,'G1=0','1'),(2000000000,'G13=0','3'),(2150000000,'G13=1','1'),(2500000000,'G1=1','0')]
   if phase<len(actions) and now>=shot+actions[phase][0]:
    _,pins,capture=actions[phase]
    original('qom-set',path='/machine',property='mcu-pins',value=pins)
    original('qom-set',path='/machine',property='capture-gpio-input',value=capture)
    with (g.out/'capture-input.jsonl').open('a')as f:f.write(json.dumps(dict(virtual_ns=now,pins=pins,capture_gpio=capture))+'\n')
    phase+=1
  return result
 g.qmp.command=command;return g
full_guest.Guest.__enter__=enter

def leave(self,*args):
 try:
  (self.out/'codec-spi-register-state.json').write_text(self.qmp.command('qom-get',path='/machine',property='codec-spi-register-state'))
  (self.out/'audio-final.json').write_text(json.dumps(snap(self),indent=2)+'\n')
  for prop in ['serial5-tx-drain-state','serial5-audio-config-state','serial5-pcm-state','serial5-pcm16-state',os.environ.get('CODEC_PEER_STATE','codec-spi-register-state')]:
   try:(self.out/(prop+'.json')).write_text(self.qmp.command('qom-get',path='/machine',property=prop))
   except Exception as e:(self.out/(prop+'-error.txt')).write_text(repr(e))
 except Exception as error:(self.out/'audio-export-error.txt').write_text(repr(error))
 return exit0(self,*args)
full_guest.Guest.__exit__=leave
sys.argv[0]=str(T/'run_native.py');runpy.run_path(sys.argv[0],run_name='__main__')
