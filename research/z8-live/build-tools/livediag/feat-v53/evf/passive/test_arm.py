from pathlib import Path
import json,struct,hashlib,subprocess
from elftools.elf.elffile import ELFFile
from capstone import Cs,CS_ARCH_ARM64,CS_MODE_ARM
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import *
R=Path('/path/to/user/fxfix/v53-evf/diag');G=Path('/path/to/user/v37build/v52g');S=G/'work/src-fix37'
exec((R/'build_test.py').read_text().split('for name,oldname')[0])
run(['clang-21',*flags,'-c',R/'offsets.c','-o',R/'offsets.o'])
with (R/'offsets.o').open('rb') as f:raw=ELFFile(f).get_section_by_name('.rodata.diag_offsets').data()
names='usize diag display gate loaded shutdown blocked presents present_fail not_current target_lost display_status rsize stream job writer phase press crumbhead crumbs dgate attached surface callbacks error global_overlay pagesize pagedesc setupcount coreslot qsize draws result submits published used drained samples samplesize log loghead'.split()
o=dict(zip(names,struct.unpack('<'+'I'*(len(raw)//4),raw)));assert len(o)==len(names)
syms=json.loads((R/'symbols.json').read_text());fw=Path('/path/to/user/fxfix/v52-root/v52g/q/main.main').read_bytes()
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x40160000,(len(fw)+4095)&~4095);u.mem_write(0x40160000,fw)
with (R/'payload.elf').open('rb') as f:
 e=ELFFile(f);sections=[(s['sh_addr'],s.data()) for s in e.iter_sections() if s['sh_flags']&2 and s['sh_size']]
for a,b in sections:u.mem_write(a,b)
u.mem_map(0xa02800000,0x1000000);u.mem_map(0xa10000000,0x2000000)
U=0xa10001000;RT=0xa10100000;CORE=0xa10200000;REG=CORE+0x2000;PAGES=0xa10300000;VIEW=0xa10400000;END=0x40160000;SP=0xa11ff0000
q=U+o['diag'];d=U+o['display'];OUT=0xa03229eb0;ROOT=0xa0322a340;TWIN=0xa1171b100
X=[globals()['UC_ARM64_REG_X'+str(i)] for i in range(29)]+[UC_ARM64_REG_FP,UC_ARM64_REG_LR]
def w32(a,v):u.mem_write(a,struct.pack('<I',v&0xffffffff))
def w64(a,v):u.mem_write(a,struct.pack('<Q',v))
def rd32(a):return struct.unpack('<I',bytes(u.mem_read(a,4)))[0]
def ret(v=0):u.reg_write(UC_ARM64_REG_X0,v);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
tick=1000000;hz=1000000;status=1;lockdepth=0;locks=0;switch_at_lock=False;logs=[];submits=[];allowed=[]
md=Cs(CS_ARCH_ARM64,CS_MODE_ARM);mrs={}
for a,b in sections:
 for ins in md.disasm(b,a):
  if ins.mnemonic=='mrs' and ('cntpct_el0' in ins.op_str or 'cntfrq_el0' in ins.op_str):
   r,name=ins.op_str.split(', ');mrs[ins.address]=(int(r[1:]),name)
def hook(uc,pc,size,data):
 global lockdepth,locks,switch_at_lock
 if pc in mrs:
  r,name=mrs[pc];uc.reg_write(X[r],tick if name=='cntpct_el0' else hz);uc.reg_write(UC_ARM64_REG_PC,pc+4)
 elif pc==syms['live_status']:ret(status)
 elif pc==0x40d2bfb4:
  assert uc.reg_read(X[0])==0xa032bbd68;lockdepth+=1;locks+=1;assert lockdepth==1
  if switch_at_lock:u.mem_write(OUT,b'\x00');switch_at_lock=False
  ret()
 elif pc==0x40d2bf60:lockdepth-=1;assert lockdepth==0;ret()
 elif pc==0x403efed8:
  submits.append([uc.reg_read(X[i]) for i in range(3)]);ret(0x8877665544332211)
 elif pc==0x401a29c4:ret(0)
 elif pc in [0x40d2c408,0x40d2c448]:raise AssertionError('diagnostic allocated/freed')
def mem(uc,access,addr,size,value,data):
 assert addr>=0x40000000,('MMIO',hex(addr));
u.hook_add(UC_HOOK_CODE,hook);u.hook_add(UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE,mem)
def call(name,*args):
 u.reg_write(UC_ARM64_REG_SP,SP);u.reg_write(UC_ARM64_REG_LR,END)
 for r,v in zip(X,args):u.reg_write(r,v)
 u.emu_start(syms[name] if isinstance(name,str) else name,END,count=100000)
 assert u.reg_read(UC_ARM64_REG_PC)==END,(name,hex(u.reg_read(UC_ARM64_REG_PC)));assert lockdepth==0
 return u.reg_read(X[0])
def reset():
 global tick,hz,status,logs,submits,locks,switch_at_lock
 u.mem_write(U,bytes(o['usize']));u.mem_write(RT,bytes(o['rsize']));w32(U,0x4c50554b)
 w64(0xa032b50d8,PAGES);w64(PAGES+663*o['pagesize']+o['pagedesc'],CORE)
 w32(CORE+64,0x465a394d);w32(CORE+68,1);w32(CORE+o['setupcount'],46);w64(CORE+o['coreslot'],REG)
 u.mem_write(REG,struct.pack('<IIQ',0x47525846,64,U));w64(d+o['dgate'],U+o['gate'])
 w64(0xa032280c8,VIEW);w32(VIEW+0x54,3);w32(VIEW+0xd0,1)
 w32(ROOT,4);u.mem_write(ROOT+0x108,struct.pack('<III',1,4,4));u.mem_write(OUT,b'\x00');w32(TWIN+4,0)
 desc=call(0x4033d8cc,1,4,4);assert 0xa02800000<=desc<0xa03000000
 u.mem_write(desc+8,struct.pack('<HH',720,480));w64(desc+0x28,0xa10500000);w32(desc+0x3c,3);u.mem_write(desc+0x4c,b'\x01')
 tick=1000000;hz=1000000;status=1;logs=[];submits=[];locks=0;switch_at_lock=False
 assert call('ui')==U
 return desc
def sample():call('live_ui_evf_sample',U)
def flush():
 before=rd32(RT+o['loghead']);call('live_ui_evf_flush',RT);after=rd32(RT+o['loghead'])
 for i in range(before,after):logs.append(list(struct.unpack('<QIIQ',bytes(u.mem_read(RT+o['log']+(i%128)*24,24)))))
checks=[]
def ok(name):checks.append(name)
# Real lookup, ABI pass-through, and no UI object still forwards stock.
reset();assert call('live_ui_evf_submit',0xa10500000,5,4)==0x8877665544332211;assert submits==[[0xa10500000,5,4]];assert rd32(q+o['submits'])==1
w64(0xa032b50d8,0);assert call('live_ui_evf_submit',7,2,1)==0x8877665544332211;assert submits[-1]==[7,2,1];ok('submit all args/return exact, null registry forwards')
# Gate closed and is_current false preserve original return and readers while counting real callback entries.
reset();assert call('icon32_shoot_draw',d,0)==1;assert rd32(q+o['draws'])==1;assert rd32(q+o['result'])==0xffffffff
w64(U+o['gate'],1);assert call('icon32_shoot_draw',d,0)==1;assert rd32(q+o['draws'])==2;ok('actual draw callback two skip paths unchanged')
# Bounded 3 EVF + return sample, no initial LCD, cadence.
reset();sample();assert rd32(q+o['used'])==0 and locks==0
u.mem_write(OUT,b'\x01');sample();assert rd32(q+o['published'])==1
sample();assert locks==1;tick+=1000000;sample();tick+=1000000;sample();tick+=1000000;sample();assert rd32(q+o['used'])==3 and locks==3
u.mem_write(OUT,b'\x00');sample();assert rd32(q+o['published'])==0x80000004 and locks==4
frozen=bytes(u.mem_read(q+o['samples'],4*o['samplesize']));sample();assert bytes(u.mem_read(q+o['samples'],len(frozen)))==frozen and locks==4
ok('3 EVF + LCD immutable, cadence, exactly four locks')
# Flush eligibility: each busy condition independently prevents any events. All nonbusy phases permit flush.
for key,val in [('job',1),('writer',1),('phase',3),('phase',4),('phase',5),('press',1)]:
 w32(RT+o[key],val);flush();assert not logs and rd32(q+o['drained'])==0;(w32(RT+o[key],0));ok('busy gate '+key+'='+str(val))
w32(TWIN+4,2);flush();assert not logs;w32(TWIN+4,0);ok('twin copying blocks')
u.mem_write(OUT,b'\x01');flush();assert not logs;u.mem_write(OUT,b'\x00');ok('back to EVF blocks flush')
for phase in [0,1,2,6,7]:
 w32(RT+o['phase'],phase);w32(q+o['drained'],0);logs=[];flush();assert len(logs)==32 and rd32(q+o['drained'])==1;flush();assert len(logs)==32;ok('idle phase '+str(phase)+' exactly once')
# Verify every event payload and unchanged 24-byte crumb ABI against published records.
for n in range(4):
 raw=bytes(u.mem_read(q+o['samples']+n*o['samplesize'],o['samplesize']));t,*words=struct.unpack('<Q24I',raw)
 for k in range(8):assert logs[n*8+k]==[t,61+k,words[3*k],words[3*k+1]|words[3*k+2]<<32]
assert bytes(u.mem_read(RT+o['crumbs'],32*24))==b''.join(struct.pack('<QIIQ',*v) for v in logs)
ok('all 32 events payload exact, original crumb ABI')
header=struct.pack('<8I',0x32474c4c,rd32(RT+o['loghead']),tick,0,0,0,0,rd32(RT+o['crumbhead']))
(R/'arm-LLG2.BIN').write_bytes(header+bytes(u.mem_read(RT+o['crumbs'],32*24))+bytes(u.mem_read(RT+o['log'],128*24)))
# Early return completes after one EVF; incomplete publication never flushes.
reset();u.mem_write(OUT,b'\x01');sample();flush();assert not logs;u.mem_write(OUT,b'\x00');sample();assert rd32(q+o['published'])==0x80000002;flush();assert len(logs)==16;ok('early LCD return completes 2 snapshots; no partial flush')
# Switching while acquiring mutex must abort, not publish fake LCD completion.
reset();u.mem_write(OUT,b'\x01');switch_at_lock=True;sample();assert rd32(q+o['used'])==0 and rd32(q+o['published'])==0 and locks==1;ok('output switch during lock aborts observation')
# Foreign/invalid pointers: read fixed root tuple, do not dereference foreign surface or invalid current-view.
reset();w64(d+o['surface'],0x15400000);w64(0xa032280c8,0x15400000);w32(ROOT+0x108,99);u.mem_write(OUT,b'\x01');sample();raw=bytes(u.mem_read(q+o['samples'],o['samplesize']));words=struct.unpack('<Q24I',raw)[1:];assert words[5]==0xffffffff and words[12:15]==(0,0,0);ok('foreign surface/invalid view/root no MMIO or dereference')
# Disabled request and missing clock never consume samples.
for why in ('off','clock'):
 reset();u.mem_write(OUT,b'\x01');status=0 if why=='off' else 1;hz=0 if why=='clock' else 1000000;sample();assert rd32(q+o['used'])==0 and locks==0;ok(why+' consumes no sample')
res=dict(passed=True,checks=checks,count=len(checks),scope='Actual final-linked AArch64 diagnostic and original native RAM descriptor getter. Mutex/clock/status/native submit are controlled boundaries; ring and crumb writes execute actual code. No full QEMU or physical EVF visibility claim.',base_main_sha256=hashlib.sha256(fw).hexdigest(),linked_elf_sha256=hashlib.sha256((R/'payload.elf').read_bytes()).hexdigest(),offsets=o)
(R/'arm-result.json').write_text(json.dumps(res,indent=2)+'\n');print(json.dumps(res,indent=2))
