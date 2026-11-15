#!/usr/bin/env python3
"""Independent G ring descriptor and candidate full native service config."""
from pathlib import Path
import hashlib,json,struct
P=Path(__file__).resolve().parent;A=P.parents[1]/'audio';FW=P.parent/'v52g.main';SRC=A/'work/pb-fix37/runtime.c'
assert hashlib.sha256(SRC.read_bytes()).hexdigest()=='96cff4d12f9652effbbaf7ff1f690a85cdbc5a95b2bac4f0894e4c4878674daa'
for name in ('test_serial_block.py','test_native_ring.py'):
 s=(A/name).read_text().replace("p=Path(__file__).resolve().parent;d=(p.parents[1]/'feat-v50/integration-writer/v51d/q/main.main').read_bytes();b=0x40160000",f"p=Path({str(P)!r});d=Path({str(FW)!r}).read_bytes();b=0x40160000")
 exec(compile(s,str(A/name),'exec'),{'__file__':str(A/name)})
s=(A/'test_native_output.py').read_text();s=s[:s.index('res=[scenario(')]
s=s.replace("P=Path(__file__).resolve().parent;FW=P.parents[1]/'feat-v50/integration-writer/v51d/q/main.main';D=FW.read_bytes();B=0x40160000",f"P=Path({str(P)!r});FW=Path({str(FW)!r});D=FW.read_bytes();B=0x40160000")
ns={'__file__':str(A/'test_native_output.py')};exec(compile(s,str(A/'test_native_output.py'),'exec'),ns)
ns['reset']();send=ns['send'];u=ns['u']
assert send(0x43180014,struct.pack('<5I',0,5,0,1,0))[20]==1
assert send(0x43180017,struct.pack('<7I',0,1,15,0,15,0,15))[28]==1
assert send(0x4318001a,struct.pack('<QIII',0x8a0000000,192000,5,0)+b'\1')[21]==1
assert send(0x43180015,struct.pack('<IIQII',0,0,ns['CALLBACK'],0,0))[20]==1
# Playback ring native state: source0, bytes8, count12; descriptor path is actual 1a/START.
base=0xa02d4c6b8
# Lower queue was intercepted before low ADC/DAC task dispatcher. Inspect queued706 setup.
configs=[x for x in ns['lower'] if x['event']=='0x42040708']
# Playback output may choose 708 (paired) or706 depending current profile.
report=dict(passed=True,source_sha256=hashlib.sha256(SRC.read_bytes()).hexdigest(),firmware_sha256=hashlib.sha256(FW.read_bytes()).hexdigest(),callbacks=[hex(x) for x in ns['events']],lower=ns['lower'],driver_state=ns['r32'](0xa02d4cb48),boundary='Actual service1a+START and native high driver execute; lower task queue observed. Ring/serial constructors separately execute from configured state.')
assert report['callbacks']==['0x43180014','0x43180017','0x4318001a','0x43180015'] and report['driver_state']==3
# Regardless queue ID, inspect the real high-driver ring carrying requested config.
raw=bytes(u.mem_read(0xa02d4cbd8,48));report['native_high_ring_hex']=raw.hex()
assert struct.unpack_from('<II',raw)==(5,192000),raw.hex()
assert struct.unpack_from('<Q',raw,24)[0]==0x8a0000000
(P/'candidate-block-service.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS actual G 14/17/1a/15: candidate1a stores5x192000 with same source, start ACK and driver3')
