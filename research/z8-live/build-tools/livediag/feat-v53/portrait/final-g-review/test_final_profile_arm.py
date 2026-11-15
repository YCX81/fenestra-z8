"""Execute final MAIN cross-bank profile BL and boot allocation edge; no guest device I/O."""
from pathlib import Path
import struct,json,hashlib
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import *
P=Path(__file__).resolve().parent;D=(P/'main.main').read_bytes();R=json.loads((P/'binary-audit.json').read_text());assert hashlib.sha256(D).hexdigest()==R['main_sha256'];B=0x40160000;PROFILE=int(R['profile_export'],16);CALL=int(R['pb_profile_call_sites'][0],16)
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(B,(len(D)+4095)&~4095);u.mem_write(B,D)
for a,n in [(0x4e000000,0x40000),(0xa03386000,0x1000),(0xa0282c000,0x1000),(0xa038fe000,0x1000),(0xa03d47000,0x1000),(0xa02a97000,0x1000)]:u.mem_map(a,n)
u.mem_write(0xa03d476d1,b'\x01');u.mem_write(0xa02a97e60,struct.pack('<Q',0x410155f8));END=0x4e001000;OUT=0x4e002000;STACK=0x4e03f000;observed=[];entries=[];prefs=[];boot=False;alloc=[]
bootcode=R['boot_allocation_disassembly'];BOOT=int(bootcode[0][0],16);BOOTGET=int(next(x[2][1:]for x in bootcode if x[1]=='bl'),16)
def w32(a,v):u.mem_write(a,struct.pack('<I',v))
def r64(a):return struct.unpack('<Q',u.mem_read(a,8))[0]
def ret(v=1):u.reg_write(UC_ARM64_REG_X0,v);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
def hook(uc,pc,size,data):
 if boot and pc==BOOTGET:ret(0)
 elif boot and pc==0x40d2c408:alloc.append([u.reg_read(UC_ARM64_REG_X0),u.reg_read(UC_ARM64_REG_X1)]);ret(0)
 elif pc==PROFILE:entries.append(pc)
 elif pc==0x403ec398:prefs.append(u.reg_read(UC_ARM64_REG_X0))
 elif pc==0x40d233d0:
  m=u.reg_read(UC_ARM64_REG_X0);p=r64(m+40);observed.append(bytes(u.mem_read(p,28)));ret()
 elif pc in (0x40d2bd04,0x40d2bcc8):ret()
u.hook_add(UC_HOOK_CODE,hook)
def call(pc,arg,end=END):
 u.reg_write(UC_ARM64_REG_SP,STACK);u.reg_write(UC_ARM64_REG_LR,end);u.reg_write(UC_ARM64_REG_X0,arg);u.emu_start(pc,end,count=10000);assert u.reg_read(UC_ARM64_REG_PC)==end
cases=[]
for v in (0,1,8,15,20,36):
 for a in (0,1,2,5):
  for b in (0,1,2,5):
   observed.clear();u.mem_write(0xa03d47afc,struct.pack('<H',v));w32(0xa0282c4a0+16,a);w32(0xa0282c4a0+24,b);w32(0xa033869ac,b);u.mem_write(0xa033868d0,struct.pack('<H',v));call(0x4066967c,v);assert len(observed)==1
   # Caller clears payload; adjacent canaries and preserved AArch64 registers must survive the relocated BL.
   u.mem_write(0xa033868d0,bytes(2));u.mem_write(OUT-16,b'\xa5'*64);u.mem_write(OUT,bytes(28));entries.clear();prefs.clear()
   regs=[UC_ARM64_REG_X19,UC_ARM64_REG_X20,UC_ARM64_REG_X21,UC_ARM64_REG_X22,UC_ARM64_REG_X23,UC_ARM64_REG_X24,UC_ARM64_REG_X25,UC_ARM64_REG_X26,UC_ARM64_REG_X27,UC_ARM64_REG_X28,UC_ARM64_REG_X29]
   for i,reg in enumerate(regs):u.reg_write(reg,0x1234567000+i)
   call(CALL,OUT,CALL+4);actual=bytes(u.mem_read(OUT,28));assert actual==observed[0],(v,a,b,actual.hex(),observed[0].hex());assert entries==[PROFILE]and prefs==[0x02000110]
   assert bytes(u.mem_read(OUT-16,16))==b'\xa5'*16 and bytes(u.mem_read(OUT+28,20))==b'\xa5'*20
   assert all(u.reg_read(reg)==0x1234567000+i for i,reg in enumerate(regs));assert u.reg_read(UC_ARM64_REG_SP)==STACK
   cases.append(dict(volume=v,cold_cache=0,connection4=a,connection6=b,payload=actual.hex()))
boot=True;call(BOOT,0);assert alloc==[[23744,0]]
report=dict(passed=True,main_sha256=hashlib.sha256(D).hexdigest(),pb_bl=hex(CALL),live_target=hex(PROFILE),profile_cases=len(cases),direct_branch_and_return_verified=True,abi_registers_and_buffer_guards=True,boot_allocation_actual_args=alloc[0],cases=cases,boundary='Final linked MAIN instructions execute. Native state getter, constructor and memcpy execute; original queue/wait leaves are observation stubs, boot lookup/allocator are failed-allocation stubs. No hardware playback, physical allocation or acoustic claim.')
(P/'final-profile-arm.json').write_text(json.dumps(report,indent=2)+'\n');print('PASS final PB BL->Live profile96 cases/canaries and actual boot allocate23744')
