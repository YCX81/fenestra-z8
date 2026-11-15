from pathlib import Path
import shutil,json,hashlib
H=Path.home();D=H/'fxfix/v50-playback/recovery-a2-wait-driver'
assert not D.exists()
shutil.copytree(H/'fxfix/v50-playback/recovery-v51d-driver',D)
p=D/'retry-driver/run_live.py';s=p.read_text()
points={
 200:(0x40ece93c,'capture_error'),201:(0x41018a80,'history_error'),
 202:(0x40f5b2a0,'status_classifier'),203:(0x40f532bc,'capture_packet'),
 204:(0x41155310,'stop_subprocess_entry'),205:(0x41155434,'wait_0x1000_before'),
 206:(0x41155438,'wait_0x1000_after'),207:(0x4115545c,'wait_0x1000_failure_or100'),
 208:(0x41152094,'completion_bit0_test'),209:(0x411520c0,'completion_set1000'),
 210:(0x41152044,'completion_event_entry'),211:(0x41154db8,'stop_entry'),
 212:(0x411552f0,'stop_return_mask'),
}
# Conditional failure branches, not high-frequency event/RAM sampling.
for kind,pc in enumerate([0x41157930,0x4115876c,0x41159b28,
 0x4115b80c,0x4115b860,0x4115bac0,0x4115bb50,0x4115bbac,
 0x4115c724,0x4115cf1c,0x4115dae8,0x4115e228,0x4115e69c,
 0x4115eee4,0x4115fbe4,0x41162b8c,0x41162bc4,0x41162cec],220):
 points[kind]=(pc,'other_failure_status80_or100')
assert 18+5+len(points)<=64
old='        error_points={200:0x40ece93c,201:0x41018a80}'
new='        error_points='+repr({k:v[0] for k,v in points.items()})
assert s.count(old)==1
before=hashlib.sha256(s.encode()).hexdigest()
p.write_text(s.replace(old,new))
provenance={'parent_driver_sha256':before,'driver_sha256':hashlib.sha256(p.read_bytes()).hexdigest(),
 'only_change':'additional sparse read-only PC/register observations; same deadlines, inputs, assertions, runtime layout and firmware',
 'points':{str(k):{'pc':hex(v[0]),'name':v[1]} for k,v in points.items()}}
(D/'a2-wait-provenance.json').write_text(json.dumps(provenance,indent=2)+'\n')
print(json.dumps(provenance,indent=2))
