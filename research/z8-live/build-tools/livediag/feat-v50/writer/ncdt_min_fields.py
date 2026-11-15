"""Only current clip scalar facts; no donor tag payload copied."""
from pathlib import Path
import json,struct
import ncdt_variants as v
R=v.R;tags=dict(v.ta)
def tag(t,k,c,data):return struct.pack('>IHH',t,k,c)+data
# The diagnostic has 95 samples; its selected native HEVC profile is nominal
# 30000/1001, 8-bit. Scalar enum values are pinned to this known-good profile.
tags[0x13]=tag(0x13,4,2,struct.pack('>II',95,0))
for t in [0x16,0x17]:tags[t]=tag(t,5,1,struct.pack('>II',30000,1001))
for t,n in [(0x18,1),(0x24,1),(0x25,8),(0x26,1),(0x32,2),(0x33,24),(0x34,48000)]:tags[t]=tag(t,3,1,struct.pack('>H',n))
out=v.replace(v.a,[b'moov',b'udta',b'NCDT',b'NCTG'],b''.join(tags[t] for t in sorted(tags)))
assert [(t,b) for t,b in v.boxes(v.a) if t!=b'moov']==[(t,b) for t,b in v.boxes(out) if t!=b'moov']
p=R/'native-min-fields.MOV';p.write_bytes(out)
r=dict(name=p.name,bytes=len(out),sha256=v.sh(out),input_sha256=v.sh(v.a),tags=len(tags),added=['0x13','0x16','0x17','0x18','0x24','0x25','0x26','0x32','0x33','0x34'],scope='generated scalar records; enum identity still under firmware scrutiny, no donor bytes')
(R/'native-min-fields.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r,indent=2))
