"""Actual G STOP705 -> lower event worker -> PCM111/STOP116 producer order.
Hardware serial stop/counter completion is explicitly shimmed, not proven here.
"""
from pathlib import Path
import struct,json
from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import *
P=Path(__file__).resolve().parent;ns={'__file__':str(P/'test_sound_cancel.py')};exec(compile((P/'test_sound_cancel.py').read_text().split('rows=[]')[0].replace('elif pc==0x40c00bac:queued.append(u.reg_read(UC_ARM64_REG_X1));ret(1)','elif pc==0x40c00bac:pass'),str(P/'test_sound_cancel.py'),'exec'),ns)
u=ns['u'];call=ns['call'];r32=ns['r32'];w32=ns['w32'];r64=ns['r64'];ret=ns['ret'];END=ns['END'];q=[];pending=0;timeline=[];serial=[];dispatch=''
w64=lambda a,v:u.mem_write(a,struct.pack('<Q',v))
u.mem_map(0x0e200000,0x10000);u.reg_write(UC_ARM64_REG_CPACR_EL1,3<<20)
MSG=0x4e008000;SER=0x4e009000;PCM=0x4e005000;STOP=PCM+16
# Additional observer runs after common instrumented heap/event hooks.
def hook(uc,pc,size,data):
 global pending
 if pc==0x40d2bc8c:
  pending|=uc.reg_read(UC_ARM64_REG_W1);timeline.append(['event',uc.reg_read(UC_ARM64_REG_W1)]);ret(0)
 elif pc==0x40d2bd04:
  if not pending:u.reg_write(UC_ARM64_REG_PC,END);u.emu_stop()
  else:w32(uc.reg_read(UC_ARM64_REG_X2),pending);ret(0)
 elif pc==0x40d2bcc8:pending&=~uc.reg_read(UC_ARM64_REG_W1);ret(0)
 elif pc==0x40c00bac:
  msg=uc.reg_read(UC_ARM64_REG_X1);q.append((uc.reg_read(UC_ARM64_REG_X0),msg));ret(0)
 elif pc==0x40c18014:
  assert u.mem_read(0xa02d4c774,1)==b'\1';serial.append('stopped flag before hardware drain');ret(400)
 elif pc==0x40c174b0:serial.append('shared serial disable');ret(0)
 elif pc==PCM:timeline.append(['PCM',uc.reg_read(UC_ARM64_REG_W1)]);ret()
 elif pc==STOP:timeline.append(['STOP_ACK',uc.reg_read(UC_ARM64_REG_W0)]);ret()
u.hook_add(UC_HOOK_CODE,hook)
rows=[]
for initial in (0,8):
 ns['reset']();pending=initial;q.clear();timeline.clear();serial.clear();u.mem_write(MSG,bytes(64));u.mem_write(SER,bytes(256))
 w64(0xa02d4c738,0x8a0000000);w32(0xa02d4c740,9600);w32(0xa02d4c744,5);w64(0xa02d4c758,SER);w32(0xa02d4c788,2)
 w64(0xa02d4c7f0,0x40c845c4);w64(0xa02d4c810,0x40c84714)
 w64(0xa02d4cbf8,PCM);w64(0xa02d4cb38,STOP);w64(0xa02d4cb40,0)
 w32(MSG+32,0x42040705);call(0x40c7cfa4,MSG)
 assert pending==initial|0x30 and u.mem_read(0xa02d4c774,1)==b'\1' and r32(0xa02d4c778)==400
 call(0x40c7dea8);lower=q[:];q.clear();events=[r32(m+32)for task,m in lower];assert events==([0x4204070c]if initial else [])+[0x4204070c,0x4204070f]
 for task,m in lower:assert task==0xa02d4c600;call(0x40c7cfa4,m)
 high=q[:];q.clear();assert [r32(m+32)for task,m in high]==([0x42040111]if initial else [])+[0x42040111,0x42040116]
 for task,m in high:assert task==0xa02d4c988;call(0x40c81ec0,m)
 assert [x for x in timeline if x[0] in ('PCM','STOP_ACK')]==([['PCM',9600]]if initial else [])+[['PCM',400],['STOP_ACK',0x42040108]]
 # Once lower STOP set its flag, a later native ADC completion callback cannot
 # publish another full block. This is a software producer check, not a model
 # of the IRQ already inside its critical section when STOP began.
 before=list(timeline);call(0x40c7e558,0);assert not pending and timeline==before and not q
 rows.append(dict(preexisting_full_block=bool(initial),lower_events=[hex(v)for v in events],high_events=[hex(r32(m+32))for task,m in high],callbacks=[x for x in timeline if x[0]in('PCM','STOP_ACK')],serial=serial[:],post_stop_callback_suppressed=True))
(P/'capture-stop-order.json').write_text(json.dumps(dict(passed=True,cases=rows,boundary='Actual native705 stop flag, event worker, lower70c/70f handlers, high111/116 constructors and handlers execute. OS queue/waits and actual serial drain call18014 are shims. For an already-entered concurrent IRQ, hardware/interrupt ordering still relies on the native serial-stop contract; no physical DMA timing is simulated.',conclusion='The native stop worker orders the final partial PCM before stop116, including a full block pending in the same event batch;116 callback is after those high111 messages. Post-stop native ADC producer sees the stop flag and generates no new event.'),indent=2)+'\n');print('PASS actual native705/event-worker/70c70f/111116 callbacks: full+partial precede STOP_ACK')
