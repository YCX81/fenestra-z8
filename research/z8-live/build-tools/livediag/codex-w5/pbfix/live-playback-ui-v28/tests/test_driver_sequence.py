"""Guard the retry against the observed unsuccessful second playback entry.
Construct actual contacts without importing/launching the command-line driver.
"""
import ast
from pathlib import Path
from types import SimpleNamespace
D=Path(__file__).resolve().parents[1]
source=ast.parse((D/'driver/run_live.py').read_text())
contact=next(n for n in source.body if isinstance(n,ast.FunctionDef) and n.name=='contacts')
class Probe:
    @staticmethod
    def key_body():return bytes(124)
    @staticmethod
    def expected_keys(*args):return bytes(44)
    @staticmethod
    def shutter_body(*args):return bytes(124)
    @staticmethod
    def navigation_contact(key):
        body=bytearray(124)
        body[{'menu':0x43,'left':0x23,'right':0x22,'down':0x25,'up':0x24,'playback':0x44,'rec':9,'delete':0x45}[key]]=1
        return bytes(body),bytes(44)
ns={'A':SimpleNamespace(mode='ui'),'probe':Probe}
exec(compile(ast.Module(body=[contact],type_ignores=[]),'driver-contacts','exec'),ns)
rows,_=ns['contacts'](None);labels=[r['label'] for r in rows]
restored=labels.index('pb-play-restored');prompt=labels.index('pb-delete-prompt');confirm=labels.index('pb-delete-confirm');complete=labels.index('pb-delete-complete');exit_playback=labels.index('pb-play-return-shooting')
assert restored<prompt<confirm<complete<exit_playback
assert not any(r['body'][0x44] for r in rows[restored:complete])
assert sum(r['body'][0x44] for r in rows)==2 # enter once, exit once
assert rows[prompt]['body'][0x45] and rows[confirm]['body'][0x45]
assert rows[confirm-1]['body'][0x45]==0 and rows[confirm-1]['gap']>=2_000_000_000
assert rows[complete-1]['body'][0x45]==0 and rows[complete-1]['gap']>=8_000_000_000
assert labels.index('pb-regress-native-remains')<exit_playback<labels.index('pb-play-shooting')
assert not any(r['body'][0x44] for r in rows[exit_playback+1:])
print('DRIVER_SEQUENCE_PASS one native playback session; EOS then guarded delete/confirm; final Play exits; no unproven re-entry')
