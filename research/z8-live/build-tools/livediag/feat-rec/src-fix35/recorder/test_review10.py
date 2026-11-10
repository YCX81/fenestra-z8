#!/usr/bin/env python3
"""Fixed-budget adversarial schedules on linked production AArch64 (Unicorn).
Native calls and scheduling/time are inputs; no QEMU or hardware.
"""
from pathlib import Path
# Reuse the object loader/boundary stubs, without running REVIEW9's cases twice.
support=Path(__file__).with_name('test_review9.py')
exec(compile(support.read_text().split('rows=[]')[0],str(support),'exec'))
from unicorn import UC_HOOK_MEM_READ
rows=[]
for entry in ('w5_close','live_encoder_preempt_lamp'):
 for scenario in ('snapshot','churn','unclaimed','disarm_after_entry','arm_after_entry','release_at_deadline','release_before_deadline'):
  m=Machine();m.close_only=True;m.w32(LOCK,1)
  m.w64(STATE+off['diag.close_owner'],HW)
  if scenario=='unclaimed':m.w32(STATE+12,0)
  if scenario=='arm_after_entry':m.w32(STATE,0)
  deadline=4000000 if scenario in ('unclaimed','arm_after_entry') else 11600000
  calls=[];slot_reads=[];attempts=[];writes=[]
  m.u.hook_add(UC_HOOK_MEM_READ,lambda u,ac,a,n,v,_:slot_reads.append(a),begin=STATE+off['diag.close_owner'],end=STATE+off['diag.d_elapsed']+47)
  # Independent real checkpoint writer. Transfer only its diagnostic RAM to
  # the competing reader. First three RAM updates complete before the reader starts.
  writer=Machine();writer.run('fr_close_begin',[STATE,REGS,HW])
  stages=iter((15,17,18));snapshot_events=[]
  def publish(stage):
   writer.time=max(writer.time,m.time+1)
   writer.run('fr_checkpoint',[STATE,REGS,stage])
   a=STATE+off['diag.d_start'];z=STATE+off['diag.d_elapsed']+48
   m.u.mem_write(a,bytes(writer.u.mem_read(a,z-a)))
   snapshot_events.append(dict(stage=stage,start_us=writer.time,end_us=writer.time))
  if scenario=='snapshot':
   writer.time=0
   for stage in (10,11,13):publish(stage)
   m.time=4000000 # All three writes precede this competitor's start.
   deadline+=m.time
  start=m.time
  def adversary(u,a,n,_):
   if a==sy['fr_close_grace']:calls.append(m.time)
   if a==sy['alive'] and entry=='live_encoder_preempt_lamp':m.ret(0 if scenario.startswith('release_') and m.time>=deadline-1000 else 1)
   i=ins.get(a)
   if not i:return
   if i.mnemonic in ('ldaxr','ldxr'):
    reg=i.op_str.split('[')[1].split(']')[0]
    if reg.startswith('x') and u.reg_read(xr[int(reg[1:])])==LOCK:
     attempts.append(m.time)
     if scenario=='churn':m.w32(LOCK,1)
   if i.mnemonic=='mrs' and 'cntpct_el0' in i.op_str:
    if scenario=='churn':m.w32(LOCK,0)
    if scenario=='snapshot' and m.time>=start+4000000 and len(snapshot_events)<6:
     # The reader has just sampled the old time (Machine hook ran first).
     # Writer publishes RAM before reader resumes.
     publish(next(stages));m.time=(writer.time+999)//1000*1000
  m.u.hook_add(UC_HOOK_CODE,adversary)
  def peer(m):
   if scenario=='churn':m.w32(LOCK,1)
   if scenario=='disarm_after_entry':m.w32(STATE,0)
   if scenario=='arm_after_entry':m.w32(STATE,1)
   if scenario=='release_at_deadline' and m.time>=deadline:m.w32(LOCK,0)
   if scenario=='release_before_deadline' and m.time>=deadline-1000:m.w32(LOCK,0)
  m.peer=peer
  m.run(entry,[HW] if entry=='w5_close' else [ENC,0,0,REGS])
  success=scenario.startswith('release_')
  assert m.get(HW+off['close_failed'])==int(not success),(entry,scenario,m.time)
  expected=deadline-1000 if scenario=='release_before_deadline' else deadline if success else deadline+1000
  assert m.time==expected,(scenario,m.time,expected)
  assert len(calls)==1 and not slot_reads,(scenario,calls,slot_reads)
  assert max(attempts)==expected,(scenario,attempts[-1],expected)
  if not success:assert m.get(HW+off['fin_rc'])>>24==11
  if scenario=='snapshot':assert len(snapshot_events)==6 and len(writer.points)==6 and not writer.emits
  rows.append(dict(entry=entry,scenario=scenario,start_us=start,deadline_us=deadline,return_us=m.time,elapsed_us=m.time-start,
    failed=not success,grace_calls=len(calls),slot_reads=len(slot_reads),cas_attempts=len(attempts),
    last_attempt_us=attempts[-1],writer=snapshot_events))
report=dict(main_sha256=hashlib.sha256((link/'live-fixed.emulation-only.main').read_bytes()).hexdigest(),cases=rows,
 limits='Real linked AArch64 wait/checkpoint instructions; native time/scheduling/emitter are stubs. No QEMU or hardware. Original CAS-first, elapsed > budget boundary.')
(link/'review10-tests.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS:',len(rows),'fixed-budget cases, one grace sample each, zero owner/slot reads, original CAS-first boundary')
for row in rows:print(row)
