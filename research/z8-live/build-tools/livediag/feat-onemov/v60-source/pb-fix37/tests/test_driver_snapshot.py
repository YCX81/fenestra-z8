"""Exercise read-only ownership resolution and malformed receipt rejection."""
import importlib.util,json,struct
from pathlib import Path
D=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('runtime_snapshot',D/'driver/runtime_snapshot.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
class Guest:
    def __init__(self):self.mem={};self.reads=[]
    def put(self,at,data):self.mem.update((at+i,x) for i,x in enumerate(data))
    def integer(self,at,value,size):self.put(at,value.to_bytes(size,'little'))
    def read_bytes(self,at,size):self.reads.append((at,size));return bytes(self.mem.get(at+i,0) for i in range(size))
g=Guest();root=0xa01000000;core=0xa02000000;runtime=0xa03000000
layout={'magic':0x50425549,'version':1,'bytes':4096,'root_events_offset':2528,'offsets':{'started':40,'native_path':352,'manifest':232,'journal':544}}
receipt={'runtime':layout};g.integer(0xa032b50c8+16,root,8);g.integer(root+663*96+80,core,8)
g.integer(core+64,0x465a394d,4);g.integer(core+68,1,4);g.integer(core+2528,runtime,8)
g.integer(runtime,0x50425549,4);g.integer(runtime+4,4096,4);g.integer(runtime+40,1,4)
g.put(runtime+352,b'A:\\DCIM\\100NCZ_8\\DSC_0001.JPG\0');g.integer(runtime+544+1028,1,4)
r=module.snapshot(g,receipt);assert r['started']==1 and r['native_path'].endswith('DSC_0001.JPG') and r['journal']['count']==1
assert r['status']=='validated heap runtime read-only'
g.integer(runtime,0xdead,4);r=module.snapshot(g,receipt);assert r['status']=='runtime identity differs from build receipt' and 'native_path' not in r
g.integer(core+2528,0x1234,8);assert module.snapshot(g,receipt)['status']=='runtime pointer unavailable'
print('DRIVER_SNAPSHOT_PASS root ownership, bounds, native path/journal, mismatched runtime rejection; no writes/calls API')
