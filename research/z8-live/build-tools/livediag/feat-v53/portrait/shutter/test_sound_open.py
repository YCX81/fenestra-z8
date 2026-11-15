#!/usr/bin/env python3
"""Show native service OPEN14 still refuses capture owner after upper gate.
Uses unchanged G service/driver and observer shims from independent REC review.
"""
from pathlib import Path
import json
P=Path(__file__).resolve().parent;A=P.parents[1]/'audio';FW=P.parent/'v52g.main'
s=(A/'test_native_output.py').read_text();s=s[:s.index('res=[scenario(')]
s=s.replace("P=Path(__file__).resolve().parent;FW=P.parents[1]/'feat-v50/integration-writer/v51d/q/main.main';D=FW.read_bytes();B=0x40160000",f"P=Path({str(P)!r});FW=Path({str(FW)!r});D=FW.read_bytes();B=0x40160000")
ns={'__file__':str(A/'test_native_output.py')};exec(compile(s,str(A/'test_native_output.py'),'exec'),ns)
rows=[]
for driver in (1,5):
 ns['reset']();ns['w32'](0xa02d4cb48,driver)
 # SFX uses p0=2; corresponding sample class from ROM4165f600 is5,0.
 payload=ns['struct'].pack('<6I',2,5,0,1,9600,0)
 reply=ns['send'](0x43180014,payload)
 rows.append(dict(driver_before=driver,driver_after=ns['r32'](0xa02d4cb48),service_after=ns['r32'](0xa031f8d28),reply_success=reply[20],callbacks=[hex(x) for x in ns['events']],lower=ns['lower'][:]))
assert rows[0]['reply_success']==1 and rows[0]['driver_after']==2,rows
assert rows[1]['reply_success']==0 and rows[1]['driver_after']==5,rows
(P/'sound-open.json').write_text(json.dumps(dict(passed=True,cases=rows,boundary='Actual native OPEN14 handler + driver; RTOS and lower queue shims. Payload is valid SFX2/sample5 48k class; not a claim that forcibly bypassing upper gate is safe.'),indent=2)+'\n')
print('PASS native OPEN14 accepts idle1; refuses capture5 and preserves capture state')
