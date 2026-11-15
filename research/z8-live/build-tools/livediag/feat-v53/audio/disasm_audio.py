from pathlib import Path
import capstone,struct
P=Path(__file__).resolve().parent
f=(P.parents[1]/'feat-v50/integration-writer/v51d/q/main.main').read_bytes();base=0x40160000
cs=capstone.Cs(capstone.CS_ARCH_ARM64,capstone.CS_MODE_ARM)
for name,lo,hi in [('driver',0x40c80000,0x40c85200),('service',0x40d23200,0x40d26000)]:
 (P/(name+'-full.txt')).write_text('\n'.join(f'{i.address:08x}: {i.mnemonic:8} {i.op_str}' for i in cs.disasm(f[lo-base:hi-base],lo))+'\n')
for a,n,fmt in [(0x42ab6760,4,'I'),(0x42ab67a7,7,'B'),(0x42ab67ad,2,'B'),(0x42ab6782,37,'B'),(0x41668598,10,'Q')]: print(hex(a),struct.unpack_from('<'+fmt*n,f,a-base))
# Cross references: mov wN, #0x17 followed within 8 instructions by movk wN,#0x4318,lsl#16.
for p in range(0,len(f)-40,4):
 w=struct.unpack_from('<I',f,p)[0]
 if w&0xffffffe0!=0x528002e0:continue
 reg=w&31
 for q in range(p+4,p+40,4):
  if struct.unpack_from('<I',f,q)[0]==0x72a86300+reg:
   print('EVENT17',hex(p+base));print('\n'.join(f'{i.address:08x}: {i.mnemonic:8} {i.op_str}' for i in cs.disasm(f[max(0,p-96):p+48],base+max(0,p-96))))
