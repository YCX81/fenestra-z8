#!/usr/bin/env python3
"""Original Nikon worker failure branches versus successful state-2 publication.

This does not assume the shared result slot remains zero. API return values and
event posting are explicit test boundaries; native branch instructions run.
"""
from pathlib import Path
import hashlib,json,struct
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import *
HERE=Path(__file__).resolve().parent
D=(HERE.parents[1]/'feat-v50/integration-writer/v51d/q/main.main').read_bytes()
BASE,RAM,STACK,END=0x40160000,0x4e000000,0x4e010000,0x4e01f000
STATE,RESULT=0xa02d3274c,0xa02d32788
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
u.mem_map(BASE,(len(D)+4095)&~4095);u.mem_write(BASE,D)
u.mem_map(RAM,0x20000);u.mem_map(0xa02d30000,0x3000)
calls=[];api_return=0;stop_on_post=True
def w32(a,v):u.mem_write(a,struct.pack('<I',v&0xffffffff))
def w64(a,v):u.mem_write(a,struct.pack('<Q',v))
def r32(a):return struct.unpack('<I',u.mem_read(a,4))[0]
def hook(uc,pc,size,data):
 if pc in (0x40c35020,0x40c350b0,0x40c35048):
  calls.append([hex(pc),uc.reg_read(UC_ARM64_REG_W1)])
  uc.reg_write(UC_ARM64_REG_W0,api_return)
  uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR))
 elif pc==0x40d2bc8c:
  calls.append(['event',uc.reg_read(UC_ARM64_REG_W1)])
  uc.reg_write(UC_ARM64_REG_PC,END if stop_on_post else uc.reg_read(UC_ARM64_REG_LR))
u.hook_add(UC_HOOK_CODE,hook)

def setup(state,rc):
 global api_return
 api_return=rc;calls.clear();u.mem_write(RAM,bytes(0x20000))
 w32(STATE,state);w32(RESULT,0x1234);w64(STACK+0xa18,STATE)
 w64(STACK+0xa10,STATE);w64(STACK+0x9f0,RAM+0x100)
 w64(RAM+0x100,RAM+0x1000);w64(STACK+0x128,RAM+0x200)
 w32(RAM+0x200,100)
 for r,v in [(UC_ARM64_REG_SP,STACK),(UC_ARM64_REG_X23,STATE),
             (UC_ARM64_REG_X21,0),(UC_ARM64_REG_X20,RAM+0x300),
             (UC_ARM64_REG_X28,RAM+0x100),(UC_ARM64_REG_W0,rc)]:u.reg_write(r,v)

cases=[]
for name,start,state,rc,expect_state,expect_result in [
 ('VPUEncOpen fails',0x40c3d4a4,0,1,0,0xffffffff),
 ('InitSequence command fails',0x40c3d51c,0,1,0,0xffffffff),
 ('GetInitialInfo fails',0x40c3c4a0,1,1,1,0xffffffff),
 ('frame buffer capacity insufficient',0x40c3c9cc,1,101,1,0xffffffff),
 ('RegisterFrameBuffer fails',0x40c3ce78,1,1,1,0xffffffff),
 ('linear frame allocation fails',0x40c3d88c,1,1,1,0xffffffff),
 ('linear frame allocation succeeds',0x40c3d88c,1,0,2,0),
 ('duplicate INIT while ready',0x40c3b17c,2,0,2,0xffffffff),
 ('sequence interrupt while already ready',0x40c3c4a0,2,0,2,0xffffffff),
]:
 setup(state,rc)
 u.emu_start(start,END,count=5000)
 assert u.reg_read(UC_ARM64_REG_PC)==END,name
 assert r32(STATE)==expect_state,(name,r32(STATE))
 assert r32(RESULT)==expect_result,(name,hex(r32(RESULT)))
 assert calls[-1]==['event',0x20],(name,calls)
 cases.append(dict(name=name,start=hex(start),state_before=state,state_after=r32(STATE),
                   result_after=hex(r32(RESULT)),calls=list(calls)))

# The native HEADER worker's actual state gate still accepts state 2 even when
# either later event above has overwritten RESULT with -1.
stop_on_post=False;setup(2,0);w32(RESULT,0xffffffff)
w64(STACK+0x978,RAM+0x400);w64(RAM+0x400,0x89f801000)
w64(STACK+0x9b0,RAM+0x410);w32(RAM+0x410,0x200000)
w64(STACK+0x9e8,RAM+0x420);w32(RAM+0x420,1)
u.reg_write(UC_ARM64_REG_W27,0x33)
u.emu_start(0x40c3b668,0x40c3b710,count=1000)
assert r32(STATE)==3 and r32(RESULT)==0xffffffff
assert calls==[['0x40c35048',0x33]],calls
report=dict(passed=True,firmware_sha256=hashlib.sha256(D).hexdigest(),cases=cases,
 header_after_ready_result_minus_one=dict(state_after=r32(STATE),calls=calls),
 conclusion='A persistent result==0 check rejects legal ready states. Tested real init failure branches leave state 0/1, while successful completion alone publishes state 2.',
 boundaries=['Native low-level API return values are supplied by the test.',
             'RTOS event post is recorded; native branch/state stores execute unchanged.'],
 limitation='These tests do not identify which duplicate event occurred in the full E/F run, nor prove device timing or ownership by themselves.')
(HERE/'native-w5-ready-failures.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS 6 real failure branches stay 0/1; success publishes 2; duplicate INIT/IRQ can set result -1 while HEADER remains legal')
