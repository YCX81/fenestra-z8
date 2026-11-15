"""Independent actual-host-source schedule/Ready counterexamples; no guest writes."""
from pathlib import Path
import ast,hashlib,importlib.util,itertools,json
P=Path(__file__).resolve().parent; A=P.parents[3]/'audio';D=A/'core-pcm-counter-f/driver/retry-driver'
spec=importlib.util.spec_from_file_location('actual_feeder',D/'pcm_feeder.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
F=m.Feeder;B=m.BLOCK_NS
state=lambda ready=True,enabled=True:dict(ready=ready,serial_config=0x450a1000 if enabled else 0x450a2000)
f=F();f.tick(123,state());assert f.due==123+B
for i in range(500):
 for shift in (-B+1,-B//2,-1):assert f.tick(123+(i+1)*B+shift,state(False))is None
 a=f.tick(123+(i+1)*B,state());assert a['index']==i and a['late_ns']==0
assert f.due==123+501*B
# Missing armed bank does not erase existing missed time; fails on next arm.
f.tick(123+505*B,state(False));due=f.due
try:f.tick(123+505*B,state());raise AssertionError('deadline erased')
except RuntimeError:pass
assert f.due==due and f.index==500
# Postcommit gap emits no input and never replays old blocks.
x=f.tick(123+505*B,state(),True);assert x['action']=='gap' and x['dropped_blocks']==5
assert f.tick(123+505*B+1,state(),True)is None
assert f.tick(123+506*B,state())['index']==505
assert f.tick(123+507*B,state(True,False))is None and f.due is None
assert f.tick(123+700*B,state(False))is None and f.due is None
assert f.tick(123+701*B,state())is None and f.due==123+702*B
# Execute actual Ready gate function with read-only host mocks; all conjunctions.
tree=ast.parse((D/'run_live.py').read_text());fun=next(x for x in tree.body if isinstance(x,ast.FunctionDef)and x.name=='audio_contact_ready')
class Log:
 def __truediv__(self,name):return self
 def open(self,mode):return self
 def __enter__(self):return self
 def __exit__(self,*args):pass
 def write(self,x):pass
class Q:
 def __init__(self):self.cmd=[]
 def command(self,c):self.cmd.append(c)
class G:
 def __init__(self):self.qmp=Q();self.data={};self.out=Log()
class Peer:
 def __init__(self):self.g=G();self.time=0
 def now(self):return self.time
 def handle(self):pass
ns={'snapshot':lambda g,label:dict(runtime=current,native_audio={}), 'json':json}
exec(compile(ast.Module(body=[fun],type_ignores=[]),'<actual Ready function>','exec'),ns)
cases=0
for warm,active,phase,frames in itertools.product((0,1),(0,1),(1,2),(40,41)):
 current={'audio_warm':warm,'audio.active':active,'capture.phase':phase,'stream.encoded':frames};before=current.copy();peer=Peer();ok,_=ns['audio_contact_ready'](peer,'full-press');assert ok==(warm==0 and active==1 and phase==2 and frames>40);assert current==before;assert peer.g.qmp.cmd==['stop','cont'];cases+=1
current={'audio_warm':1,'audio.active':1,'capture.phase':1,'stream.encoded':100};peer=Peer();ns['audio_contact_ready'](peer,'full-press');peer.time=60_000_000_001
try:ns['audio_contact_ready'](peer,'full-press');raise AssertionError('unbounded wait')
except AssertionError as e:assert 'inside60s' in str(e)
r=dict(passed=True,source_sha256={str(p.relative_to(A)):hashlib.sha256(p.read_bytes()).hexdigest()for p in (D/'pcm_feeder.py',D/'run_live.py')},rearm_cycles=500,ready_combinations=cases,deadline_tests=['beforecommit missing bank retains deadline and fails on rearm','postcommit gap drops expired blocks, no same-time burst','RX disable resets epoch without input','Ready wait after60s fails; no Ready field written'],boundary='Host source functions execute with read-only fake snapshots; this checks driver logic, not the physical ADC clock or QEMU TX/RX model.')
(P/'counter-feeder-review.json').write_text(json.dumps(r,indent=2)+'\n');print('PASS independent actual Feeder500 cycles, Ready16 combinations and timeout')

# Independent exact admission-gate and post-stop counter combinations.
exact=RuntimeError({'class':'GenericError','desc':'PCM16 input does not match original enabled nonoverlapping9600B RX bank'})
before=dict(ready=True,serial_config=0x450a1000,blocks=31,irq_assertions=29)
accepted=0; total=0
for ready,on,blockdelta,irqdelta in itertools.product((False,True),(False,True),(-1,0,1),(-1,0,1)):
 after=dict(ready=ready,serial_config=0x450a1000 if on else 0x450a0000,blocks=31+blockdelta,irq_assertions=29+irqdelta)
 ok=m.cancelled_by_rx_stop(exact,before,after)
 assert ok==(not ready and not on and blockdelta==0 and irqdelta==0)
 total+=1;accepted+=ok
for err in [RuntimeError(),RuntimeError('PCM16 input does not match original enabled nonoverlapping9600B RX bank'),RuntimeError({'class':'GenericError','desc':'PCM16 DMA write failed; no completion issued'}),RuntimeError({'class':'DeviceNotFound','desc':exact.args[0]['desc']})]:
 assert not m.cancelled_by_rx_stop(err,before,dict(ready=False,serial_config=0,blocks=31,irq_assertions=29))
assert accepted==1 and total==36
assert m.BLOCK_NS==49200000 and m.MAX_LATENESS_NS==98400000
for n in range(0,984*50,7):assert (n//41)//24==n//984
f=m.Feeder();f.tick(123,state());a=f.tick(123+B,state());assert a and f.due==123+2*B
assert m.cancelled_by_rx_stop(exact,before,dict(ready=False,serial_config=0x450a0000,blocks=31,irq_assertions=29))
assert f.tick(123+B+1,state(False,False))is None and f.due is None
assert f.tick(999+B,state())is None and f.due==999+2*B
r.update(stop_gate_combinations=total,accepted_exact_stop_cases=accepted,other_error_rejection_cases=4,virtual_block_ns=B,late_virtual_ns=m.MAX_LATENESS_NS,clock_coordinate_identity_checked=True)
(P/'counter-feeder-review.json').write_text(json.dumps(r,indent=2)+'\n')
print('PASS exact gate36 cases, other errors4, stopped/restarted epoch,984 conversion')
