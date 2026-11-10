#!/usr/bin/env python3
"""REVIEW11: execute real close loops against the original compiled object.
Unicorn only; time, native calls and scheduling are controlled inputs.
"""
import hashlib,json,os,subprocess,sys
from pathlib import Path
link=Path(sys.argv[1]).resolve()
source=Path(__file__).resolve().parents[2]/'src-fix35'
# Compile the untouched original codec using exactly the production flags.
commands=json.loads((link/'commands.json').read_text())
cmd=next(c[:] for c in commands if c[c.index('-c')+1].endswith('codec/live_w5enc.c'))
cmd[cmd.index('-c')+1]=str(source/'codec/live_w5enc.c')
cmd[cmd.index('-o')+1]=str(link/'original-close.o')
subprocess.run(cmd,check=True)
# The alias satisfies an unused runtime relocation, never executes in reference
# cases. Original w5_close and live_encoder_preempt remain unmodified.
ld=link/'original-close.ld'
ld.write_text((link/'payload.ld').read_text()+'\nlive_encoder_preempt_lamp = live_encoder_preempt;\n')
objects=[str(link/'original-close.o') if c[-1].endswith('codec_live_w5enc.c.o') else c[-1] for c in commands]
root=source.parent
zig=next((root/'.recorder-venv/lib').glob('python*/site-packages/ziglang/zig'))
env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR='/tmp/review11-zig-global',ZIG_LOCAL_CACHE_DIR='/tmp/review11-zig-local')
subprocess.run([str(zig),'cc','-target','aarch64-freestanding-none','-nostdlib','-Wl,-e,live_ui_system_dispatch','-Wl,-T,'+str(ld),*objects,'-o',str(link/'original-close.elf')],check=True,env=env)
support=Path(__file__).with_name('test_review9.py')
prefix=support.read_text().split('rows=[]')[0]
def load(original=False):
 ns={'__name__':'review11_support'}
 exec(compile(prefix.replace("link/'payload.elf'", "link/'original-close.elf'") if original else prefix,str(support),'exec'),ns)
 return ns
new=load();old=load(True)
MASK=(1<<64)-1

def run(ns,entry,armed,scenario,release=None,claimed=1):
 Machine=ns['Machine'];r=ns['r'];xr=ns['xr'];ins=ns['ins'];sy=ns['sy'];off=ns['off'];HW=ns['HW'];STATE=ns['STATE'];LOCK=ns['LOCK']
 m=Machine();m.close_only=True;m.w32(STATE,armed);m.w32(STATE+12,claimed);m.w32(LOCK,0 if release==0 else 1)
 # A positive initial timestamp for frequency failure / backwards reading.
 start=10000000 if scenario in ('zero_frequency','backwards') else MASK-4002000 if scenario=='wrap' else 0
 m.time=start;attempts=[];grace=[];reads=[];delays=[]
 def peer(m):
  if scenario=='wrap' and not delays:m.time=start+4004000
  if release is not None and m.time-start>=release:m.w32(LOCK,0)
 m.peer=peer
 def observe(u,a,n,_):
  if a==sy['fr_close_grace']:grace.append(m.time)
  if a==0x40187b00:delays.append(m.time)
  if a==sy['alive'] and entry!='w5_close':m.ret(0 if attempts and release is not None and m.time-start>=release else 1)
  i=ins.get(a)
  if not i:return
  if i.mnemonic in ('ldaxr','ldxr'):
   reg=i.op_str.split('[')[1].split(']')[0]
   if reg.startswith('x') and u.reg_read(xr[int(reg[1:])])==LOCK:attempts.append(m.time-start)
  if i.mnemonic=='mrs':
   reg,what=i.op_str.split(', ')
   if what=='cntpct_el0':
    value=(start-1 if scenario=='backwards' and delays else m.time)&MASK
    u.reg_write(xr[int(reg[1:])],value);u.reg_write(r.UC_ARM64_REG_PC,a+4)
   if what=='cntfrq_el0':
    value=0 if scenario=='zero_frequency' and delays else 1000000
    u.reg_write(xr[int(reg[1:])],value);u.reg_write(r.UC_ARM64_REG_PC,a+4)
 m.u.hook_add(ns['UC_HOOK_CODE'],observe)
 from unicorn import UC_HOOK_MEM_READ
 m.u.hook_add(UC_HOOK_MEM_READ,lambda u,ac,a,n,v,_:reads.append(a),begin=STATE+off['diag.close_owner'],end=STATE+off['diag.d_elapsed']+47)
 m.run(entry,[HW] if entry=='w5_close' else [ns['ENC'],0,0,ns['REGS']])
 return dict(elapsed_us=m.time-start,failed=m.get(HW+off['close_failed']),fin_error=m.get(HW+off['fin_rc'])>>24,delays=len(delays),grace_calls=len(grace),slot_reads=len(reads),cas_attempts=len(attempts),last_cas_us=attempts[-1])

rows=[]
for entry in ('w5_close','live_encoder_preempt_lamp'):
 reference_entry='w5_close' if entry=='w5_close' else 'live_encoder_preempt'
 for scenario in ('zero_frequency','backwards','wrap'):
  ref=run(old,reference_entry,0,scenario)
  for armed in (0,1):
   # REVIEW11 wrap input is unarmed: the original 4 s budget is the oracle.
   # Also test armed/unclaimed, which must retain that same budget.
   claimed=0 if armed and scenario=='wrap' else 1
   got=run(new,entry,armed,scenario,claimed=claimed)
   for key in ('elapsed_us','failed','fin_error','delays','cas_attempts','last_cas_us'):assert got[key]==ref[key],(entry,scenario,armed,key,got,ref)
   assert got['elapsed_us']==(4004000 if scenario=='wrap' else 1000) and got['failed']==1 and got['fin_error']==11
   assert got['grace_calls']==1 and got['slot_reads']==0 and got['delays']==1
   rows.append(dict(entry=entry,scenario=scenario,armed=armed,claimed=claimed,new=got,original=ref))
 for armed,claimed in ((0,1),(1,0),(1,1)):
  budget=11600000 if armed and claimed else 4000000
  for release in (None,0,200000,6200000,budget,budget+1000):
   got=run(new,entry,armed,'normal',release,claimed)
   success=release is not None and release<=budget+1000
   expected=release if success else budget+1000
   assert got['elapsed_us']==expected and got['failed']==int(not success),(entry,armed,release,got)
   assert got['grace_calls']==1 and got['slot_reads']==0
   if not success:assert got['fin_error']==11
   ref=run(old,reference_entry,0,'normal',release) if budget==4000000 else None
   if ref:
    for key in ('elapsed_us','failed','fin_error','delays','cas_attempts','last_cas_us'):assert got[key]==ref[key],(key,got,ref)
   rows.append(dict(entry=entry,scenario='normal',armed=armed,claimed=claimed,budget_us=budget,release_us=release,new=got,original=ref))
# Save disassembly for an auditable instruction-level comparison.
lines=[]
for label,ns in [('original',old),('fixed',new)]:
 for name in ('w5_close','live_encoder_preempt_lamp' if label=='fixed' else 'live_encoder_preempt'):
  sym=next(s for s in ns['elf'].get_section_by_name('.symtab').iter_symbols() if s.name==name)
  lines.append(f'\n{label}: {name}')
  lines.extend(f'{a:08x}: {i.mnemonic} {i.op_str}' for a,i in ns['ins'].items() if sym['st_value']<=a<sym['st_value']+sym['st_size'])
(link/'review11-close-disassembly.txt').write_text('\n'.join(lines)+'\n')
report=dict(main_sha256=hashlib.sha256((link/'live-fixed.emulation-only.main').read_bytes()).hexdigest(),original_source_sha256=hashlib.sha256((source/'codec/live_w5enc.c').read_bytes()).hexdigest(),cases=rows,limits='Actual linked AArch64; original source compiled with identical flags. Native time/services/scheduling stubbed. No QEMU/hardware. Strict elapsed > preserves the original one-sampling-step boundary; no wall-clock bound without scheduling.')
(link/'review11-tests.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS:',len(rows),'cases; REVIEW11 failures return at original object times; CAS first, fixed budget, zero slot reads')
for row in rows:print(row)
