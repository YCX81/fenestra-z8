import struct,sys
from capstone import *
b=open('/path/to/user/fxfix/v51d/q/main.main','rb').read();base=0x40160000;regs={};m=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
lo=int(sys.argv[1],16);hi=int(sys.argv[2],16) if len(sys.argv)>2 else lo+1
for off in range(0,0x2b00000,4):
 w=struct.unpack_from('<I',b,off)[0];rd=w&31
 if w&0x7f800000==0x52800000:regs[rd]=(w>>5&65535)<<((w>>21&3)*16)
 elif w&0x7f800000==0x72800000 and rd in regs:
  sh=(w>>21&3)*16;regs[rd]=(regs[rd]&~(65535<<sh))|((w>>5&65535)<<sh)
  if sh==(32 if lo>0xffffffff else 16) and lo<=regs[rd]<hi:
   print('VALUE',hex(regs[rd]))
   for i in m.disasm(b[off-24:off+40],base+off-24):print(hex(i.address),i.mnemonic,i.op_str)
