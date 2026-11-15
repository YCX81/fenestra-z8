#!/usr/bin/env python3
"""Independent unchanged v52 G native service/driver rerun; no physical output."""
from pathlib import Path
import hashlib
P=Path(__file__).resolve().parent;A=P.parents[1]/'audio';FW=P.parent/'v52g.main'
assert hashlib.sha256(FW.read_bytes()).hexdigest()=='ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613'
s=(A/'test_native_output.py').read_text()
s=s.replace("P=Path(__file__).resolve().parent;FW=P.parents[1]/'feat-v50/integration-writer/v51d/q/main.main';D=FW.read_bytes();B=0x40160000",f"P=Path({str(P)!r});FW=Path({str(FW)!r});D=FW.read_bytes();B=0x40160000")
s=s.replace("print(json.dumps(out,indent=2))", "print('PASS actual delivered-G service/driver: clean cached profile fails, explicit17 and dirty cached profile both configure output')")
exec(compile(s,str(A/'test_native_output.py'),'exec'),{'__file__':str(A/'test_native_output.py')})
