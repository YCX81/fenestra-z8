from pathlib import Path
import json,struct,hashlib
from elftools.elf.elffile import ELFFile
from capstone import Cs,CS_ARCH_ARM64,CS_MODE_ARM
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import *
P=Path(__file__).resolve().parent;D=(P.parent/'main.main').read_bytes();B=0x40160000
assert hashlib.sha256(D).hexdigest()=='c14894087d5fd03b0124c7d94f6cd521e94def58831a087738145ef91b60fcd2'
with (P.parent/'live/payload.elf').open('rb') as f:sym={s.name:s['st_value']for s in ELFFile(f).get_section_by_name('.symtab').iter_symbols()if s.name.startswith('lp_')}
md=Cs(CS_ARCH_ARM64,CS_MODE_ARM);calls=[]
for i in md.disasm(D[0x40196000-B:0x4019f020-B],0x40196000):
 if i.mnemonic=='bl':
  dest=int(i.op_str[1:],16)
  if dest in sym.values():calls.append(dict(site=hex(i.address),target=hex(dest),name=next(k for k,v in sym.items()if v==dest)))
assert set(c['name']for c in calls)==set(sym)
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(B,(len(D)+4095)&~4095);u.mem_write(B,D);u.mem_map(0x50000000,0x40000)
R=0x50000000;SP=0x50030000;END=0x40197308;now=0;reads=[];hides=[];visits=[];ready_stub=True
def w(a,v,n=4):u.mem_write(a,v.to_bytes(n,'little'))
def r(a,n=4):return int.from_bytes(u.mem_read(a,n),'little')
def ret(v=0):u.reg_write(UC_ARM64_REG_X0,v);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
def hook(uc,pc,size,data):
 visits.append(pc)
 if pc==0x40196748:ret(now)
 elif pc==0x4019803c:reads.append(u.reg_read(UC_ARM64_REG_X1));ret(1)
 elif pc==0x401f6bc8:ret(0)
 elif pc==0x40197d60 and ready_stub:ret(1)
 elif pc==0x4019a298:ret(0)
 elif pc==0x40197940:w(u.reg_read(UC_ARM64_REG_X0),123);ret(1)
 elif pc==0x40197e48:ret(1)
 elif pc==0x40198588:hides.append(True);u.reg_write(UC_ARM64_REG_PC,END)
u.hook_add(UC_HOOK_CODE,hook)
rows=[]
# Actual PB frame instructions and actual linked reader; file/JPEG/GUI leaves are deterministic observations.
for delta,index in [(0,0),(71182,0),(71183,1),(44*71183,44),(45*71183,None)]:
 u.mem_write(R,bytes(0x3000));u.mem_write(SP,bytes(0x1000));w(R+0x14,7);w(R+0x34,123);w(R+0x50,0xffffffff);w(R+0x1e8,100000000,8)
 u.mem_write(R+0x200,struct.pack('<IIIII4xQ',1024,682,45,1024*682*3//2,45*71183,7));w(R+0x23c8,1);w(R+0x1fc8,0x50020000,8);w(R+0x2444,4);w(R+0x244c,614000)
 w(R+0x2458,100000000,8);w(SP+0x40,R+0x200,8);w(SP+0x10,R+0x2100,8)
 for i in range(46):w(R+0x2100+i*8,1000+i*100,8)
 now=100000000+delta;reads.clear();hides.clear();visits.clear();u.reg_write(UC_ARM64_REG_X19,R);u.reg_write(UC_ARM64_REG_X21,1);u.reg_write(UC_ARM64_REG_SP,SP);u.emu_start(0x40197118,END,count=20000)
 assert u.reg_read(UC_ARM64_REG_PC)==END
 if index is not None:assert r(R+0x48)==1 and r(R+0x50)==index and not hides,(delta,r(R+0x48),r(R+0x50))
 else:assert hides and not reads
 rows.append(dict(elapsed_us=delta,expected_index=index,actual_last_frame=r(R+0x50),frames=r(R+0x48),hide=bool(hides),read_at=reads.copy()))
# Discriminating actual-G gate case: an unconsumed first request prevents further publication.
# This is a conditional mechanism test, not an assertion that the failing run had request=1.
u.mem_write(R,bytes(0x3000));u.mem_write(SP,bytes(0x1000));w(R+0x14,7);w(R+0x48,1);w(R+0x4c,1);w(R+0x50,0);w(R+0x1e8,100000000,8)
u.mem_write(R+0x200,struct.pack('<IIIII4xQ',1024,682,45,1024*682*3//2,45*71183,7));w(R+0x23c8,1);w(R+0x2004,1);w(SP+0x40,R+0x200,8);w(SP+0x10,R+0x2100,8)
for i in range(46):w(R+0x2100+i*8,1000+i*100,8)
ready_stub=False;now=100000000+71183;hides.clear();u.reg_write(UC_ARM64_REG_X19,R);u.reg_write(UC_ARM64_REG_SP,SP);u.emu_start(0x40197118,END,count=20000)
assert r(R+0x48)==1 and r(R+0x50)==0 and r(R+0x4c)==1 and r(R+0x2004)==1 and not hides
blocked_case=dict(input_render_request=1,frames=r(R+0x48),last_frame=r(R+0x50),playing=r(R+0x4c),hide=False,scope='Actual ready_to_write executed; condition is hypothetical until live snapshot confirms it.')
# Execute final inline successful START ACK tail: both epochs must be assigned the same current stamp.
u.mem_write(R,bytes(0x3000));u.reg_write(UC_ARM64_REG_X19,R);u.reg_write(UC_ARM64_REG_X22,123456789);u.emu_start(0x40197644,0x40197418,count=30)
assert r(R+0x2444)==4 and r(R+0x2458,8)==123456789 and r(R+0x1e8,8)==123456789
out=dict(pass_=True,main_sha256=hashlib.sha256(D).hexdigest(),linked_shared_calls=calls,frame_cases=rows,unconsumed_request_case=blocked_case,start_ack=dict(stage=r(R+0x2444),audio_t=r(R+0x2458,8),start_us=r(R+0x1e8,8)),scope='Actual G PB inline frame/START tail and actual reader execute. Clock, IO/JPEG/GUI leaves are substituted; this excludes branch/index/epoch wiring failures, not GUI scheduling or physical behavior.')
(P/'pb-frame-arm.json').write_text(json.dumps(out,indent=2)+'\n');print('PASS',len(calls),'reader calls,',len(rows),'PB frame cases and START epochs')
