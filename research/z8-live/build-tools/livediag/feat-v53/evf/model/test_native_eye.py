"""Original GPIO configuration/read/ISR execution; peripheral inputs are fixtures."""
from pathlib import Path
import hashlib,json,struct
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import *

FW=Path('/path/to/user/fxfix/v52-root/v52g/q/main.main')
b=FW.read_bytes();B=0x40160000
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(B,(len(b)+4095)&~4095);u.mem_write(B,b)
for a,n in [(0xa02800000,0x800000),(0xa10000000,0x100000),(0x0c86f000,0x1000),(0x0d150000,0x3000)]:u.mem_map(a,n)
u.mem_write(0xa02808000,b[0x4b13000:0x4b13000+(0xa02ab5000-0xa02808000)])
END=0x40160000;STACK=0xa100ff000;CFG=0xa10001000;OBJ=0xa028092d8;IRQ=0xa0280c370
def w(a,v,n=8):u.mem_write(a,int(v).to_bytes(n,'little'))
def r(a,n=4):return int.from_bytes(u.mem_read(a,n),'little')
events=[]
def hook(uc,pc,size,data):
    if pc in (0x40c0a0dc,0x40c0a108):uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR))
    elif pc==0x40c948b8:
        events.append([uc.reg_read(UC_ARM64_REG_X0),uc.reg_read(UC_ARM64_REG_X1)])
        uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR))
u.hook_add(UC_HOOK_CODE,hook)
def call(pc,*args):
    u.reg_write(UC_ARM64_REG_SP,STACK);u.reg_write(UC_ARM64_REG_LR,END)
    for reg,v in zip([UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2],args):u.reg_write(reg,v)
    u.emu_start(pc,END,count=10000);assert u.reg_read(UC_ARM64_REG_PC)==END
    return u.reg_read(UC_ARM64_REG_W0)
assert [r(OBJ),r(OBJ+4),r(OBJ+8),r(OBJ+32,8)]==[44,3,13,IRQ]
assert [r(IRQ+4),r(IRQ+8),r(IRQ+12),r(IRQ+16,8)]==[1,4,68,0x40c162e0]
w(0xa02c96d70+8,0x0c86f000);w(0xa02c96d80+3*8,0x0d15100c)
w(0xa02e815c8,OBJ);w(0xa02e815dc,1,4)
u.mem_write(CFG,struct.pack('<IIQ',2,0,0x40c948b8))
cases=[]
for initial in (0,1):
    u.mem_write(0x0c86f000,bytes(0x1000));w(0x0d15100c,initial<<13,4)
    assert call(0x40c15e80,OBJ,CFG)==0
    assert r(0x0c86f014)&0x10 == (0x10 if initial==0 else 0)
    assert r(0x0c86f018)&0x10
    assert call(0x40c15c80,OBJ)==initial
    assert call(0x40c94538)==1-initial
    for level in (1-initial,initial):
        w(0x0d15100c,level<<13,4);w(0x0c86f008,0x10,4)
        events.clear();call(0x40c16358,OBJ)
        assert events==[[68,level]]
        assert r(0x0c86f010)==0x10
        assert r(0x0c86f014)&0x10 == (0x10 if level==0 else 0)
        assert call(0x40c94538)==1-level
        cases.append(dict(initial=initial,level=level,callback=events[:],ack=hex(r(0x0c86f010)),next_polarity=hex(r(0x0c86f014))))
out=dict(passed=True,original_main_sha256=hashlib.sha256(b).hexdigest(),cases=cases,
    scope='Actual original ARM config/read/ISR and eye inversion; MMIO levels, register pointer table and RTOS exclusion are fixtures. Physical timing and whole GUI behavior are not proved.')
Path('/path/to/user/fxfix/v53-evf/native-eye-arm.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps(out,indent=2))
