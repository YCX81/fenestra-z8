#!/usr/bin/env python3
from pathlib import Path
import struct,json,sys
codec=int(sys.argv[1]) if len(sys.argv)>1 else 1
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X30,UC_ARM64_REG_SP
D=Path(__file__).resolve().parent
symbols={s[2]:int(s[0],16)for l in (D/f'retained-codec{codec}.nm').read_text().splitlines() if len(s:=l.split())==3}
CODE=0x40100000; E=0xa11000000; W=0x89f5ff000; INST=0xa12abc000;STACK=0xa13000000;ARENA=0x8a2e9b000;STOP=0x40200000
cases=[('valid retained owner',None,1),('null encoder',('arg',0),0),('null hw',('q',E+40,0),0),('bad magic',('i',W,0),0),('close failed',('i',W+1053564,1),0),('busy',('i',W+32,1),0),('release pending',('i',W+36,1),0),('VDI off',('q',0xa02d3d980,0),0),('VDI different',('q',0xa02d3d980,0x17000000),0),('own client cleared',('b',0xa02e98484,255),0),('own client moved',('b',0xa02e98484,1),0),('invalid stream',('i',W+4,2),0),('own slot closed',('i',0xa02d30118,0),0),('other slot used',('i',0xa02d30124,1),0),('arena changed',('q',0xa02d3d950,ARENA+4096),0),('worker ended',('i',0xa02d3274c,5),0),('instance cleared',('q',0xa02d32758,0),0),('instance replaced',('q',0xa02d32758,INST+4096),0),('binding absent',('q',W+1057016,0),0),('instance released',('i',INST,0),0),('instance wrong codec',('i',INST+12,3 if codec else 1),0)]
cases += [(f'other client {n} mapped',('b',0xa02e98480+n,1),0)for n in range(4)]
results=[]
for name,change,expected in cases:
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
 for addr,n in [(CODE,0x10000),(STOP,4096),(E,4096),(W,0x103000),(INST,8192),(STACK,0x10000),(0xa02d30000,0x10000),(0xa02e98000,4096)]:u.mem_map(addr,n)
 u.mem_write(CODE,(D/f'retained-codec{codec}.bin').read_bytes())
 def p(k,a,v):u.mem_write(a,struct.pack({'q':'<Q','i':'<I','b':'<B'}[k],v))
 for k,a,v in [('q',E+40,W),('i',W,0x57354c31),('i',W+4,0),('q',W+48,ARENA),('q',W+1057016,INST),('q',0xa02d3d980,0x16010000),('q',0xa02d3d950,ARENA),('i',0xa02d30118,1),('i',0xa02d3274c,4),('q',0xa02d32758,INST),('i',INST,1),('i',INST+12,1 if codec else 3)]:p(k,a,v)
 u.mem_write(0xa02e98480,b'\xff\xff\xff\xff\0')
 u.mem_write(0xa02d3d9d8,struct.pack('<Q',ARENA+0x200000)) # partition, not arena base
 arg=E
 if change:
  if change[0]=='arg':arg=change[1]
  else:p(*change)
 writes=[]
 u.hook_add(UC_HOOK_MEM_WRITE,lambda uc,access,address,size,value,user: writes.append(address) if not STACK<=address<STACK+0x10000 else None)
 u.reg_write(UC_ARM64_REG_X0,arg);u.reg_write(UC_ARM64_REG_X30,STOP);u.reg_write(UC_ARM64_REG_SP,STACK+0xfff0)
 u.emu_start(symbols['live_encoder_retained'],STOP,count=100000)
 got=u.reg_read(UC_ARM64_REG_X0);assert got==expected,(name,got,expected);assert not writes,(name,writes)
 results.append({'case':name,'expected':expected,'actual':got,'nonstack_memory_writes':0})
(D/f'retained-arm-codec{codec}-tests.json').write_text(json.dumps({'source':'actual compiled live_w5enc.c retained/alive ARM, real fixed RAM addresses/layout','tests':results,'count':len(results),'physical_hardware_verified':False},indent=2)+'\n')
print(f'actual ARM owner helper: {len(results)} checks passed, no nonstack writes')
