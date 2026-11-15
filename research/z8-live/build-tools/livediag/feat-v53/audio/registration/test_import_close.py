"""Exact-G ARM control flow; filesystem/locks/DCF sink are explicit stubs.
This does not create/register any file in the emulator or camera.
"""
from pathlib import Path
import hashlib, struct, json
from unicorn import *
from unicorn.arm64_const import *
p=Path(__file__).resolve().parent
b=0x40160000; d=(p.parents[1]/'native-rec/v52g.main').read_bytes()
sha=hashlib.sha256(d).hexdigest();assert sha=='ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613'
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(b,(len(d)+4095)&~4095);u.mem_write(b,d)
u.mem_map(0x4e000000,0x10000);u.mem_map(0xa0481f000,0x2000)
end,stk,obj,src,dst=0x4e001000,0x4e00f000,0x4e002000,0x4e003000,0x4e003100
r32=lambda a:struct.unpack('<I',u.mem_read(a,4))[0]
r64=lambda a:struct.unpack('<Q',u.mem_read(a,8))[0]
def w32(a,v):u.mem_write(a,struct.pack('<I',v&0xffffffff))
def w64(a,v):u.mem_write(a,struct.pack('<Q',v))
def reg(n):return u.reg_read(UC_ARM64_REG_X0+n)
def ret(v=0):u.reg_write(UC_ARM64_REG_X0,v&0xffffffffffffffff);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
log=[];rename_rc=0;stat_rc=0

def hook(u,pc,sz,unused):
 if pc in (0x4085d348,0x4085d364):ret()
 elif pc==0x4085c018:
  log.append(dict(call='construct_path',mode=reg(1)));ret(dst)
 elif pc==0x402dad58:
  log.append(dict(call='rename',src=reg(0),dst=reg(1),rc=rename_rc));ret(rename_rc)
 elif pc==0x402dadf4:
  log.append(dict(call='stat',path=reg(0),rc=stat_rc))
  if stat_rc==0:
   w32(reg(1)+0x218,12345);w32(reg(1)+0x21c,0)
  ret(stat_rc)
 elif pc==0x408647c4:
  log.append(dict(call='commit_DCF',record=reg(0),size=r64(obj+0x40)));ret(0x04320080)
u.hook_add(UC_HOOK_CODE,hook)
cases=[]
for state,rename_rc,stat_rc in [(2,0,0),(2,-1,-1),(2,-28,-1),(1,0,0)]:
 log.clear();u.mem_write(obj,bytes(0xa0));w32(obj+0xc,-1);w32(obj+0x70,state)
 u.mem_write(src,b'A:\\LIVE\\100NCZ_8\\LIV_9013.MOV\0');u.mem_write(dst,b'A:\\DCIM\\100NCZ_8\\DSC_9014.MOV\0')
 u.reg_write(UC_ARM64_REG_SP,stk);u.reg_write(UC_ARM64_REG_LR,end)
 u.reg_write(UC_ARM64_REG_X0,obj);u.reg_write(UC_ARM64_REG_X1,src);u.reg_write(UC_ARM64_REG_X2,54321)
 u.emu_start(0x4085c2c0,end,count=10000);assert u.reg_read(UC_ARM64_REG_PC)==end
 rc=reg(0)&0xffffffff
 assert rc==0x04320080
 assert sum(x['call']=='commit_DCF' for x in log)==(state==2)
 if state==2 and stat_rc:assert r64(obj+0x40)==54321
 cases.append(dict(initial_state=state,rename_rc=rename_rc,stat_rc=stat_rc,rc=hex(rc),final_state=r32(obj+0x70),calls=list(log)))
(p/'import-close-tests.json').write_text(json.dumps(dict(image_sha256=sha,passed=True,cases=cases,boundary='Unchanged G instructions in c2c0/b430/c524 and record accessors. Locks, path resolution, filesystem rename/stat and final DCF sink mocked. This proves control flow only.'),indent=2)+'\n')
print('PASS: real c2c0 returns success/commits even failed rename+stat; state1 returns success with no move/commit')
