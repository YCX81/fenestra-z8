import json
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent/'driver/retry-driver'))
from pcm_feeder import Feeder, BLOCK_NS as B, cancelled_by_rx_stop
assert B==49_200_000

def st(ready=True, enabled=True):
    return dict(ready=ready, serial_config=0x450a1000 if enabled else 0x450a2000)

f = Feeder()
assert f.tick(0, st()) is None
assert f.tick(B, st())['index'] == 0
# Native callback may briefly leave no armed bank: keep the absolute deadline.
assert f.tick(B + 1, st(False)) is None and f.due == 2 * B
assert f.tick(2 * B - 1, st()) is None
assert f.tick(2 * B, st())['index'] == 1
for i in range(2, 202):
    assert f.tick((i + 1) * B - 10, st(False)) is None
    r = f.tick((i + 1) * B, st())
    assert r['index'] == i and r['late_ns'] == 0
assert f.due == 203 * B
# Only original RX disable resets the epoch, without producing an IRQ/sample.
assert f.tick(203 * B, st(False, False)) is None and f.due is None
assert f.tick(300 * B, st(False)) is None and f.due is None
assert f.tick(301 * B, st()) is None and f.due == 302 * B
assert f.tick(302 * B, st())['index'] == 202
# Before the captured MOV is committed, lateness is still an explicit failure.
try:
    f.tick(305 * B, st())
    raise AssertionError('late capture input accepted')
except RuntimeError:
    pass
assert f.index == 203 and f.due == 303 * B
# After commit, report/drop expired external samples; never send them in a burst.
gap = f.tick(305 * B, st(), allow_gap=True)
assert gap['action'] == 'gap' and gap['dropped_blocks'] == 3
assert f.index == 206 and f.due == 306 * B
assert f.tick(305 * B + 1, st(), allow_gap=True) is None
assert f.tick(306 * B, st())['index'] == 206
# A stopped RX must never accept or catch up even if stale ready metadata remains.
assert f.tick(999 * B, st(True, False)) is None and f.due is None
report = dict(passed=True, rearm_cycles=200,
    cases=['absolute cadence survives unavailable bank', 'RX disable ends epoch',
           'enabled/unarmed does not invent input', 'capture lateness remains fatal',
           'postcommit gap is explicit and drops expired input', 'stop never supplies PCM'],
    scope='Host scheduling only. Model tuple/phase/length/ownership gates unchanged; no Ready writes.')
Path(__file__).with_name('pcm-feeder-tests.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report))

from copy import deepcopy
exact=RuntimeError(dict(**{'class':'GenericError','desc':'PCM16 input does not match original enabled nonoverlapping9600B RX bank'}))
before=dict(ready=True,serial_config=0x450a1000,blocks=571,irq_assertions=571)
after=dict(ready=False,serial_config=0x450a0000,blocks=571,irq_assertions=571)
assert cancelled_by_rx_stop(exact,before,after)
negative=0
for key,value in [('ready',True),('serial_config',0x450a1000),('blocks',572),('blocks',570),('irq_assertions',572),('irq_assertions',570)]:
    x=deepcopy(after);x[key]=value;assert not cancelled_by_rx_stop(exact,before,x);negative+=1
assert not cancelled_by_rx_stop(exact,dict(before,ready=False),after);negative+=1
for e in [RuntimeError(),RuntimeError('same text is not structured gate error'),RuntimeError({'class':'Different','desc':exact.args[0]['desc']}),RuntimeError({'class':'GenericError','desc':'PCM16 DMA write failed; no completion issued'})]:
    assert not cancelled_by_rx_stop(e,before,after);negative+=1
Path(__file__).with_name('counter-feeder-tests.json').write_text(json.dumps(dict(passed=True,original_scheduler_cases=report,gate_rejection_negative_cases=negative,exact_rx_stop_case=True,guest_block_us=50000,virtual_block_ns=B),indent=2)+'\n')
print('PASS counter coordinates and strict stop race plus',negative,'negative cases')
