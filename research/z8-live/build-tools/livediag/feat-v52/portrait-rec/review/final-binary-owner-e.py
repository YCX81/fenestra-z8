"""Independent both-slot/both-codec owner and early-close guard execution."""
from pathlib import Path
import struct,json,hashlib
from unicorn import *
from unicorn.arm64_const import *
D=Path(__file__).resolve().parent
from elftools.elf.elffile import ELFFile
ELF=Path("/path/to/user/v37build/v52e/live/payload.elf")
with ELF.open("rb")as f:
 e=ELFFile(f);ELFSY={s.name:s["st_value"]for s in e.get_section_by_name(".symtab").iter_symbols()};SECTIONS=[(s["sh_addr"],s.data())for s in e.iter_sections()if s["sh_flags"]&2 and s["sh_size"]]
CODE=0x40100000; E=0xa11000000; W=0x89f5ff000; INST=0xa12abc000;STACK=0xa13000000;ARENA=0x8a2e9b000;STOP=0x40200000;RT=0xa14000000
results=[]
def run(codec,slot,mode,mutation,expect):
 name=f'{mode}-codec{codec}';symbols=ELFSY
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
 for a,n in[(0x401e0000,0x20000),(STOP,4096),(E,4096),(W,0x103000),(INST,8192),(STACK,0x10000),(RT,0x10000),(0x401c6000,4096),(0x40c30000,0x10000),(0x402f3000,4096),(0xa02d30000,0x10000),(0xa02e98000,4096)]:u.mem_map(a,n)
 
 for a,blob in SECTIONS:u.mem_write(a,blob)
 def p(k,a,v):u.mem_write(a,struct.pack({'q':'<Q','i':'<I','b':'<B'}[k],v))
 for k,a,v in [('q',E+40,W),('i',W,0x57354c31),('i',W+4,slot),('q',W+48,ARENA),('q',W+1057016,INST),('q',W+1053592,RT),('q',0xa02d3d980,0x16010000),('q',0xa02d3d950,ARENA),('q',0xa02d3d9d8,ARENA+0x200000),('i',0xa02d30118+slot*12,1),('i',0xa02d3274c+slot*4,4),('q',0xa02d32758+slot*8,INST),('i',INST,1),('i',INST+12,1 if codec else 3)]:p(k,a,v)
 u.mem_write(0xa02e98480,b'\xff\xff\xff\xff'+bytes([slot]));arg=E
 if mutation:
  if mutation[0]=='arg':arg=mutation[1]
  else:p(*mutation)
 writes=[];calls=[]
 def hw(uc,a,n,_):
  if a==0x401c6cb4:
   dest,src,count=[uc.reg_read(k)for k in(UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2)];uc.mem_write(dest,bytes(uc.mem_read(src,count)));uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_X30));return
  if a in(0x40c33e34,0x40c34078,0x40c33fb0,0x40c340e8,0x402f3084):calls.append(hex(a));uc.emu_stop()
 u.hook_add(UC_HOOK_CODE,hw)
 u.hook_add(UC_HOOK_MEM_WRITE,lambda uc,access,a,n,v,_:writes.append(a)if not STACK<=a<STACK+0x10000 else None)
 u.reg_write(UC_ARM64_REG_X0,arg);u.reg_write(UC_ARM64_REG_X30,STOP);u.reg_write(UC_ARM64_REG_SP,STACK+0xfff0)
 try:u.emu_start(symbols['live_encoder_'+('retained'if mode=='retained'else'release')],STOP,count=100000)
 except UcError as e:raise RuntimeError((codec,slot,mode,mutation,hex(u.reg_read(UC_ARM64_REG_PC)))) from e
 got=u.reg_read(UC_ARM64_REG_X0)&0xffffffff
 if mode=='retained':assert got==expect and not writes and not calls,(codec,slot,mutation,got,writes,calls)
 elif expect=='allow':assert calls==['0x40c33e34'],(codec,slot,calls)
 else:
  assert got==0xffffffff and not calls,(codec,slot,mutation,got,calls)
  assert struct.unpack('<I',bytes(u.mem_read(W+1053564,4)))[0]==1
  assert struct.unpack('<I',bytes(u.mem_read(W+32,4)))[0]==0
  assert struct.unpack('<I',bytes(u.mem_read(RT+18988,4)))[0]==0
 results.append(dict(codec=codec,slot=slot,mode=mode,mutation=mutation,expected=expect,actual=got,native_calls=calls))
for codec in(1,):
 for slot in(0,1):
  cases=[(None,1),(('arg',0),0),(('q',E+40,0),0),(('i',W,0),0),(('i',W+1053564,1),0),(('i',W+32,1),0),(('i',W+36,1),0),(('q',0xa02d3d980,0),0),(('q',0xa02d3d980,0x17000000),0),(('b',0xa02e98484,255),0),(('b',0xa02e98484,slot^1),0),(('i',W+4,2),0),(('i',0xa02d30118+slot*12,0),0),(('i',0xa02d30118+(slot^1)*12,1),0),(('q',0xa02d3d950,ARENA+4096),0),(('i',0xa02d3274c+slot*4,5),0),(('q',0xa02d32758+slot*8,0),0),(('q',0xa02d32758+slot*8,INST+4096),0),(('q',W+1057016,0),0),(('i',INST,0),0),(('i',INST+12,3 if codec else 1),0)]
  cases += [(('b',0xa02e98480+n,slot),0)for n in range(4)]
  cases += [(('q',0xa02d3d9d8,0),1),(('q',0xa02d3d9d8,ARENA),1)]
  for mutation,expect in cases:run(codec,slot,'retained',mutation,expect)
  for mutation in [None,('q',0xa02d3d950,ARENA+4096),('q',W+1057016,INST+4096),('i',INST+12,3 if codec else 1),('i',INST,0),('i',0xa02d3274c+slot*4,5)]:run(codec,slot,'release',mutation,'allow'if mutation is None else'refuse')
report={'pass':True,'cases':len(results),'owner_cases':54,'early_close_cases':12,'results':results,'elf_sha256':hashlib.sha256(ELF.read_bytes()).hexdigest(),'source_sha256':hashlib.sha256(Path('/path/to/user/v37build/v52e/work/src-fix37/codec/live_w5enc.c').read_bytes()).hexdigest(),'scope':'Exact final v52e linked Live ELF instructions. Correct original arena d950 differs from DMA2 d9d8 by 2MiB. Hardware API stopped before first call; no full EOS/IRQ proof.'}
(D/'final-binary-owner-e.json').write_text(json.dumps(report,indent=2)+'\n');print('PASS',len(results),'actual ARM owner/early-close cases')
