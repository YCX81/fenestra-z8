from pathlib import Path
import io,json,struct,hashlib
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import *
P=Path(__file__).resolve().parent;main=(P/'v53f.main').read_bytes();assert hashlib.sha256(main).hexdigest()=='b0cd7c40c3c6ffdf07b87fcbab20b98fbe9ed0c2a848550855d989b9e64fd8b0'
e=ELFFile(io.BytesIO((P/'live.elf').read_bytes()));sy={s.name:s['st_value']for s in e.get_section_by_name('.symtab').iter_symbols()}
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);B=0x40160000;u.mem_map(B,(len(main)+4095)&~4095);u.mem_write(B,main);u.mem_map(0x4e000000,0x10000);END=0x4e001000;events=[]
def ret(v):u.reg_write(UC_ARM64_REG_X0,v);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
def hook(uc,pc,size,data):
 if pc==sy['live_ui_engine_get']:ret(0)
 elif pc==0x40d2c408:events.append(dict(alloc_bytes=uc.reg_read(UC_ARM64_REG_W0),flags=uc.reg_read(UC_ARM64_REG_W1)));ret(0)
u.hook_add(UC_HOOK_CODE,hook);u.reg_write(UC_ARM64_REG_SP,0x4e00f000);u.reg_write(UC_ARM64_REG_LR,END);u.emu_start(sy['live_runtime_boot'],END,count=1000);assert events==[dict(alloc_bytes=23744,flags=0)]
branches={}
for a,name,link in [(0x40e30e3c,'live_shutter_dispatch',False),(0x40f6cff0,'live_shutter_prepare_entry',True),(0x40f6e174,'live_shutter_fire_entry',True)]:
 w=struct.unpack_from('<I',main,a-B)[0];assert w>>26==(37 if link else 5);d=w&0x3ffffff;d-=0x4000000 if d&0x2000000 else 0;t=a+4*d;assert t==sy[name];branches[hex(a)]=dict(symbol=name,target=hex(t))
assert 0x4019f020<=sy['live_shutter_dispatch']<0x4019ff40
assert sy['live_stock_shutter_prepare']==0x40f9cccc and sy['live_stock_shutter_fire']==0x40f9cd54
r=dict(passed=True,main_sha256=hashlib.sha256(main).hexdigest(),native_boot_allocation=events,branches=branches,passive_wrappers_keep_stock_targets=True,boundary='Actual final compiled boot executes until allocator failure, validating requested runtime size. Registry getter/allocator are shims. Does not allocate camera memory or run live audio hardware.')
(P/'final-abi.json').write_text(json.dumps(r,indent=2)+'\n');print(r)
