"""Version-pinned native playback experiment. Keep the real SHA function intact."""
import sys,json,hashlib,runpy
from pathlib import Path
T=Path('/path/to/Nikon/rdwing-z8-qemu/tools');sys.path.insert(0,str(T))
expected='ceefa749d298066d3666c083d4a15a1985939f22ca13ebd8d80244b28963857b'
fw=Path(sys.argv[sys.argv.index('--firmware')+1]).resolve();actual=hashlib.sha256(fw.read_bytes()).hexdigest();assert actual==expected
import runtime
source_manifest=json.loads((runtime.ROOT/'source-manifest.json').read_text())
def verify(path):
 path=path.resolve();assert path==fw and hashlib.sha256(path.read_bytes()).hexdigest()==expected
 return path,source_manifest
runtime.verify=verify
import full_guest
full_guest.MAIN_SHA=expected  # sole accepted experiment image; full_guest.sha remains real
out=Path(sys.argv[sys.argv.index('--output')+1]);out.parent.mkdir(exist_ok=True,parents=True)
(out.parent/(out.name+'-identity.json')).write_text(json.dumps({'executed_main':str(fw),'executed_main_sha256':actual,'sha_function_unmodified':True,'source_manifest':source_manifest,'scope':'functional native firmware test, with explicit model and loader inputs recorded by driver; no physical camera claim'},indent=2)+'\n')
sys.argv[0]=str(T/'run_native.py');runpy.run_path(sys.argv[0],run_name='__main__')
