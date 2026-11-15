"""Execute linked lifecycle hook trampolines with adversarial notification stubs."""
import importlib.util,json,random,struct,subprocess
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
import unicorn.arm64_const as A
spec=importlib.util.spec_from_file_location('prior',Path(__file__).with_name('hook_test.py'));prior=importlib.util.module_from_spec(spec);spec.loader.exec_module(prior)
elf=Path('/path/to/user/.local/share/nikon-z8-qemu/fenestra-z8/bundle/live-a/v4/payload.elf');blob=elf.read_bytes()
nm=subprocess.check_output(['llvm-nm','--defined-only',str(elf)],text=True);sym={p[2]:int(p[0],16) for l in nm.splitlines() if len(p:=l.split())==3}
cases=[]
for entry,helper,resume,closed in [('live_start_entry','live_epoch_notify','live_start_resume',False),('live_stop_entry','live_epoch_notify','live_stop_resume',False),('live_hold_entry','live_epoch_notify','live_hold_resume',False),('live_closed_entry','live_closed_notify','live_closed_resume',True)]:
 for flags in range(16):
  rng=random.Random(flags+4096);uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM);pages=set()
  def mp(addr,size):
   for page in range(addr&~4095,(addr+size+4095)&~4095,4096):
    if page not in pages:uc.mem_map(page,4096);pages.add(page)
  for va,size,data in prior.segments(blob):mp(va,size);uc.mem_write(va,data)
  mp(sym[resume],4);mp(0x90000000-65536,65536);mp(0x15431000,4096);mp(0x91000000,4096)
  uc.mem_write(0x15431004,struct.pack('<I',0x12345678));uc.mem_write(0x91000020,b'\xaa\xbb')
  regs=[getattr(A,f'UC_ARM64_REG_X{i}') for i in range(31)];before=[rng.getrandbits(64) for _ in regs]
  before[0]=0x15431000;before[21]=0x91000020;before[29]=0x91000000;before[28]=0x20
  for reg,value in zip(regs,before):uc.reg_write(reg,value)
  uc.reg_write(A.UC_ARM64_REG_SP,0x90000000);uc.reg_write(A.UC_ARM64_REG_NZCV,flags<<28);seen=[]
  def hook(cpu,pc,size,data):
   if pc==sym[helper]:
    seen.append('helper');assert cpu.reg_read(regs[0])==(0x91000020 if closed else 0x15431000)
    if closed:assert bytes(cpu.mem_read(0x91000020,2))==b'\0\0'
    ret=cpu.reg_read(regs[30])
    for reg in regs:cpu.reg_write(reg,rng.getrandbits(64))
    cpu.reg_write(A.UC_ARM64_REG_NZCV,(flags^15)<<28);cpu.reg_write(A.UC_ARM64_REG_PC,ret)
   elif pc==sym[resume]:seen.append('resume');cpu.emu_stop()
  uc.hook_add(UC_HOOK_CODE,hook);uc.emu_start(sym[entry],0,count=300)
  assert seen==['helper','resume']
  after=[uc.reg_read(reg) for reg in regs]
  if not closed:before[8]=0x12345678
  assert before==after and uc.reg_read(A.UC_ARM64_REG_NZCV)==flags<<28 and uc.reg_read(A.UC_ARM64_REG_SP)==0x90000000
  cases.append(entry)
print(json.dumps(dict(passed=True,cases=len(cases),scope='linked trampolines; helper stubbed; original descriptor store checked before notification'),indent=2))
