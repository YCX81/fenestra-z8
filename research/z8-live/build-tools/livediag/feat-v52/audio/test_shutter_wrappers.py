"""Actual diagnostic wrapper instructions, all NZCV values, destructive helper."""
from pathlib import Path
import io,json,random,subprocess,tempfile
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
import unicorn.arm64_const as A
H=Path(__file__).resolve().parent
src=(H/'shutter-work/src-fix37/hooks.S').read_text()
asm=src[:src.index('.global live_irq_entry')]+src[src.index('/* v52 passive wrappers'):]
# All labels in this probe are in one section, so no linker relocation is needed.
asm+='\nlive_shutter_diag: ret\nlive_stock_shutter_prepare: ret\nlive_stock_shutter_fire: ret\n'
with tempfile.TemporaryDirectory(dir=H,prefix='shutter-test-') as td:
 t=Path(td);subprocess.run(['clang','--target=aarch64-none-elf','-x','assembler','-c','-o',str(t/'probe.o'),'-'],input=asm.encode(),check=True)
 e=ELFFile(io.BytesIO((t/'probe.o').read_bytes()));assert not e.get_section_by_name('.rela.live_extra')
 code=e.get_section_by_name('.live_extra').data();symbols={s.name:0x47000000+s['st_value'] for s in e.get_section_by_name('.symtab').iter_symbols()}
 cases=0
 for name,original,point in [('live_shutter_prepare_entry','live_stock_shutter_prepare',1),('live_shutter_fire_entry','live_stock_shutter_fire',2)]:
  for flags in range(16):
   u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x47000000,4096);u.mem_write(0x47000000,code)
   u.mem_map(0x48000000,65536);u.mem_map(0x49000000,4096);u.mem_write(0x49000008,b'\xa5')
   regs=[getattr(A,f'UC_ARM64_REG_X{i}') for i in range(31)];rng=random.Random(cases)
   before=[rng.getrandbits(64) for _ in regs];before[0]=0x49000000 if point==1 else 0x12345678
   for reg,value in zip(regs,before):u.reg_write(reg,value)
   u.reg_write(A.UC_ARM64_REG_SP,0x48010000);u.reg_write(A.UC_ARM64_REG_NZCV,flags<<28);seen=[]
   def trace(cpu,pc,size,unused):
    if pc==symbols['live_shutter_diag']:
     assert cpu.reg_read(regs[0])==point
     assert cpu.reg_read(regs[1])==(0xa5 if point==1 else 0x12345678)
     ret=cpu.reg_read(regs[30]);seen.append('diag')
     for reg in regs:cpu.reg_write(reg,rng.getrandbits(64))
     cpu.reg_write(A.UC_ARM64_REG_NZCV,(flags^15)<<28);cpu.reg_write(A.UC_ARM64_REG_PC,ret)
    elif pc==symbols[original]:seen.append('stock');cpu.emu_stop()
   u.hook_add(UC_HOOK_CODE,trace);u.emu_start(symbols[name],0,count=180)
   assert seen==['diag','stock'] and [u.reg_read(r) for r in regs]==before
   assert u.reg_read(A.UC_ARM64_REG_SP)==0x48010000 and u.reg_read(A.UC_ARM64_REG_NZCV)==flags<<28
   cases+=1
report={'passed':True,'cases':cases,'all_integer_registers_and_nzcv_preserved':True,'original_arguments_and_return_address_preserved':True,'scope':'Assembler output with destructive diagnostic helper; real stock targets verified separately in shutter-sites.json. No physical shutter/audio test.'}
(H/'shutter-wrappers.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
