"""One-MOV QEMU experiment driver: any image, own observer plugin (env NB_OUT, NB_PTS)."""
import sys,json,hashlib,runpy,os
from pathlib import Path
T=Path('/path/to/Nikon/rdwing-z8-qemu/tools');sys.path.insert(0,str(T))
fw=Path(sys.argv[sys.argv.index('--firmware')+1]).resolve();expected=hashlib.sha256(fw.read_bytes()).hexdigest()
import runtime
source_manifest=json.loads((runtime.ROOT/'source-manifest.json').read_text())
def verify(path):
 path=path.resolve();assert path==fw
 return path,source_manifest
runtime.verify=verify
import full_guest
old_init=full_guest.Guest.__init__
def init(self,*args,**kwargs):
 old_init(self,*args,**kwargs)
 if os.environ.get('NB_PEER'):self.command[self.command.index('-M')+1]+=',codec-spi-register-peer=on,serial5-tx16-ring-model=on,serial5-tx16-sfx-model=on,serial5-tx-counter-clock=on'
 self.command += ['-plugin','/path/to/user/fxfix/onemov/nbobs.so,out=%s,pts=%s'%(os.environ['NB_OUT'],os.environ['NB_PTS'])]
full_guest.Guest.__init__=init
full_guest.MAIN_SHA=expected
sys.argv[0]='/path/to/user/fxfix/v53-native-rec/probe-n2/run_native_observed.py';runpy.run_path(sys.argv[0],run_name='__main__')
