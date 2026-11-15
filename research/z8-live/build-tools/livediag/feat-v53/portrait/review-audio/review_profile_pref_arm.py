#!/usr/bin/env python3
"""Independent rerun of native/candidate profile constructor on delivered G.
Reuses the observation adapter, changes the firmware to delivered v52 G and
expands every 0..36 volume step. Writes only this private review directory.
"""
from pathlib import Path
import hashlib
P=Path(__file__).resolve().parent
A=P.parents[1]/'audio'
FW=P.parent/'v52g.main'
assert hashlib.sha256(FW.read_bytes()).hexdigest()=='ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613'
assert hashlib.sha256((A/'work/pb-fix37/runtime.c').read_bytes()).hexdigest()=='2fc42c4b40189cd5ab670007f2c4226020a618a8c7a39f50203f0ac9e0fde3eb'
s=(A/'test_profile.py').read_text()
s=s.replace("P=Path(__file__).resolve().parent;D=(P.parents[1]/'feat-v50/integration-writer/v51d/q/main.main').read_bytes();B=0x40160000",f"P=Path({str(A)!r});R=Path({str(P)!r});D=Path({str(FW)!r}).read_bytes();B=0x40160000")
s=s.replace('tempfile.TemporaryDirectory(dir=P)','tempfile.TemporaryDirectory(dir=R)').replace('for v in (0,1,8,15,20,36):','for v in range(37):')
s=s.replace("(P/'profile-tests.json')","(R/'profile-pref-arm.json')")
s=s.replace("cases.append(dict(volume=v", "assert bytes(u.mem_read(0xa03d47afc,2))==struct.pack('<H',v) and bytes(u.mem_read(0xa033868d0,2))==bytes(2)\n   cases.append(dict(volume=v")
s=s.replace("report=dict(passed=True,", "report=dict(firmware_sha256=hashlib.sha256(D).hexdigest(),source_sha256=hashlib.sha256(s.encode()).hexdigest(),passed=True,")
exec(compile(s,str(A/'test_profile.py'),'exec'),{'__file__':str(A/'test_profile.py')})
