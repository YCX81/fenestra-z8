#!/usr/bin/env python3
"""Fallback scope, stage placement, no REC I/O, and no unarmed instrumentation."""
import sys,re,hashlib
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent.parent))
import recorder_patch as patch
b=Path(sys.argv[1]);src=patch.SRC
base=(src/'codec/live_w5enc.c').read_text().replace('    EXIT(W5_CLIENT);','    fr_live_exit(W5_CLIENT);')
actual=(b/'codec/live_w5enc.c').read_text()
prefix,clone=actual.split('/* v6: private armed REC diagnostics; original close untouched. */')
assert prefix.rstrip()==('#include "../recorder/recorder.h"\n'+base).rstrip()
close=base[base.index('static int w5_close_body('):base.index('/* v27: worker task, Live not encoding')]
a=base.index('void live_encoder_preempt(');pre=base[a:base.index('\n}',a)+2]
restored=re.sub(r'fr_checkpoint\(\d+u\); ', '',clone).replace('w5_close_body_lamp(', 'w5_close_body(').replace('w5_close_lamp(', 'w5_close(').replace('live_encoder_preempt_lamp(', 'live_encoder_preempt(')
assert restored.strip()==(close+pre).strip()
assert re.findall(r'fr_checkpoint\((\d+)u\)',clone)==['10','11','12','13','14','15','16','17','18','19','1','2','30']
# No new original path guard, no thread/timer/IRQ hook, complete original bodies.
asm=(b/'hooks.S').read_text()
a=asm.index('fr_notify_call:');assert asm[a:].splitlines()[1].strip()=='bl live_w5_bringup_notify_lamp'
a=asm.index('fr_bringup_unarmed:');assert asm[a:].splitlines()[2].strip()=='bl live_w5_bringup_notify'
c=(src/'recorder/recorder.c').read_text();entry=c[c.index('struct fr_state *fr_before('):c.index('/* Same rejection')]
assert all(word not in entry for word in ['persist(', 'record(', 'take(', 'DELAY(', 'OPEN(', 'WRITE(', 'SEEK(', 'CLOSE('])
assert c.count('persist(s,')==1 and 'persist(s,1,0)' in c
# Current file format remains readable; old binaries untouched.
import decoder
for token,events in [(4,[0,15]),(5,[0])]:
 rows,errors=decoder.decode((patch.ROOT/f'runs/W5L0000{token}-v5.BIN').read_bytes())
 assert not errors and [r['事件号'] for r in rows]==events
print('PASS: exact original close prefix, reversible private instrumentation, armed-only dispatch, entry no file call, v5 A/B files')
print('LIMIT: synchronous checkpoints, no periodic hang reporter; native context/locks remain hardware-unverified')
