#!/usr/bin/env python3
"""Build one private QEMU object/header variant, preserving the shared baseline."""
from pathlib import Path
import argparse,hashlib,json,subprocess

ap=argparse.ArgumentParser();ap.add_argument('--output',type=Path,required=True);ap.add_argument('--header',type=Path,required=True)
a=ap.parse_args();P=a.output.resolve();B=Path('/path/to/user/fxfix/v53-tx16-counter-rdw-v3');build=Path('/path/to/user/.local/share/nikon-z8-qemu/rdw/build')
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
assert sha(B/'qemu-system-aarch64')=='dd4da89142651ca3658b0e70b9d2d0b62ae7eb38e7fe1b3ab1778525260a0762'
assert sha(B/'nikon-z8-lab.c')=='ba5b1e46026bcac948621db12a846168c09a344691b471d78b11a5dc0614729d'
assert sha(B/'nikon-z8-audio-tx.h')=='f28c1c2bfa085b58775764c3e9bcbea9a58898e90f2965d17d62daff6bb19f0d'
assert sha(a.header)=='59f9a9097a4b9845419b16d58b4574ae113491c5b169a81169368aafea80ad11'
P.mkdir(parents=True,exist_ok=False)
(P/'nikon-z8-lab.c').write_bytes((B/'nikon-z8-lab.c').read_bytes())
(P/'nikon-z8-audio-tx.h').write_bytes(a.header.read_bytes())
cmd=[x.replace(str(B),str(P)) for x in json.loads((B/'private-compile.json').read_text())]
(P/'private-compile.json').write_text(json.dumps(cmd,indent=2)+'\n')
rsp=(B/'private-link.rsp').read_text().replace(str(B),str(P));(P/'private-link.rsp').write_text(rsp)
for label,command in [('compile',cmd),('link',['c++','-m64','@'+str(P/'private-link.rsp')])]:
    with (P/(label+'.log')).open('w') as f:r=subprocess.run(command,cwd=build,stdout=f,stderr=subprocess.STDOUT)
    print(label,r.returncode,flush=True)
    if r.returncode:
        print((P/(label+'.log')).read_text()[-10000:]);raise SystemExit(r.returncode)
help_run=subprocess.run([str(P/'qemu-system-aarch64'),'-M','nikon-z8-lab,help'],capture_output=True,text=True)
(P/'machine-help.txt').write_text(help_run.stdout+help_run.stderr)
assert help_run.returncode==0 and 'serial5-tx16-sfx-model' in help_run.stdout+help_run.stderr
assert sha(B/'qemu-system-aarch64')=='dd4da89142651ca3658b0e70b9d2d0b62ae7eb38e7fe1b3ab1778525260a0762'
out={'output':str(P),'baseline_unchanged':True,'hashes':{n:sha(P/n) for n in ['nikon-z8-lab.c','nikon-z8-audio-tx.h','qemu-system-aarch64','private-compile.json','private-link.rsp']},'machine_help_passed':True,'scope':'Only private TX16 finite SFX lengths/time extension; no firmware or physical-camera operation.'}
(P/'build-receipt.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out,indent=2))
