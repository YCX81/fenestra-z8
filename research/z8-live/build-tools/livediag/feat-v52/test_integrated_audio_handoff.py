"""Execute the linked Live -> registry -> PB audio handoff path, no service mocks."""
import argparse, hashlib, json
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X30, UC_ARM64_REG_SP

p=argparse.ArgumentParser();p.add_argument('main',type=Path);p.add_argument('layout',type=Path);p.add_argument('output',type=Path)
a=p.parse_args();b=a.main.read_bytes();sy=json.loads(a.main.with_name('changed-ranges.json').read_text())['symbols']
layout=json.loads(a.layout.read_text());base=0x40160000;end=base
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(base,(len(b)+4095)&~4095);u.mem_write(base,b)
u.mem_map(0xa02000000,0x2000000);u.mem_map(0xa10000000,0x40000)
pages=0xa10000000;core=0xa10010000;reg=0xa10012000;pb=0xa10014000;stack=0xa1003fff0
def put(address,value,n=4):u.mem_write(address,value.to_bytes(n,'little'))
def get(address):return int.from_bytes(u.mem_read(address,4),'little')
def call():
    u.reg_write(UC_ARM64_REG_SP,stack);u.reg_write(UC_ARM64_REG_X30,end)
    u.emu_start(sy['live_ui_pb_audio_handoff'],end,count=10000)
    return u.reg_read(UC_ARM64_REG_X0)
checks=0
def check(condition):
    global checks
    assert condition;checks+=1
check(call()==1)
put(0xa032b50d8,pages,8);put(pages+663*96+80,core,8)
check(call()==1)
put(core+64,0x465a394d);put(core+68,1);put(core+32,46,2)
check(call()==1)
put(core+2528,reg,8);put(reg,0x47525846);put(reg+4,64)
check(call()==1)
for bad in [1,0x40195ffc,0x401a0000,0x40196001]:
    put(reg+32,bad,8);check(call()==0)
put(reg+32,sy['pb_audio_handoff'],8)
check(call()==1)
put(reg+16,pb,8);put(pb,0x50425549);put(pb+4,layout['struct_bytes'])
gate=pb+layout['audio_gate']
for before,after,quiet in [(0,5,1),(5,5,1),(2,7,0),(7,7,0),(6,11,0),(9,9,1)]:
    put(gate,before);check(call()==quiet and get(gate)==after)
result=dict(passed=True,checks=checks,main_sha256=hashlib.sha256(b).hexdigest(),
            linked_live_registry_pb_path=True,native_audio_service_executed=False,
            quiet_without_runtime=True,invalid_callback_rejected=True,
            ownership_and_generation_preserved=True)
a.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
