#!/usr/bin/env python3
"""Build one private QEMU object/header variant, preserving the shared baseline."""
from pathlib import Path
import argparse,hashlib,json,subprocess

ap=argparse.ArgumentParser();ap.add_argument('--output',type=Path,required=True);ap.add_argument('--header',type=Path,required=True)
a=ap.parse_args();P=a.output.resolve();B=Path('/path/to/user/fxfix/v53-tx16-counter-merged-v3');build=Path('/path/to/user/.local/share/nikon-z8-qemu/merged/work/build')
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
assert sha(B/'qemu-system-aarch64')=='0f341f3285e968f3121d40bf08828fb7faeb40ae6aa65b2a720ef5290b795914'
assert sha(B/'nikon-z9-lab.c')=='974216a2454e6fa46a86096485784e0c3bdfdb322475657bcc5f6a799dc3aceb'
assert sha(B/'nikon-private-tx16.h')=='532ecd8590c2b757066d51813c8c4280f06b83a9b5d6f05c5a327dc4f4cc1069'
assert sha(a.header)=='fddf0b50cf383d3344069a942af6f975af4d5deed900e73d974b27198f1f75fb'
P.mkdir(parents=True,exist_ok=False)
(P/'nikon-z9-lab.c').write_bytes((B/'nikon-z9-lab.c').read_bytes())
(P/'nikon-private-tx16.h').write_bytes(a.header.read_bytes())
cmd=[x.replace(str(B),str(P)) for x in json.loads((B/'private-compile.json').read_text())]
(P/'private-compile.json').write_text(json.dumps(cmd,indent=2)+'\n')
rsp=(B/'private-link.rsp').read_text().replace(str(B),str(P));(P/'private-link.rsp').write_text(rsp)
for label,command in [('compile',cmd),('link',['cc','-m64','@'+str(P/'private-link.rsp')])]:
    with (P/(label+'.log')).open('w') as f:r=subprocess.run(command,cwd=build,stdout=f,stderr=subprocess.STDOUT)
    print(label,r.returncode,flush=True)
    if r.returncode:
        print((P/(label+'.log')).read_text()[-10000:]);raise SystemExit(r.returncode)
help_run=subprocess.run([str(P/'qemu-system-aarch64'),'-M','nikon-z8-bringup,help'],capture_output=True,text=True)
(P/'machine-help.txt').write_text(help_run.stdout+help_run.stderr)
assert help_run.returncode==0 and 'serial5-tx16-sfx-model' in help_run.stdout+help_run.stderr
assert sha(B/'qemu-system-aarch64')=='0f341f3285e968f3121d40bf08828fb7faeb40ae6aa65b2a720ef5290b795914'
out={'output':str(P),'baseline_unchanged':True,'hashes':{n:sha(P/n) for n in ['nikon-z9-lab.c','nikon-private-tx16.h','qemu-system-aarch64','private-compile.json','private-link.rsp']},'machine_help_passed':True,'scope':'Only private TX16 finite SFX lengths/time extension; no firmware or physical-camera operation.'}
(P/'build-receipt.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out,indent=2))
