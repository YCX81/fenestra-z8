# local experiment wrapper: run upstream run_native.py with a patched MAIN (skips the original-firmware check)
import sys, json, runpy
T='/path/to/Nikon/rdwing-z8-qemu/tools'
sys.path.insert(0,T)
import runtime
def verify(fw):
    fw=fw.resolve(); return fw, json.loads((runtime.ROOT/'source-manifest.json').read_text())
runtime.verify=verify
import full_guest
_o=full_guest.sha
full_guest.sha=lambda p:(full_guest.MAIN_SHA if str(p).endswith('.main') else _o(p))
sys.argv[0]=T+'/run_native.py'
runpy.run_path(sys.argv[0],run_name='__main__')
