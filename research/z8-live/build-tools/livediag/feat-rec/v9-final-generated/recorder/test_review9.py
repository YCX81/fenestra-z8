#!/usr/bin/env python3
"""Execute production linked AArch64 with Unicorn; no QEMU/boot/device.
Native service calls, physical time and asynchronous peers are explicit inputs.
No replacement of checkpoint/identity/grace/timeout/decision implementations.
"""
import io,json,struct,subprocess,sys,hashlib
from pathlib import Path
from elftools.elf.elffile import ELFFile
from capstone import Cs,CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_WRITE
from unicorn import arm64_const as r
link=Path(sys.argv[1]).resolve();build=link.parent
elf=ELFFile(io.BytesIO((link/'payload.elf').read_bytes()))
sy={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
code=elf.get_section_by_name('.text');raw=code.data();base=code['sh_addr']
d=Cs(CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN)
ins={i.address:i for i in d.disasm(raw,base)}
# Compiler-produced offsets; don't bake private live_w5 layout into the test.
names=['close_lock','close_failed','fin_rc','frames']
snames=['diag.close_owner','diag.d_start','diag.d_elapsed','diag.packed','diag.i_done']
layout=link/'review9-layout.c'
layout.write_text('#include <stddef.h>\n#include "codec/live_w5enc.c"\nconst unsigned test_offsets[]={'+','.join('offsetof(struct live_w5,'+n+')' for n in names)+','+','.join('offsetof(struct fr_state,'+n+')' for n in snames)+'};\n')
subprocess.run(['clang','--target=aarch64-none-elf','-ffreestanding','-mgeneral-regs-only','-I',str(build),'-I',str(build/'codec/freestanding'),'-I',str(build/'ui/core/include'),'-c',str(layout),'-o',str(link/'review9-layout.o')],check=True)
le=ELFFile(io.BytesIO((link/'review9-layout.o').read_bytes()));sym=next(s for s in le.get_section_by_name('.symtab').iter_symbols() if s.name=='test_offsets');sec=le.get_section(sym['st_shndx']);off=dict(zip(names+snames,struct.unpack('<9I',sec.data()[sym['st_value']:sym['st_value']+36])))
STATE=0x200000;HW=0x600000;ENC=0x220000;TOP=0x320000;REGS=TOP-256;DONE=0x500000;INST=0x230000
WSTATE=0xa02d3274c;VDI=0xa02d3d980;INFO=0xa02d38b98
xr=[getattr(r,f'UC_ARM64_REG_X{i}') for i in range(31)]
class Machine:
 def __init__(self):
  self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u=self.u
  for at,size in [(0x40000000,0x2000000),(0x200000,0x40000),(0x300000,0x40000),(DONE,0x1000),(HW,0x200000),(0xa02d30000,0x10000)]:u.mem_map(at,size)
  for seg in elf.iter_segments():
   if seg['p_type']=='PT_LOAD':u.mem_write(seg['p_vaddr'],seg.data())
  self.time=0;self.points=[];self.emits=[];self.duration=1000000;self.peer=None;self.exit_at=None;self.clear_after=1500000;self.triggers=0;self.close_only=False;self.diag=[]
  self.w32(STATE+off['diag.i_done'],1);self.w32(STATE,1);self.w32(STATE+12,1);self.w32(STATE+52,1);self.w64(STATE+80,TOP);self.w64(REGS+240,0x40d07ba8)
  self.w32(HW,0x57354c31);self.w32(HW+off['frames'],1);self.w64(ENC+40,HW)
  self.w32(WSTATE,4);self.w64(WSTATE+12,INST);self.w32(INST,1);self.w32(INST+12,3);self.w64(VDI,1)
  u.hook_add(UC_HOOK_CODE,self.hook)
  u.hook_add(UC_HOOK_MEM_WRITE,lambda u,ac,a,n,v,_:self.points.append((v>>3,v&7,self.time)),begin=STATE+off['diag.packed'],end=STATE+off['diag.packed']+3)
 def w32(self,a,v):self.u.mem_write(a,struct.pack('<I',v))
 def w64(self,a,v):self.u.mem_write(a,struct.pack('<Q',v))
 def get(self,a,n=4):return int.from_bytes(self.u.mem_read(a,n),'little')
 def ret(self,v=0):self.u.reg_write(xr[0],v);self.u.reg_write(r.UC_ARM64_REG_PC,self.u.reg_read(xr[30]))
 def hook(self,u,a,n,_):
  if a==sy['fr_context']:self.ret(STATE if self.get(STATE) else 0);return
  if a==sy['emit_frame']:
   self.emits.append((u.reg_read(xr[0]),u.reg_read(xr[1]),self.time));self.time+=self.duration;self.update();self.ret(1);return
  if a==sy['w5_diag']:self.diag.append((u.reg_read(xr[1]),self.time));self.ret();return
  if a==sy['alive'] and self.close_only:self.ret(0);return
  if a==0x40187b00:self.time+=1000;self.update();self.ret();return
  if a==0x40c34078:self.ret();return # STOP
  if a==0x40c33fb0:
   self.triggers+=1;self.w32(INFO+8,1);self.w32(INFO+0x60,self.triggers)
   if self.triggers==2:self.w32(WSTATE,5)
   self.ret();return
  if a==0x40c340e8:self.w32(WSTATE,0);self.w32(INST,0);self.ret();return
  if a==sy['fr_live_exit']:self.exit_at=self.time;self.update();self.ret();return
  i=ins.get(a)
  if i and i.mnemonic=='mrs':
   reg,what=i.op_str.split(', ')
   if what in ('cntpct_el0','cntfrq_el0'):
    u.reg_write(xr[int(reg[1:])],self.time if what=='cntpct_el0' else 1000000);u.reg_write(r.UC_ARM64_REG_PC,a+4)
 def update(self):
  if self.peer:self.peer(self)
  if self.exit_at is not None and self.time-self.exit_at>=self.clear_after:self.w64(VDI,0)
 def run(self,name,args,sp=TOP-784):
  for i,v in enumerate(args):self.u.reg_write(xr[i],v)
  self.u.reg_write(r.UC_ARM64_REG_SP,sp);self.u.reg_write(xr[30],DONE)
  self.u.emu_start(sy[name],DONE,count=3000000)
  assert self.u.reg_read(r.UC_ARM64_REG_PC)==DONE,(name,hex(self.u.reg_read(r.UC_ARM64_REG_PC)))
  return self.u.reg_read(xr[0])
rows=[]
# Competing closer runs actual w5_close, all wait/timeout and grace instructions.
for entry,armed,owner,release,mode,expect in [(entry,*case) for entry in ('w5_close','live_encoder_preempt_lamp') for case in [(1,HW,200000,'six',0),(1,HW,6200000,'six',0),(1,HW,None,'six',1),(1,HW,None,'stuck_frame',1),(1,HW,None,'none',1),(0,HW,6200000,'six',1),(1,HW+16,6200000,'six',0)]]:
 m=Machine();m.close_only=True;m.w32(STATE,armed);m.w32(HW+off['close_lock'],1);m.w64(STATE+off['diag.close_owner'],owner)
 def peer(m):
  for k in range(6 if mode=='six' else 1 if mode=='stuck_frame' else 0):
   start=100000+k*1000000
   if m.time>=start:m.w64(STATE+off['diag.d_start']+k*8,start+1)
   if mode=='six' and m.time>=start+1000000:m.w64(STATE+off['diag.d_elapsed']+k*8,1000001)
  if release is not None and m.time>=release:m.w32(HW+off['close_lock'],0)
 m.peer=peer
 if entry=='live_encoder_preempt_lamp':
  def alive_peer(u,a,n,_):
   if a==sy['alive']:m.ret(0 if release is not None and m.time>=release else 1)
  m.u.hook_add(UC_HOOK_CODE,alive_peer)
 m.run(entry,[HW] if entry=='w5_close' else [ENC,0,0,REGS]);failed=m.get(HW+off['close_failed']);assert failed==expect
 bound=11600000 if armed else 4000000
 if expect:assert m.time==bound+1000,(mode,m.time,bound)
 rows.append(dict(test='close_lock',entry=entry,armed=armed,owner_match=owner==HW,release_us=release,mode=mode,time_us=m.time,close_failed=failed,fin_rc=hex(m.get(HW+off['fin_rc']))))
# Same production claim + checkpoint: foreign MOVS cannot emit OR change ownership.
for fault in ['none','movs','other_stack','pointer','session','saved_lr','unarmed','done','returned','no_i']:
 m=Machine();m.w32(STATE+12,0)
 saved=REGS;call_sp=TOP-784
 if fault=='movs':
  assert m.run('fr_before',[REGS])==STATE  # Existing real REC session.
  saved=REGS+0x1000;call_sp=TOP+0x1000-784
  m.w64(saved+240,0x40cf9858)
 accepted=m.run('fr_before',[saved],sp=call_sp);assert bool(accepted)==(fault!='movs')
 if fault=='saved_lr':m.w64(REGS+240,0x140d07ba8)
 if fault=='unarmed':m.w32(STATE,0)
 if fault=='done':m.w32(STATE+24,0x80000000)
 if fault=='returned':m.w32(STATE+28,1<<29)
 if fault=='no_i':m.w32(STATE+off['diag.i_done'],0)
 writes=[]
 m.u.hook_add(UC_HOOK_MEM_WRITE,lambda u,access,a,n,v,_:writes.append((a,n,v)),begin=STATE+28,end=STATE+31)
 before=bytes(m.u.mem_read(STATE+28,4))
 m.run('fr_checkpoint',[STATE+16 if fault=='session' else STATE,saved+16 if fault=='pointer' else saved,17],sp=call_sp-(0x10000 if fault=='other_stack' else 0))
 assert bool(m.points)==(fault=='none') and not m.emits,(fault,m.emits)
 if fault!='none':assert not writes,(fault,writes)
 assert bytes(m.u.mem_read(STATE+28,4))==before
 rows.append(dict(test='identity',fault=fault,fr_before=accepted,emits=m.emits))
# v8 checkpoints take no simulated native time and never fill old D slots.
m=Machine();m.run('fr_close_begin',[STATE,REGS,HW]);m.duration=1370000
m.run('fr_checkpoint',[STATE,REGS,17]);assert m.get(STATE+off['diag.d_elapsed']+4*8,8)==0 and m.time==0 and not m.emits
assert m.run('fr_close_grace',[])==7600000
m.run('fr_close_end',[STATE,HW]);assert m.run('fr_close_grace',[])==7600000
m.w32(STATE+12,0);assert m.run('fr_close_grace',[])==0
rows.append(dict(test='ram_only_checkpoint',elapsed_us=0,grace_us=7600000))
# Uninstrumented close is the v5 timeout reference (the static test proves
# its exact source except the unrelated session wait deadline / NR wrapper).
reference={}
for clear in [0,200000,1000000,1500000]:
 m=Machine();m.clear_after=clear
 def alive_ref(u,a,n,_):
  if a==sy['alive']:m.ret(1)
 m.u.hook_add(UC_HOOK_CODE,alive_ref);m.run('w5_close',[HW])
 reference[clear]=(m.get(HW+off['fin_rc'])>>24,m.time-m.exit_at)
 rows.append(dict(test='v5_reference',clear_after_us=clear,error=reference[clear][0],decision_us=reference[clear][1]))
# Full production private preempt/close path, including RAM points and VDI loop.
for clear in [0,200000,1000000,1500000]:
 for duration in [0,1000000]:
  m=Machine();m.duration=duration;m.clear_after=clear
  # alive() uses SLOT_USED and client stream mapping; return 1 at this native
  # state observation boundary so the complete close body runs.
  def alive(u,a,n,_):
   if a==sy['alive']:m.ret(1)
  m.u.hook_add(UC_HOOK_CODE,alive)
  m.run('live_encoder_preempt_lamp',[ENC,0,STATE,REGS]);error=m.get(HW+off['fin_rc'])>>24
  assert error==(9 if clear>1000000 else 0),(clear,duration,error,m.diag)
  d18=[t for stage,v,t in m.points if stage==45];assert len(d18)==1
  decision=d18[0]-m.exit_at
  assert (error,decision)==reference[clear],(clear,error,decision,reference[clear])
  assert not m.emits
  assert all(stage in [p[0] for p in m.points] for stage in [2,3,5,7,12,21,23,29,35,39,40,41,45,53])
  rows.append(dict(test='VDI',clear_after_us=clear,D_us=duration,decision_us=decision,error=error,return_after_exit_us=m.time-m.exit_at))
report=dict(main_sha256=hashlib.sha256((link/'live-fixed.emulation-only.main').read_bytes()).hexdigest(),cases=rows,limits='Unicorn instruction execution; native calls/time/asynchronous state stubbed. No QEMU or hardware.')
(link/'review9-tests.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS:',len(rows),'object counterexamples: bounded competitor, MOVS identity, RAM-only checkpoints, VDI decisions')
for row in rows:
 if row['test'] in ('close_lock','VDI'):print(row)
