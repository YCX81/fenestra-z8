"""Diagnose small NCTG scalar groups; current geometry/JPEGs are preserved."""
import ncdt_variants as v
from pathlib import Path
import json,struct
R=v.R;a=v.a;ta=v.ta;tb=v.tb
# Scalar format values are taken from the known-good same video profile strictly
# as test controls; field 0x13 is replaced with current movie sample count.
groups={'native-core-timing.MOV':[0x13,0x16,0x17,0x18],
        'native-core-format.MOV':[0x21,0x24,0x25,0x26,0x31,0x32,0x33,0x34]}
groups['native-core-all.MOV']=groups['native-core-timing.MOV']+groups['native-core-format.MOV']
report=[]
for name,extra in groups.items():
 tags=dict(ta)
 for tag in extra:tags[tag]=tb[tag]
 if 0x13 in extra:tags[0x13]=tags[0x13][:8]+struct.pack('>II',95,0)
 data=v.replace(a,[b'moov',b'udta',b'NCDT',b'NCTG'],b''.join(tags[t] for t in sorted(tags)))
 assert [(t,b) for t,b in v.boxes(a) if t!=b'moov']==[(t,b) for t,b in v.boxes(data) if t!=b'moov']
 (R/name).write_bytes(data);report.append(dict(name=name,sha256=v.sh(data),bytes=len(data),added_tags=[hex(x) for x in extra],frame_count=95 if 0x13 in extra else None))
(R/'ncdt-core-variants.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
