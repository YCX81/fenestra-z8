from pathlib import Path
import struct,json,hashlib
from unicorn import *
from unicorn.arm64_const import *
p=Path(__file__).resolve().parent;f=p.parents[1]/'native-rec/v52g.main';d=f.read_bytes();assert hashlib.sha256(d).hexdigest()=='ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613'
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);b=0x40160000;u.mem_map(b,(len(d)+4095)&~4095);u.mem_write(b,d)
for a,n in [(0x4e000000,0x10000),(0xa02d4c000,0x1000),(0xe200000,0x1000)]:u.mem_map(a,n)
end,stk,obj,state=0x4e001000,0x4e00f000,0x4e002000,0xa02d4c738
records=[];events=[]
def w(a,v,fmt='I'):u.mem_write(a,struct.pack('<'+fmt,v))
def hook(u,pc,sz,_):
 if pc in (0x40c16674,0x40c168c4,0x40d2bc8c):
  if pc==0x40c16674:records.append(bytes(u.mem_read(u.reg_read(UC_ARM64_REG_X1),48)))
  if pc==0x40d2bc8c:events.append(u.reg_read(UC_ARM64_REG_X1))
  u.reg_write(UC_ARM64_REG_X0,0);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
u.hook_add(UC_HOOK_CODE,hook)
def call(pc,x0):
 u.reg_write(UC_ARM64_REG_SP,stk);u.reg_write(UC_ARM64_REG_LR,end);u.reg_write(UC_ARM64_REG_X0,x0);u.emu_start(pc,end,count=20000);assert u.reg_read(UC_ARM64_REG_PC)==end
w(obj+4,0);w(obj+0x38,25);w(obj+0x3c,26)
# Actual native selector: bit0 selects single packed-word vs two32bit word FIFO.
rows=[]
for bit in [0,1]:
 w(0xe200108,0x1000|bit);call(0x40c18160,obj);fifo=u.reg_read(UC_ARM64_REG_X0);assert fifo==0xe200100+(0x38 if bit else 0)
 u.mem_write(state,bytes(0x40));w(state,0x20000000,'Q');w(state+8,9600);w(state+0xc,5);w(state+0x20,obj,'Q');w(state+0x28,fifo)
 records.clear();events.clear()
 call(0x40c7e4ac,0);call(0x40c7e4ac,1)
 for phase in [0,1,0,1,0,1,0,1]:call(0x40c7e558,phase)
 for i,x in enumerate(records):
  request=struct.unpack_from('<I',x,0)[0];words=struct.unpack_from('<H',x,10)[0]+1;src,dst=struct.unpack_from('<II',x,40)
  assert request==25+i%2 and words==2400 and src==fifo and dst==0x20000000+(i%5)*9600
 assert events==[8,1]*8
 before=len(records);w(state+0x3c,1,'B');call(0x40c7e558,0);call(0x40c7e558,1);assert len(records)==before and events==[8,1]*8
 rows.append(dict(config_bit0=bit,fifo=hex(fifo),descriptors=[x.hex()for x in records],producer_events=events,post_stop_no_rearm=True))
(p/'native-rx.json').write_text(json.dumps(dict(passed=True,cases=rows,boundary='Actual original selector18160 and RX configure/refill e4ac/e558 ->17dac/17e5c execute. DMA install/start and kernel eventpost recorded; no hardware transfer/IRQ claimed. Explicit LE16stereo fixture contract follows backend9600B=S16 stereo2400frames, not silicon serial packing proof.'),indent=2)+'\n')
print('PASS original ARM RX FIFO selector and20bank submissions/refills, bothwidths, stopflag blocksnewrearm')
