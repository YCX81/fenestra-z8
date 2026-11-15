"""Original G GO dispatcher and completion ABI; codec/DMA leaf hardware calls mocked."""
from pathlib import Path
import hashlib,struct,json
from unicorn import *
from unicorn.arm64_const import *
p=Path(__file__).resolve().parent;fw=p.parents[1]/'native-rec/v52g.main';d=fw.read_bytes();sha=hashlib.sha256(d).hexdigest();assert sha=='ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613'
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x40160000,(len(d)+4095)&~4095);u.mem_write(0x40160000,d)
u.mem_map(0xa02d4c000,0x2000);u.mem_map(0x4e000000,0x10000)
msg,go,callback,end,ctx=0x4e002000,0x4e002100,0x4e001000,0x4e001100,0x4e002200
state=0xa02d4cb48
r=lambda n:u.reg_read(UC_ARM64_REG_X0+n)
r32=lambda a:struct.unpack('<I',u.mem_read(a,4))[0]
def w32(a,v):u.mem_write(a,struct.pack('<I',v))
def w64(a,v):u.mem_write(a,struct.pack('<Q',v))
def ret(v=0):u.reg_write(UC_ARM64_REG_X0,v);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
events=[];fail=0
leaves=[0x40c80d90,0x40c814ac,0x40c813c4,0x40c8133c,0x40c8115c]
def hook(u,pc,n,user):
 if pc in leaves:
  events.append(dict(call=hex(pc),state=r32(state)));ret(int(pc==fail))
 elif pc==callback:
  events.append(dict(call='callback',event=hex(r(0)),context=hex(r(1)),state=r32(state),payload=bytes(u.mem_read(go,8)).hex()))
  assert r(0)==0x42040106 and r(1)==ctx
  u.mem_write(go+7,b'\x01');ret()
u.hook_add(UC_HOOK_CODE,hook)
cases=[]
for initial,failure in [(4,0),(1,0),(5,0),(4,0x40c814ac),(4,0x40c813c4),(4,0x40c8115c)]:
 fail=failure;events.clear();u.mem_write(0xa02d4c000,bytes(0x2000));u.mem_write(0x4e002000,bytes(0x1000))
 w32(state,initial);u.mem_write(0xa02d4cb14,b'\x01');u.mem_write(go,bytes([0,0,0,0,1,1,1,0]));u.mem_write(go+8,b'CANARY!!')
 w64(msg+0x18,callback);w32(msg+0x20,0x42040106);w64(msg+0x28,go);w64(msg+0x30,ctx)
 u.reg_write(UC_ARM64_REG_X0,msg);u.reg_write(UC_ARM64_REG_SP,0x4e00f000);u.reg_write(UC_ARM64_REG_LR,end)
 u.emu_start(0x40c81ec0,end,count=10000);assert u.reg_read(UC_ARM64_REG_PC)==end
 assert bytes(u.mem_read(go+8,8))==b'CANARY!!' and len([x for x in events if x['call']=='callback'])==1
 cb=events[-1];assert cb['call']=='callback' and cb['payload'][-2:]=='00' # native never writes ACK +7
 ready=(r32(state)==5 and initial==4 and not failure)
 # Existing state5 is rejected; new backend additionally only accepts our queued start from idle.
 assert cb['state']==(5 if initial==4 and not failure else initial)
 cases.append(dict(initial_state=initial,failed_leaf=hex(failure),callback=cb,post_go_state=r32(state),own_go_success=ready,events=list(events)))
(p/'native-go.json').write_text(json.dumps(dict(passed=True,image_sha256=sha,cases=cases,scope='Original dispatcher/GO control flow, payload writes and callback/state ordering; codec/DMA leaves mocked. Does not establish microphone acoustic output.'),indent=2)+'\n')
print('PASS original G GO: completion/state ordering, late error, rejected state, payload +7/canary (6 cases)')
# Original kernel queue insertion: our post wrapper sets the same priority 2
# for GO and STOP. Equal-priority insertion must leave GO ahead of STOP.
head,first,second,control=0x4e004000,0x4e004100,0x4e004200,0x4e004400
fifo=[]
for priority_queue in (False,True):
 u.mem_write(head,bytes(0x1000));w64(head,head);w64(head+8,head)
 for item,kind in ((first,0x42040106),(second,0x42040108)):
  w32(item+8,2);w32(item+0x20,kind)
  u.reg_write(UC_ARM64_REG_X8,item);u.reg_write(UC_ARM64_REG_X9,head);u.reg_write(UC_ARM64_REG_SP,0x4e00f000)
  u.emu_start(0x4017ce90 if priority_queue else 0x4017ce64,0x4017cebc if priority_queue else 0x4017cedc,count=100)
  assert u.reg_read(UC_ARM64_REG_PC)==(0x4017cebc if priority_queue else 0x4017cedc)
 ptr=struct.unpack('<Q',u.mem_read(head+0x18,8))[0];chain=[]
 while ptr:chain.append(r32(ptr+0x20));ptr=struct.unpack('<Q',u.mem_read(ptr,8))[0]
 assert chain==[0x42040106,0x42040108];fifo.append(dict(priority_mode=priority_queue,ids=[hex(x) for x in chain]))
# Run original single-handler consumer loop. STOP handler admission occurs only
# after the synchronous GO callback has returned. Dequeue/wakeup is mocked.
u.mem_write(0xa02d4c000,bytes(0x2000));w32(state,4);u.mem_write(0xa02d4cb14,b'\x01');u.mem_write(go,bytes([0,0,0,0,1,1,1,0]));fail=0;events.clear()
w64(first+0x18,callback);w64(first+0x28,go);w64(first+0x30,ctx);w32(first+0x20,0x42040106)
w32(second+0x20,0x42040108);w64(control+0x48,0x40c81ec0);pending=[first,second];consumed=[]
def consumer_hook(u,pc,n,arg):
 if pc==0x40d2c170:
  assert pending;x=pending.pop(0);consumed.append(r32(x+0x20));w64(r(1),x);ret(1)
 elif pc==0x40c82720:
  assert events[-1]['call']=='callback' and bytes(u.mem_read(go+7,1))==b'\x01';u.emu_stop()
h=u.hook_add(UC_HOOK_CODE,consumer_hook);u.reg_write(UC_ARM64_REG_X0,control);u.reg_write(UC_ARM64_REG_SP,0x4e00f000);u.reg_write(UC_ARM64_REG_LR,end)
u.emu_start(0x40c00b70,end,count=10000);assert u.reg_read(UC_ARM64_REG_PC)==0x40c82720 and consumed==[0x42040106,0x42040108]
report=json.loads((p/'native-go.json').read_text());report.update(equal_priority_original_queue=fifo,original_consumer_go_callback_before_stop=True)
(p/'native-go.json').write_text(json.dumps(report,indent=2)+'\n');print('PASS native equal-priority FIFO and single consumer: old GO completion precedes STOP processing')
