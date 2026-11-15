"""Compare compiled REC profile with original movie UI -> native17 constructor."""
from pathlib import Path
import struct,json,io,subprocess,tempfile,hashlib
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import *
from elftools.elf.elffile import ELFFile
P=Path(__file__).resolve().parent;D=(P.parents[1]/'feat-v50/integration-writer/v51d/q/main.main').read_bytes();B=0x40160000
s=(P/'work/pb-fix37/runtime.c').read_text();body=s[s.index('static void audio_output('):s.index('\n/* One step per worker loop:')].replace('static void','void',1)
src='#include <stdint.h>\n#define FN(a,t) ((t)(uintptr_t)(a))\n#define memcpy __builtin_memcpy\n'+body
with tempfile.TemporaryDirectory(dir=P) as td:
 o=Path(td)/'p.o';subprocess.run(['clang','--target=aarch64-none-elf','-O2','-ffreestanding','-x','c','-c','-o',str(o),'-'],input=src.encode(),check=True)
 e=ELFFile(io.BytesIO(o.read_bytes()));assert e.get_section_by_name('.rela.text') is None;code=e.get_section_by_name('.text').data()
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(B,(len(D)+4095)&~4095);u.mem_write(B,D)
for a,n in [(0x47000000,0x10000),(0x4e000000,0x40000),(0xa03386000,0x1000),(0xa0282c000,0x1000),(0xa038fe000,0x1000),(0xa03d47000,0x1000),(0xa02a97000,0x1000)]:u.mem_map(a,n)
u.mem_write(0x47000000,code);u.mem_write(0xa03d476d1,b'\x01');u.mem_write(0xa02a97e60,struct.pack('<Q',0x410155f8));END=0x4e001000;OUT=0x4e002000;STACK=0x4e03f000;observed=[]
def w32(a,v):u.mem_write(a,struct.pack('<I',v))
def r64(a):return struct.unpack('<Q',u.mem_read(a,8))[0]
def ret(v=1):u.reg_write(UC_ARM64_REG_W0,v);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
def hook(uc,pc,size,data):
 if pc==0x40d233d0:
  m=u.reg_read(UC_ARM64_REG_X0);p=r64(m+40);observed.append(bytes(u.mem_read(p,28)));ret()
 elif pc in (0x40d2bd04,0x40d2bcc8):ret()
u.hook_add(UC_HOOK_CODE,hook)
def call(pc,arg):
 u.reg_write(UC_ARM64_REG_SP,STACK);u.reg_write(UC_ARM64_REG_LR,END);u.reg_write(UC_ARM64_REG_X0,arg);u.emu_start(pc,END,count=10000);assert u.reg_read(UC_ARM64_REG_PC)==END
cases=[]
for v in (0,1,8,15,20,36):
 for a in (0,1,2,5):
  for b in (0,1,2,5):
   observed.clear();u.mem_write(0xa03d47afc,struct.pack('<H',v));w32(0xa0282c4a0+16,a);w32(0xa0282c4a0+24,b);w32(0xa033869ac,b);u.mem_write(0xa033868d0,struct.pack('<H',v))
   call(0x4066967c,v);assert len(observed)==1
   u.mem_write(0xa033868d0,bytes(2));u.mem_write(OUT,bytes(32));call(0x47000000,OUT);actual=bytes(u.mem_read(OUT,28));assert actual==observed[0],(v,a,b,actual.hex(),observed[0].hex())
   cases.append(dict(volume=v,movie_ui_cache=0,connection4=a,connection6=b,payload=actual.hex()))
report=dict(passed=True,cases=len(cases),actual_native_constructor='4066967c -> 403f7e64 -> 40e35828 -> 43180017',candidate_function='actual audio_output compiled AArch64 O2',candidate_sha256=hashlib.sha256(code).hexdigest(),data=cases,boundaries=['Native queues/event waits are observation stubs; original payload builder and state lookup execute unchanged.','Actual persistent getter 403ec398 -> 40951228 -> 410d5ec8 -> 41012c84 -> 4101553c executes with original ROM table and u16 a03d47afc; group1 ready and type2 reader RAM dispatch seeded as initialized runtime.', 'Cold movie UI cache is zero for every candidate call; pref0 remains mute and nonzero prefs survive.','Current connection6 queried directly instead of relying on the UI cached copy.'])
(P/'profile-tests.json').write_text(json.dumps(report,indent=2)+'\n');print('PASS',len(cases),'compiled candidate payloads exactly match original movie UI constructor')
