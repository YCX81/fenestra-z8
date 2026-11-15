from pathlib import Path
import struct,json,hashlib
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import *
P=Path('/path/to/user/fxfix/v53-codec-spi');B=0x40160000;f=Path('/path/to/user/fxfix/v53-root/v53f/q/main.main').read_bytes();sha=hashlib.sha256(f).hexdigest();assert sha=='b0cd7c40c3c6ffdf07b87fcbab20b98fbe9ed0c2a848550855d989b9e64fd8b0'
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(B,(len(f)+4095)&~4095);u.mem_write(B,f)
for a,n in [(0xa02800000,0x20000),(0xa02c96000,0x2000),(0xa02d4c000,0x1000),(0xa10000000,0x100000),(0x0d150000,0x2000)]:u.mem_map(a,n)
w32=lambda a,v:u.mem_write(a,struct.pack('<I',v));w64=lambda a,v:u.mem_write(a,struct.pack('<Q',v));r64=lambda a:struct.unpack('<Q',u.mem_read(a,8))[0]
# Exact F4 observed globals/object; GPIO table address is a declared fixture.
u.mem_write(0xa02d4c908,bytes.fromhex('d89780020a000000010000004000000041756453764d7830010000000a0000000100000000000000'))
u.mem_write(0xa028097d8,bytes.fromhex('4c000000050000000d00000001010001'))
w64(0xa02c96da8,0x0d151014);w64(0xa02813f08,0xa02808b90)
END=B;SP=0xa100f0000;OUT=0xa10000100;events=[];leaf_shim=True
def hook(uc,pc,size,data):
 if pc==0x40c1459c and leaf_shim:
  obj=uc.reg_read(UC_ARM64_REG_X0);p=uc.reg_read(UC_ARM64_REG_X1);d=bytes(uc.mem_read(p,32));tx,rx,nt,nr,mode,timeout=struct.unpack('<QQIIII',d)
  e=dict(kind='serial_leaf',object=hex(obj),tx=hex(tx),rx=hex(rx),tx_words=nt,rx_words=nr,mode=mode,timeout=timeout,tx_hex=bytes(uc.mem_read(tx,2*nt)).hex() if tx else '')
  events.append(e);assert obj==0xa02808b90 and mode==1
  if rx:assert nr==1;uc.mem_write(rx,struct.pack('<H',0x4567))
  uc.reg_write(UC_ARM64_REG_X0,0);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR))
def mem(uc,access,addr,size,value,data):
 if addr<0x40000000:events.append(dict(kind='gpio',address=hex(addr),size=size,value=hex(value)))
u.hook_add(UC_HOOK_CODE,hook);u.hook_add(UC_HOOK_MEM_WRITE,mem)
results=[]
for mode in ['write','read']:
 for reg in [0x15,0x16,0x17]:
  events.clear();u.mem_write(OUT,b'\xa5\xa5');u.reg_write(UC_ARM64_REG_SP,SP);u.reg_write(UC_ARM64_REG_LR,END);u.reg_write(UC_ARM64_REG_X0,reg);u.reg_write(UC_ARM64_REG_X1,OUT if mode=='read' else 0x1234)
  u.emu_start(0x40c7fd08 if mode=='read' else 0x40c7f824,END,count=10000);assert u.reg_read(UC_ARM64_REG_PC)==END
  assert u.reg_read(UC_ARM64_REG_X0)==0
  assert events[0]==dict(kind='gpio',address='0xd151014',size=4,value='0x20000000')
  assert events[-1]==dict(kind='gpio',address='0xd151014',size=4,value='0x20002000')
  serial=[e for e in events if e['kind']=='serial_leaf']
  if mode=='read':
   assert len(serial)==2 and serial[0]['tx_hex']==struct.pack('<H',reg|0x8000).hex() and serial[0]['rx_words']==0
   assert serial[1]['tx_words']==0 and serial[1]['tx']=='0x0' and serial[1]['rx_words']==1
   assert bytes(u.mem_read(OUT,2))==b'\x67\x45'
  else:assert len(serial)==1 and serial[0]['tx_hex']==struct.pack('<HH',reg,0x1234).hex() and serial[0]['rx_words']==0
  results.append(dict(operation=mode,reg=reg,events=list(events)))
res=dict(firmware_sha256=sha,cases=len(results),pass_all=True,results=results,boundary='Actual exact-F codec wrappers, GPIO helper, serial service and descriptor ABI; low-level serial transfer is an explicit leaf shim. GPIO table address 0d151014 is a fixture, not a hardware reading. Full QEMU must independently validate actual RAM GPIO table and RX/IRQ completion.')
(P/'native-tests.json').write_text(json.dumps(res,indent=2)+'\n');print(json.dumps(res,indent=2))
# Execute the actual low-level initializer and native PIO TX helper. The native
# controller bit5 turns RX-only into one zero dummy clock; never infer from name.
u.mem_map(0x0e700000,0x1000)
D=0xa02808b90;T=0xa10000300
prefix=[];leaf_shim=False
for control in [0,0x20]:
 u.mem_write(D,bytes(0x68));w32(D+8,10);u.mem_write(T,struct.pack('<QQIIII',0,OUT,0,1,1,0));w32(0x0e700a0c,control)
 u.reg_write(UC_ARM64_REG_SP,SP);u.reg_write(UC_ARM64_REG_LR,END);u.reg_write(UC_ARM64_REG_X0,D);u.reg_write(UC_ARM64_REG_X1,T)
 # Temporarily disable only our serial leaf hook; these original instructions run.
 u.emu_start(0x40c1459c,0x40c1466c,count=1000)
 assert u.reg_read(UC_ARM64_REG_PC)==0x40c1466c
 counts=struct.unpack('<IIII',bytes(u.mem_read(D+0x28,16)))
 txneeded=struct.unpack('<I',bytes(u.mem_read(D+0x28,4)))[0]
 assert txneeded==bool(control&0x20) and r64(D+0x20)==0
 events.clear();w32(0x0e700a14,0x20);w32(0x0e700a48,64);w32(0x0e700a20,0x84);u.reg_write(UC_ARM64_REG_SP,SP);u.reg_write(UC_ARM64_REG_LR,END);u.reg_write(UC_ARM64_REG_X0,D);u.reg_write(UC_ARM64_REG_X1,4)
 u.emu_start(0x40c15190,END,count=2000);assert u.reg_read(UC_ARM64_REG_PC)==END
 wire=[x for x in events if x.get('address')=='0xe700a00']
 assert len(wire)==bool(control&0x20) and all(x['value']=='0x0' for x in wire)
 prefix.append(dict(controller_c=hex(control),native_tx_pointer=hex(r64(D+0x20)),native_tx_needed=txneeded,native_rx_needed=struct.unpack('<I',bytes(u.mem_read(D+0x38,4)))[0],actual_tx_writes=wire,actual_helper_mode=4,rx_fifo_empty=True,controller_capacity=64))
res['native_rx_dummy_clock_cases']=prefix;res['cases']+=len(prefix)
(P/'native-tests.json').write_text(json.dumps(res,indent=2)+'\n');print(json.dumps(prefix,indent=2))
