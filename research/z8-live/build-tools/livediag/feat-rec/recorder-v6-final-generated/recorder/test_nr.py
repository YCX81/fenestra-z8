#!/usr/bin/env python3
"""Production linked AArch64 NR guards under Unicorn; native calls stubbed.
No QEMU, device, firmware boot, or MMIO execution.
"""
import io,json,struct,sys,hashlib
from pathlib import Path
from capstone import Cs,CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn import arm64_const as r
import arm
BASE=0x40160000

def main(link):
 root=Path(__file__).resolve().parents[2];base=(root/'bg34.main').read_bytes()
 data=(link/'live-fixed.emulation-only.main').read_bytes()
 elf=ELFFile(io.BytesIO((link/'payload.elf').read_bytes()))
 sy={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
 d=Cs(CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN)
 # No patches anywhere in stock EXIT/NR reset/deassert; all nine stock IDs remain.
 for a,n in [(0x402f3084,0x1d8),(0x40c2d884,0x90),(0x402f2ee8,0x6c)]:
  assert data[a-BASE:a-BASE+n]==base[a-BASE:a-BASE+n]
 # Independently verify every relocated stock instruction/target in the prefix.
 for i in d.disasm(base[0x402f3084-BASE:0x402f3194-BASE],0x402f3084):
  a=sy[f'fr_clone_{i.address:x}'];j=next(d.disasm(data[a-BASE:a-BASE+4],a))
  if i.address in (0x402f3184,0x402f3190):continue
  if i.mnemonic=='bl':assert j.mnemonic=='bl' and j.op_str==i.op_str
  elif i.mnemonic=='cbz':assert j.op_str=='w0, #'+hex(sy['fr_clone_402f30b0'])
  else:assert j.bytes==i.bytes
 # The original 3 gate bodies are byte-identical including local branches.
 old=ELFFile(io.BytesIO((root/'recorder-build/linked-lamp-v4/objects/hooks.S.o').read_bytes()))
 obj=ELFFile(io.BytesIO((link/'objects/hooks.S.o').read_bytes()))
 def gates(e):
  ss={s.name:s['st_value'] for s in e.get_section_by_name('.symtab').iter_symbols()};b=e.get_section_by_name('.text').data()
  return [b[ss[n]:ss[n]+496] for n in ('live_w5_bringup_entry','fr_entry_2','fr_entry_3')]
 assert gates(old)==gates(obj)
 state,sp,done=0x200000,0x310000,0x500000
 xregs=[getattr(r,f'UC_ARM64_REG_X{k}') for k in range(31)]
 qregs=[getattr(r,f'UC_ARM64_REG_Q{k}') for k in range(32)]
 native={int(i.op_str[1:],16) for i in d.disasm(base[0x402f3084-BASE:0x402f325c-BASE],0x402f3084) if i.mnemonic in ('bl','b')}
 def machine(armed=1,skip=1,active=0):
  u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x40000000,0x2000000);u.mem_write(0x401e2000,data[0x401e2000-BASE:0x401fe000-BASE]);u.mem_write(0x402f3084,data[0x402f3084-BASE:0x402f325c-BASE])
  u.mem_map(state,0x10000);u.mem_map(0x300000,0x20000);u.mem_map(done,0x1000);u.mem_map(0xa02e98000,0x1000)
  u.mem_write(state,struct.pack('<I',armed));u.mem_write(state+100,struct.pack('<I',skip));u.mem_write(state+88,struct.pack('<Q',active))
  for k,reg in enumerate(xregs):u.reg_write(reg,0x12340000+k)
  for k,reg in enumerate(qregs):u.reg_write(reg,(1<<100)+k)
  u.reg_write(r.UC_ARM64_REG_SP,sp);u.reg_write(r.UC_ARM64_REG_LR,done)
  return u
 def mock(u,a,n,_):
  if a in native:
   if a==0x40c2d884 and expected_at_call is not None:
    actual=[u.reg_read(v) for v in checked_regs]
    # BL itself changes LR; every other live register must be fully restored.
    assert actual[:30]+actual[31:]==expected_at_call[:30]+expected_at_call[31:]
   calls.append((a,u.reg_read(r.UC_ARM64_REG_X1)))
   u.reg_write(r.UC_ARM64_REG_X0,0);u.reg_write(r.UC_ARM64_REG_PC,u.reg_read(r.UC_ARM64_REG_LR))
 # Both real guard bodies: wrong arm/flag/SP/full LR/stack LR/ID fail closed.
 cases=0
 for id_,site,lrname,nextsite in [(0x4a,0x402f3184,'fr_nr_lr_a',0x402f3188),(0x4b,0x402f3190,'fr_nr_lr_b',None)]:
  for fault in ('none','unarmed','off','invalid_flag','sp','lr','stack_lr','id','no_owner'):
   for flags in range(16):
    u=machine(armed=fault!='unarmed',skip=0 if fault=='off' else 2 if fault=='invalid_flag' else 1,active=0 if fault=='no_owner' else sp+16 if fault=='sp' else sp)
    u.reg_write(r.UC_ARM64_REG_X22,state);u.reg_write(r.UC_ARM64_REG_X1,id_+1 if fault=='id' else id_)
    u.reg_write(r.UC_ARM64_REG_LR,sy[lrname]+(1<<32 if fault=='lr' else 0))
    u.reg_write(r.UC_ARM64_REG_NZCV,flags<<28);u.reg_write(r.UC_ARM64_REG_FPCR,0x1000000);u.reg_write(r.UC_ARM64_REG_FPSR,0x8000001)
    u.mem_write(sp+24,struct.pack('<Q',sy['fr_exit_return']+(1<<32 if fault=='stack_lr' else 0)))
    regs=xregs+qregs+[r.UC_ARM64_REG_SP,r.UC_ARM64_REG_NZCV,r.UC_ARM64_REG_FPCR,r.UC_ARM64_REG_FPSR]
    before=[u.reg_read(v) for v in regs];expected_at_call=before;checked_regs=regs;calls=[];u.hook_add(UC_HOOK_CODE,mock)
    end=sy[f'fr_clone_{nextsite:x}'] if nextsite else 0x402f3194
    u.emu_start(sy[f'fr_clone_{site:x}'],end,count=2000)
    assert u.reg_read(r.UC_ARM64_REG_PC)==end
    assert len(calls)==(fault!='none'),(id_,fault,calls)
    if fault=='none':
     after=[u.reg_read(v) for v in regs];before[30]=sy['fr_nr_lr_b'] if id_==0x4a else before[30]
     assert after==before,(id_,fault,'register corruption')
     assert struct.unpack('<I',u.mem_read(state+52,4))[0]==1<<(8+id_-0x4a)
    cases+=1
 # Complete scoped prefix + real native tail, including CAS owner release;
 # stock EXIT interleaved on another stack retains all nine reset calls.
 expected_at_call=None
 for mode in ('on','off','unarmed','invalid','contended','native_concurrent'):
  u=machine(armed=mode!='unarmed',skip=0 if mode=='off' else 2 if mode=='invalid' else 1,active=0xdead if mode in ('contended','native_concurrent') else 0)
  u.reg_write(r.UC_ARM64_REG_X0,4 if mode=='native_concurrent' else state)
  saved=[u.reg_read(reg) for reg in xregs[19:]];calls=[];u.hook_add(UC_HOOK_CODE,mock)
  u.emu_start(0x402f3084 if mode=='native_concurrent' else sy['fr_exit_scoped'],done,count=10000)
  assert u.reg_read(r.UC_ARM64_REG_PC)==done
  assert [v for a,v in calls if a==0x40c2d884]==list(range(0x43,0x4a if mode=='on' else 0x4c))
  assert [u.reg_read(reg) for reg in xregs[19:]]==saved
  assert struct.unpack('<Q',u.mem_read(state+88,8))[0]==(0xdead if mode in ('contended','native_concurrent') else 0)
  assert u.reg_read(r.UC_ARM64_REG_SP)==sp
 # arm writes tested solely against RAM bytes; no connector import/connect.
 l=arm.layout(link/'layout.o');ram=bytearray(512);writes=[]
 def read(a,n):return bytes(ram[a:a+n])
 def write(a,b):writes.append((a,b));ram[a:a+len(b)]=b
 for token,skip in [(1,True),(2,False),(0,False)]:
  arm.write_arm(read,write,0,l,token,skip)
  assert struct.unpack_from('<I',ram,0)[0]==token and struct.unpack_from('<I',ram,100)[0]==int(skip)
 for token,skip,offset in [(0,True,None),(1,True,88),(1,True,96),(1,True,16)]:
  ram[:]=bytes(512);writes.clear()
  if offset is not None:ram[offset]=1
  try:arm.write_arm(read,write,0,l,token,skip)
  except ValueError:pass
  else:raise AssertionError('unsafe arm accepted')
  assert not writes
 # Static deassert table: same offset8 and bits23/24, write-clear address.
 table=0x4164c494-BASE
 assert base[table+2*0x4a:table+2*0x4c]==bytes([8,23,8,24])
 report=dict(status='PASS',guard_paths=cases,scoped_paths=6,main_sha256=hashlib.sha256(data).hexdigest(),
  tests='relocated prefix; unchanged stock EXIT/reset/deassert; unchanged lamp gates; actual AArch64 guard and scoped calls; native concurrent owner; arm RAM writes',
  limitation='Unicorn instruction execution with native calls stubbed. No NR MMIO, native task concurrency, QEMU or hardware validation.')
 (link/'nr-audit.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main(Path(sys.argv[1]))
