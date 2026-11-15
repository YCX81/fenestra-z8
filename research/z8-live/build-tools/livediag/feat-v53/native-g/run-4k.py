"""Version-pinned native playback experiment. Keep the real SHA function intact."""
import sys,json,hashlib,runpy
from pathlib import Path
T=Path('/path/to/Nikon/rdwing-z8-qemu/tools');sys.path.insert(0,str(T))
expected='c14894087d5fd03b0124c7d94f6cd521e94def58831a087738145ef91b60fcd2'
fw=Path(sys.argv[sys.argv.index('--firmware')+1]).resolve();actual=hashlib.sha256(fw.read_bytes()).hexdigest();assert actual==expected
import runtime
source_manifest=json.loads((runtime.ROOT/'source-manifest.json').read_text())
def verify(path):
 path=path.resolve();assert path==fw and hashlib.sha256(path.read_bytes()).hexdigest()==expected
 return path,source_manifest
runtime.verify=verify
import full_guest
old_init=full_guest.Guest.__init__
def init(self,*args,**kwargs):
 old_init(self,*args,**kwargs)
 self.command += ['-plugin','/path/to/user/fxfix/v53-native-rec/probe-n2/geometry_observer6.so,out=/path/to/user/fxfix/v53-root/native-g/trace-4k.jsonl']
full_guest.Guest.__init__=init
full_guest.MAIN_SHA=expected  # sole accepted experiment image; full_guest.sha remains real
out=Path(sys.argv[sys.argv.index('--output')+1]);out.parent.mkdir(exist_ok=True,parents=True)
(out.parent/(out.name+'-identity.json')).write_text(json.dumps({'executed_main':str(fw),'executed_main_sha256':actual,'sha_function_unmodified':True,'source_manifest':source_manifest,'scope':'functional native firmware test, with explicit model and loader inputs recorded by driver; no physical camera claim'},indent=2)+'\n')
sys.argv[0]='/path/to/user/fxfix/v53-native-rec/probe-n2/run_native_observed.py';runpy.run_path(sys.argv[0],run_name='__main__')
