"""Read-only checkpoint diagnostics; observer timestamps are host monotonic ns."""
import mmap,re,struct,time
from pathlib import Path
HEADER=struct.Struct('<12Q');RECORD=struct.Struct('<8Q2I')
KIND={1:'1542-isr',2:'native-lv-entry-patched',3:'native-lv-ack-path',4:'native-lv-time-path',5:'c0-start-hook-site',6:'c0-stop-hook-site',7:'c0-hold-hook-site',8:'c0-start-write',9:'c0-stop-write',10:'c0-hold-write',11:'c0-reset-write',12:'native-stop-chain-40e90d38',13:'native-stop-chain-40e9bb5c',14:'native-plan-40ea0674',15:'class-allocate',16:'class-allocate-return',17:'class-release',18:'native-full-press-hook-site',100:'live_irq_notify',101:'live_epoch_notify',102:'live_press_notify',103:'live_writer_wrap',104:'live_encoder_frame_timed'}
def record_tuple(values):
 seq,host_ns,pc,x0,x1,x2,x3,lr,cpu,kind=values
 return dict(seq=seq,host_ns=host_ns,pc=hex(pc),args=[hex(v) for v in [x0,x1,x2,x3]],lr=hex(lr),cpu=cpu,kind=kind,event=KIND.get(kind,str(kind)))
def observer(path,last=12):
 if not path.exists():return {'unavailable':str(path)}
 with path.open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as mm:
  magic,version,size,cap,head,overwritten,irqs,hooks,errors,*_=HEADER.unpack_from(mm)
  assert magic==0x4c564f4253455256 and size==RECORD.size
  def at(i):return RECORD.unpack_from(mm,HEADER.size+(i%cap)*size)
  recent=[record_tuple(at(i)) for i in range(max(0,head-min(last,cap)),head)]
  irq=None
  for i in range(head-1,max(-1,head-cap-1),-1):
   e=at(i)
   if e[9] in [2,100] and e[3]==0x41070030 and e[4]&1:irq=record_tuple(e);break
  return dict(head=head,overwritten=overwritten,irq_count=irqs,hook_irq_count=hooks,reg_errors=errors,last_completion=irq,recent=recent)
def capture(g,original,info,previous,hook_symbols):
 r=info.get('runtime',{});clock=info.get('runtime-clock-state',{});ns=clock.get('virtual_ns',0)
 seq=r.get('capture.irq_seq',0);label=info['label'];stale=seq and previous.get('seq')==seq and ns-previous.get('changed_ns',ns)>=500000000
 if previous.get('seq')!=seq:previous.update(seq=seq,changed_ns=ns,last_progress_label=label)
 out=dict(host_ns=time.monotonic_ns(),irq_seq=seq,irq_timestamp_us=r.get('capture.irq_timestamp'),stale_over_500ms=bool(stale),last_irq_progress_checkpoint=previous.get('last_progress_label'),observer=observer(g.out/'lv-observer.dat'))
 out['hook_instructions']={}
 for addr,name in [(0x40e90838,'live_irq_entry'),(0x40c26804,'live_start_entry'),(0x40c26818,'live_stop_entry'),(0x40fdc9c8,'live_press_entry')]:
  word=int.from_bytes(g.read_bytes(addr,4),'little');delta=(((word&0x3ffffff)^0x2000000)-0x2000000)*4;target=addr+delta
  out['hook_instructions'][hex(addr)]=dict(word=hex(word),target=hex(target),expected=hex(hook_symbols[name]),matches=(word>>26==5 and target==hook_symbols[name]))
 for prop in ['engine1542-state','engine1543-control-state','primary-d980-state','engine153-frame-pass-state','engine153-inplace-identity-state']:
  try:out[prop]=__import__('json').loads(original('qom-get',path='/machine',property=prop))
  except RuntimeError as e:out[prop]=str(e)
 out['engine153-alternate-bank-model']=original('qom-get',path='/machine',property='engine153-alternate-bank-model')
 out['z8-jpeg-decode']=original('qom-get',path='/machine',property='z8-jpeg-decode')
 assert out['engine153-alternate-bank-model'] is True and out['z8-jpeg-decode'] is True
 if stale or label=='final' or 'full-press' in label:
  try:
   regs=original('human-monitor-command',**{'command-line':'info registers -a'});out['cpu_registers']=regs
   out['cpu_pcs']=re.findall(r'PC=([0-9a-fA-F]+)',regs)
  except RuntimeError as e:out['register_error']=str(e)
  if g.log_path.exists():
   with g.log_path.open('rb') as f:
    f.seek(max(0,g.log_path.stat().st_size-131072));tail=f.read().decode('utf-8','replace')
   selected=[x for x in tail.splitlines() if re.search(r'ENGINE1543_CONTROL|RAW_PIPELINE|150C_STOP|D980|Z8_153|[Ff]ault|[Uu]nknown',x)]
   out['last_model_events']=selected[-20:]
 if g.log_path.exists():out['stderr_bytes']=g.log_path.stat().st_size
 return out
