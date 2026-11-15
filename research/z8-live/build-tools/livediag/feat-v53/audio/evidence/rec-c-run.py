"""Version-pinned native playback experiment. Keep the real SHA function intact."""
import sys,json,hashlib,runpy
from pathlib import Path
T=Path('/path/to/Nikon/rdwing-z8-qemu/tools');sys.path.insert(0,str(T))
expected='4a24a4655c3afb14df4b89dd305bec62100030144ffca36157747d26f09019ce'
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
 self.command += ['-plugin','/path/to/user/fxfix/v53-audio/rec-c/observer.so,/path/to/user/fxfix/v53-audio/rec-c/messages.jsonl']
full_guest.Guest.__init__=init
full_guest.MAIN_SHA=expected  # sole accepted experiment image; full_guest.sha remains real
old_exit=full_guest.Guest.__exit__
def leave(self,*args):
 try:
  import base64
  for prop in ['serial5-tx-drain-state','serial5-audio-config-state','serial5-pcm-state']:
   (self.out/(prop+'.json')).write_text(self.qmp.command('qom-get',path='/machine',property=prop))
  pcm=self.qmp.command('qom-get',path='/machine',property='serial5-tx-pcm-capture')
  (self.out/'actual-tx.pcm').write_bytes(base64.b64decode(pcm))
 except Exception as error:(self.out/'audio-export-error.txt').write_text(repr(error))
 return old_exit(self,*args)
full_guest.Guest.__exit__=leave
out=Path(sys.argv[sys.argv.index('--output')+1]);out.parent.mkdir(exist_ok=True,parents=True)
(out.parent/(out.name+'-identity.json')).write_text(json.dumps({'executed_main':str(fw),'executed_main_sha256':actual,'sha_function_unmodified':True,'source_manifest':source_manifest,'scope':'functional native firmware test, with explicit model and loader inputs recorded by driver; no physical camera claim'},indent=2)+'\n')
sys.argv[0]=str(T/'run_native.py');runpy.run_path(sys.argv[0],run_name='__main__')
