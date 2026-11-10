import sys,struct,hashlib
from pathlib import Path
from capstone import Cs,CS_ARCH_ARM64,CS_MODE_ARM
B=0x40160000
d=Path('bg34.main').read_bytes(); md=Cs(CS_ARCH_ARM64,CS_MODE_ARM);md.skipdata=True
mode=sys.argv[2]; args=sys.argv[3:]
if mode=='dis':
 for spec in args:
  a,n=(int(x,0) for x in spec.split(':'));print('\nRANGE',hex(a))
  for i in md.disasm(d[a-B:a-B+n*4],a):print(f'{i.address:08x}: {i.mnemonic:8s} {i.op_str}')
elif mode=='refs':
 targets={int(x,0) for x in args}
 for off in range(0,len(d)-4,4):
  w=struct.unpack_from('<I',d,off)[0];a=B+off
  if w&0x7c000000==0x14000000:
   imm=w&0x3ffffff;imm=imm-(1<<26) if imm&(1<<25) else imm;t=a+4*imm
   if t in targets:print(hex(a),'BL' if w>>31 else 'B',hex(t))
 for t in targets:
  for fmt in ['<I','<Q']:
   p=struct.pack(fmt,t);off=0
   while True:
    off=d.find(p,off)
    if off<0:break
    print(hex(B+off),'pointer'+fmt,hex(t));off+=1
elif mode=='hash':print(len(d),hashlib.sha256(d).hexdigest())
elif mode=='hex':
 for spec in args:
  a,n=(int(x,0) for x in spec.split(':'))
  for at in range(a,a+n,16):
   b=d[at-B:at-B+16]; print(hex(at),b.hex(' '),''.join(chr(x) if 32<=x<127 else '.' for x in b))
elif mode=='const':
 targets={int(x,0) for x in args}; ws=struct.unpack('<%dI'%(len(d)//4),d[:len(d)//4*4])
 for j,w in enumerate(ws):
  if w&0x7f800000!=0x52800000:continue
  rd=w&31;v=((w>>5)&65535)<<(((w>>21)&3)*16)
  for k in range(j+1,min(j+25,len(ws))):
   y=ws[k]
   if y&0x7f800000==0x72800000 and y&31==rd:
    sh=((y>>21)&3)*16;v=(v&~(65535<<sh))|((y>>5)&65535)<<sh
    if v in targets:print(hex(B+j*4),hex(B+k*4),'->',hex(v));break
elif mode=='sw':
 for a in range(0x42ad1114,0x42ad2014,40):
  o=a-B; print(hex(a),d[o:o+36].split(b'\0')[0].decode(errors='replace'),hex(struct.unpack_from('<I',d,o+36)[0]))
elif mode=='bit':
 bit=int(args[0],0)
 for off in range(0,min(len(d),0x1400000),4):
  w=struct.unpack_from('<I',d,off)[0]
  if w&0x7e000000==0x36000000 and (((w>>19)&31)|((w>>31)<<5))==bit:
   for i in md.disasm(d[off:off+4],B+off):print(hex(i.address),i.mnemonic,i.op_str)
elif mode=='findins':
 import re
 a=int(args[0],0);end=int(args[1],0);pat=re.compile(args[2])
 ins=list(md.disasm_lite(d[a-B:end-B],a))
 for j,i in enumerate(ins):
  if pat.search(i[2]+' '+i[3]):
   print('\nMATCH')
   for z in ins[max(0,j-3):j+5]:print(hex(z[0]),z[2],z[3])
