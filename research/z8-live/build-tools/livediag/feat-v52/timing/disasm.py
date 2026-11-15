from capstone import *
import struct,sys
b=open('/path/to/user/fxfix/v51d/q/main.main','rb').read();base=0x40160000;m=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
if sys.argv[1]=='xref':
 targets={int(x,16) for x in sys.argv[2:]}
 for off in range(0,len(b)-3,4):
  w=struct.unpack_from('<I',b,off)[0]
  if w>>26 not in (5,37):continue
  imm=w&0x3ffffff
  if imm&(1<<25):imm-=1<<26
  if base+off+imm*4 in targets:
   print('TARGET',hex(base+off+imm*4))
   for y in m.disasm(b[max(0,off-40):off+24],base+max(0,off-40)):print(hex(y.address),y.mnemonic,y.op_str)
else:
 for a,z in zip(sys.argv[1::2],sys.argv[2::2]):
  a,z=int(a,16),int(z,16)
  for y in m.disasm(b[a-base:z-base],a):print(hex(y.address),y.mnemonic,y.op_str)
