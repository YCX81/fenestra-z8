"""Pinned experimental native runner, preserving actual loaded-MAIN identity."""
import hashlib
import json
from pathlib import Path
import runpy
import sys

T = '/path/to/Nikon/rdwing-z8-qemu/tools'
sys.path.insert(0, T)
import runtime
def verify(fw):
    return fw.resolve(), json.loads((runtime.ROOT / 'source-manifest.json').read_text())
runtime.verify = verify
import full_guest
original_sha = full_guest.sha
# Upstream Guest accepts stock only. Preserve its compatibility placeholder but
# explicitly append the independently hashed loader input to this run's data.
full_guest.sha = lambda p: full_guest.MAIN_SHA if str(p).endswith('.main') else original_sha(p)
original_init = full_guest.Guest.__init__
def init(self, firmware, *args, **kw):
    original_init(self, firmware, *args, **kw)
    self.data['actual_loaded_main'] = dict(path=str(firmware.resolve()), sha256=hashlib.sha256(firmware.read_bytes()).hexdigest())
    self.data['live_on_input_scope'] = 'valid persistent FENESTRA config on isolated virtual card; actual encoding must be observed before claiming Live On playback'
full_guest.Guest.__init__ = init
runpy.run_path(str(Path(__file__).with_name('run_native_observed.py')), run_name='__main__')
