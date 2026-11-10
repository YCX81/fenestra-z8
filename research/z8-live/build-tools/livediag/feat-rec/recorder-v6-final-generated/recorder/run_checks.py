#!/usr/bin/env python3
"""Offline regression suite plus separate, unweakened full link acceptance.
Exit 1 if ANY check fails, including the existing incomplete CFG proof.
"""
import argparse,hashlib,json,subprocess,sys,time
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('build',type=Path);p.add_argument('linked',type=Path)
    a=p.parse_args();build=a.build.resolve();linked=a.linked.resolve()
    source=Path(__file__).resolve().parent;root=source.parents[1]
    out=build/'recorder-check/lamp-v6-tests';out.mkdir(exist_ok=True)
    tests=[(n,[str(build)]) for n in ('verify_static','test_gate','test_frame','test_io','test_recorder','test_v6','test_verify_link')]
    tests += [('test_video_decoder',['--build',str(build)]),('test_forward_stack',[str(linked)]),
              ('verify_lamp_artifact',[str(linked)]),('test_nr',[str(linked)]),
              ('verify_link',['--base',str(root/'bg34.main'),'--final',str(linked/'live-fixed.emulation-only.main'),
                 '--elf',str(linked/'payload.elf'),'--hooks',str(linked/'objects/hooks.S.o'),'--src',str(build)])]
    results=[]
    for name,args in tests:
        command=[sys.executable,str(source/(name+'.py')),*args]
        start=time.monotonic();r=subprocess.run(command,capture_output=True,text=True)
        log=out/(name+'.log');log.write_text(r.stdout+r.stderr)
        result=dict(test=name,exit_code=r.returncode,seconds=round(time.monotonic()-start,3),log=str(log),command=command)
        results.append(result);print(name,'PASS' if r.returncode==0 else 'FAIL',flush=True)
    report=dict(results=results,hardware_tested=False,
        verify_link_sha256=hashlib.sha256((source/'verify_link.py').read_bytes()).hexdigest(),
        main_sha256=hashlib.sha256((linked/'live-fixed.emulation-only.main').read_bytes()).hexdigest())
    (out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    return int(any(r['exit_code'] for r in results))

if __name__=='__main__':raise SystemExit(main())
