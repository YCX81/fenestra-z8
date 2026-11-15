from pathlib import Path
import hashlib,json,subprocess,tempfile

root=Path(__file__).resolve().parent
pb=root/'work/pb-fix37'
baseline=root.parents[1]/'feat-v52/integration-g/work/pb-fix37/native_ui.c'
with tempfile.TemporaryDirectory(prefix='v53-aspect-') as td:
    binary=Path(td)/'test'
    command=['clang','-std=c11','-O1','-g','-ffunction-sections','-fdata-sections',
        '-fsanitize=address,undefined','-I',str(pb/'core-include'),'-I',str(pb),
        str(root/'test_aspect.c'),str(pb/'ui.c'),'-Wl,-dead_strip','-o',str(binary)]
    subprocess.run(command,check=True)
    fixed=subprocess.run([str(binary)],capture_output=True,text=True)
    assert fixed.returncode==0,fixed.stderr
    # Keep the fixed converter to isolate only the rendering defect. The G
    # renderer must fail the same circle/containment invariant in this harness.
    subprocess.run(command+[f'-DPB_NATIVE_SOURCE="{baseline}"'],check=True)
    old=subprocess.run([str(binary)],capture_output=True,text=True)
    assert old.returncode!=0 and 'sx-sy' in old.stderr,old.stderr
    result={'fixed_exit':fixed.returncode,'fixed_output':fixed.stdout.strip(),
        'baseline_renderer_exit':old.returncode,'baseline_error':old.stderr.strip(),
        'source_sha256':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (pb/'ui.c',pb/'native_ui.c')},
        'scope':'host execution of production converter and native VG command construction; not physical display acceptance'}
    (root/'test-result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
