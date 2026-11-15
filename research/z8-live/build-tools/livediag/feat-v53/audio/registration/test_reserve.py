"""Exact-G public explicit-card reserve + rollback; no real card or filesystem.
Storage/policy/clock boundaries stubbed; do not treat as end-to-end uniqueness.
"""
from pathlib import Path
import hashlib,struct,json
from unicorn import *
from unicorn.arm64_const import *
p=Path(__file__).resolve().parent;b=0x40160000;d=(p.parents[1]/'native-rec/v52g.main').read_bytes();sha=hashlib.sha256(d).hexdigest();assert sha=='ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613'
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(b,(len(d)+4095)&~4095);u.mem_write(b,d)
for a,n in [(0x4e000000,0x10000),(0xa04822000,0x2000),(0xa04841000,0x1000)]:u.mem_map(a,n)
end,stk,flags,date,plan,out=0x4e001000,0x4e00f000,0x4e002000,0x4e002010,0x4e002020,0x4e002080
SLOTS=0xa04822af8;OK=0x04320080;ERR=0x04320089
r32=lambda a:struct.unpack('<I',u.mem_read(a,4))[0]
r64=lambda a:struct.unpack('<Q',u.mem_read(a,8))[0]
def w32(a,v):u.mem_write(a,struct.pack('<I',v&0xffffffff))
def w64(a,v):u.mem_write(a,struct.pack('<Q',v))
def reg(n):return u.reg_read(UC_ARM64_REG_X0+n)
def ret(v=0):u.reg_write(UC_ARM64_REG_X0,v&0xffffffffffffffff);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
log=[];fail_create=0;create_count=0

def hook(u,pc,sz,unused):
 global create_count
 if pc==0x40875834:log.append(dict(call='lock',object=reg(0)));ret()
 elif pc==0x408758b4:log.append(dict(call='unlock',object=reg(0)));ret()
 elif pc==0x4085fef8:u.mem_write(reg(1),bytes(128));ret(OK)
 elif pc==0x4086f00c:ret()
 elif pc==0x4086f210:
  ptr=reg(1);log.append(dict(call='choose_unique_boundary',groups=r32(ptr)))
  for g in range(r32(ptr)):
   for j in range(r32(ptr+4+12*g)):
    a=ptr+8+12*g+4*j;w32(a,r32(a)|(100<<19)|(9014<<5))
  ret(OK)
 elif pc==0x41012d68:
  log.append(dict(call='prefix_setting',key=reg(0)));u.mem_write(reg(1),b'DSC\0');ret()
 elif pc==0x4086f6e4:u.mem_write(reg(0),b'\x5a\x61\x12\x34\x56\x00');ret()
 elif pc==0x4085ff9c:
  create_count+=1
  log.append(dict(call='reserve_index',storage=reg(0),id=hex(reg(2)),size=r64(reg(3)+0x18),prefix=bytes(u.mem_read(reg(3)+0x20,4)).decode(),rc=hex(ERR if fail_create==create_count else OK)))
  ret(ERR if fail_create==create_count else OK)
 elif pc==0x408602cc:log.append(dict(call='associate_type',id=hex(reg(1)),type=r32(reg(2)),size=r64(reg(2)+8)));ret(OK)
 elif pc==0x4086f1d0:ret(0)
 elif pc in (0x4086f560,0x4086f5e0):log.append(dict(call='save_dir' if pc==0x4086f560 else 'save_number',value=reg(0)));ret(OK)
 elif pc==0x408648a8:log.append(dict(call='refresh_counts',mode=reg(0)));ret()
 elif pc==0x40875718:log.append(dict(call='event',id=hex(reg(0))));ret()
 elif pc==0x4086f05c:log.append(dict(call='allocation_policy_update',flag=reg(0)));ret()
 elif pc==0x40861004:log.append(dict(call='rollback_index',storage=reg(0),id=hex(reg(1))));ret(OK)
u.hook_add(UC_HOOK_CODE,hook)
cases=[]
def run(name,card=0,available=True,fail=0,dual=False):
 global fail_create,create_count
 fail_create=fail;create_count=0;log.clear();u.mem_write(0x4e002000,bytes(0x200));u.mem_write(0xa04822cf0,bytes(0x50))
 w64(SLOTS,0x4e004000 if available else 0);w64(SLOTS+8,0x4e004100 if available else 0)
 w32(plan,1);w32(plan+8,5);w64(plan+16,123456);u.mem_write(date,b'\x5a\x61\x12\x34\x56\0');u.mem_write(out+36,b'CANARY!!')
 args=[card,5,flags,date,plan,0,out];entry=0x40866c4c
 if dual:
  w32(out,2)
  for g in range(2):w32(out+4+g*12,1);w32(out+8+g*12,(g<<29)|5)
  args=[5,flags,date,plan,0x4e005000,0x4e005100,out];entry=0x40866800
 u.reg_write(UC_ARM64_REG_SP,stk);u.reg_write(UC_ARM64_REG_LR,end)
 for n,v in enumerate(args):u.reg_write(UC_ARM64_REG_X0+n,v)
 u.emu_start(entry,end,count=30000);assert u.reg_read(UC_ARM64_REG_PC)==end
 assert bytes(u.mem_read(out+36,8))==b'CANARY!!'
 result=dict(name=name,entry=hex(entry),rc=hex(reg(0)),out=list(struct.unpack('<9I',u.mem_read(out,36))),calls=list(log));cases.append(result);return result
for card in (0,1):
 c=run('card'+str(card),card);assert c['rc']==hex(OK) and c['out'][:3]==[1,1,(card<<29)|(100<<19)|(9014<<5)|5];assert any(x.get('key')==0x02001200 for x in c['calls'])
c=run('invalid_card',2);assert c['rc']=='0x4320082' and not c['calls']
c=run('card_missing',0,False);assert c['rc']=='0x4320082' and not c['calls']
c=run('first_reserve_failed',fail=1);assert c['rc']==hex(ERR) and c['out'][0]==0 and not any(x['call']=='rollback_index' for x in c['calls'])
c=run('dual_second_reserve_failed',fail=2,dual=True);assert c['rc']==hex(ERR) and c['out'][0]==0;assert len([x for x in c['calls'] if x['call']=='rollback_index'])==1
(p/'reserve-tests.json').write_text(json.dumps(dict(image_sha256=sha,passed=True,cases=cases,boundary='Public explicit-card validation, inner reserve loops, record/prefix construction and rollback are original ARM. Unique-number chooser, filesystem-index storage, prefs, clock, lock/notifications are mocked; no claim of full allocator/card validation.'),indent=2)+'\n')
print('PASS exact G explicit-card MOV reserve, missing/invalid card, failure rollback and output canary')
