#!/usr/bin/env python3
"""Fallback scope, stage placement, no REC I/O, and no unarmed instrumentation."""
import sys,re,hashlib
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent.parent))
import recorder_patch as patch
b=Path(sys.argv[1]);src=patch.SRC
base=(src/'codec/live_w5enc.c').read_text().replace('    EXIT(W5_CLIENT);','    fr_live_exit(W5_CLIENT);')
actual=(b/'codec/live_w5enc.c').read_text()
v7=(patch.ROOT/'runs/lamp-v8-baseline/recorder-build/codec/live_w5enc.c').read_text()
marker='/* v6: private armed REC diagnostics; shared close has a fixed deadline. */'
prefix,clone=actual.split(marker)
from v9_scope import without_allocation
assert without_allocation(prefix)==without_allocation(v7.split(marker)[0]), 'shared/unarmed code outside v9 allocation changed'
assert (b/'hooks.S').read_bytes()==(patch.ROOT/'runs/lamp-v8-baseline/recorder-build/hooks.S').read_bytes()
assert clone.count('DELAY(1)')==5
assert 'next_beat' not in clone and 'emit(' not in clone
for stage in (2,3,4,5,6,7,8,9,10,11,12,13,14,16,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,50,51,52,53,58):
 assert f'regs,{stage}u,' in clone, stage
# No new thread/timer/IRQ hook; shared timeout change is normalized above.
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
print('PASS: shared code unchanged outside v9 allocation and fixed session deadline, private instrumentation, armed-only dispatch, entry no file call, v5 A/B files')
print('LIMIT: synchronous busy heartbeats require task progress; native context/locks remain hardware-unverified')
