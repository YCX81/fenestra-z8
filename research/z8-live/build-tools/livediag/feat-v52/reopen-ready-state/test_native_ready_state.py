#!/usr/bin/env python3
"""Unmodified native reset/completion/flag-match instructions, not a scheduler model."""
from pathlib import Path
import hashlib,json,struct
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn import arm64_const as A
D=Path(__file__).resolve().parent
FW=Path('/path/to/user/fxfix/v52-root/v52eclose/q/main.main')
B=FW.read_bytes();BASE=0x40160000;RAM=0x4e000000;STOP=RAM+0x1f000;SP=RAM+0x10000
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(BASE,(len(B)+4095)&~4095);u.mem_write(BASE,B)
u.mem_map(RAM,0x20000);u.mem_map(0xa02d30000,0x3000)
def p(k,a,v):u.mem_write(a,struct.pack('<'+k,v))
def q(k,a):return struct.unpack('<'+k,u.mem_read(a,struct.calcsize(k)))[0]
posts=[]; commands=[]
def hook(uc,pc,size,data):
    if pc==0x4127ab40:
        dst=uc.reg_read(A.UC_ARM64_REG_X0);value=uc.reg_read(A.UC_ARM64_REG_W1)&255;n=uc.reg_read(A.UC_ARM64_REG_X2)
        uc.mem_write(dst,bytes([value])*n)
    elif pc==0x40c35048:
        commands.append(dict(command=uc.reg_read(A.UC_ARM64_REG_W1),state=q('I',0xa02d3274c),result=q('I',0xa02d32788)))
        uc.reg_write(A.UC_ARM64_REG_W0,0)
    elif pc==0x40d2bc8c:
        posts.append(dict(event_object=hex(uc.reg_read(A.UC_ARM64_REG_X0)),mask=hex(uc.reg_read(A.UC_ARM64_REG_W1)),
            state=q('I',0xa02d3274c),worker_result=q('I',0xa02d32788)))
    else:return
    uc.reg_write(A.UC_ARM64_REG_PC,uc.reg_read(A.UC_ARM64_REG_X30))
u.hook_add(UC_HOOK_CODE,hook)
def call(start,end=STOP,regs=()):
    u.reg_write(A.UC_ARM64_REG_SP,SP);u.reg_write(A.UC_ARM64_REG_X30,end)
    for r,v in regs:u.reg_write(r,v)
    u.emu_start(start,end,count=10000)
    assert u.reg_read(A.UC_ARM64_REG_PC)==end
    return u.reg_read(A.UC_ARM64_REG_W0)
resets=[]
for slot in (0,1):
    u.mem_write(0xa02d30000,b'\xa5'*0x3000)
    old=bytes(u.mem_read(0xa02d32748,0x58));events=bytes(u.mem_read(0xa02d30070,24))
    call(0x40c375e0,regs=[(A.UC_ARM64_REG_W0,slot)])
    assert bytes(u.mem_read(0xa02d32748,0x58))==old
    assert bytes(u.mem_read(0xa02d30070,24))==events
    assert q('Q',0xa02d30f28+slot*8)==0 and q('I',0xa02d31078+slot*4)==0
    resets.append(dict(slot=slot,result_region_2748_279f_unchanged=True,event_wrappers_0070_0087_unchanged=True,header_pointer_cleared=True))
matches=[]
for pending in (0,0x10,0x20,0x30,0x80020):
    for mask in (0x20,0x800):
        p('I',RAM+0x18,pending);p('I',RAM+0x100,0x12345678)
        rc=call(0x4017b1ec,regs=[(A.UC_ARM64_REG_X0,RAM),(A.UC_ARM64_REG_W1,mask),(A.UC_ARM64_REG_W2,0x21),(A.UC_ARM64_REG_X3,RAM+0x100)])
        assert rc==bool(pending&mask)
        assert q('I',RAM+0x18)==(pending&~mask if rc else pending)
        assert q('I',RAM+0x100)==(pending if rc else 0x12345678)
        matches.append(dict(pending=hex(pending),mask=hex(mask),return_value=rc,pending_after=hex(q('I',RAM+0x18))))
p('I',0xa02d32768,0xfffffffd);p('I',0xa02d32788,0xffffffff);p('I',0xa02d3274c,1)
p('Q',SP+0xa18,0xa02d3274c)
call(0x40c3d898,0x40c3c4f4,[(A.UC_ARM64_REG_X21,0),(A.UC_ARM64_REG_X20,0xa02d30070)])
assert q('I',0xa02d32768)==0xfffffffd and q('I',0xa02d32788)==0 and q('I',0xa02d3274c)==2
assert posts==[dict(event_object='0xa02d30070',mask='0x20',state=2,worker_result=0)]
# The actual INIT rejection overwrites the shared result, without undoing state2.
call(0x40c3b17c,0x40c3b660,[(A.UC_ARM64_REG_X23,0xa02d3274c),(A.UC_ARM64_REG_X21,0),(A.UC_ARM64_REG_X20,0xa02d30070)])
assert q('I',0xa02d3274c)==2 and q('I',0xa02d32788)==0xffffffff
# The actual HEADER gate still accepts state2 and submits command0x33.
p('Q',SP+0x978,RAM+0x200);p('Q',SP+0x9b0,RAM+0x210);p('Q',SP+0x9e8,RAM+0x220)
p('Q',RAM+0x200,0x89f801000);p('I',RAM+0x210,0x100000);p('I',RAM+0x220,1)
p('Q',RAM+0x230,RAM+0x240)
call(0x40c3b668,0x40c3b710,[(A.UC_ARM64_REG_X23,0xa02d3274c),(A.UC_ARM64_REG_X21,0),(A.UC_ARM64_REG_X20,0xa02d30070),(A.UC_ARM64_REG_X28,RAM+0x230),(A.UC_ARM64_REG_W27,0x33)])
assert commands==[dict(command=0x33,state=2,result=0xffffffff)]
assert q('I',0xa02d3274c)==3 and q('I',0xa02d32788)==0xffffffff
ranges=[(0x40c375e0,0x40c37a54),(0x4017b1ec,0x4017b254),(0x40c3d898,0x40c3d8bc),(0x40c3c4e8,0x40c3c4f4)]
report=dict(passed=True,firmware_sha256=hashlib.sha256(B).hexdigest(),reset=resets,event_consume=matches,
    completion_posts=posts,completion_preserves_old_2768=True,header_command_after_result_overwrite=commands,
    code_ranges=[dict(start=hex(a),end=hex(b),sha256=hashlib.sha256(B[a-BASE:b-BASE]).hexdigest())for a,b in ranges],
    limitation='Reset memsets and event-post are bounded test shims. Flag matching is original RTOS code. This proves persistence/consumption behavior, not that an old event caused the full-run failure.')
(D/'native-ready-state.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS original native completion -> shared-result overwrite -> HEADERS acceptance')
