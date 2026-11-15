from pathlib import Path
import struct
BASE=0x40160000
SOURCE=Path(__file__).with_name('v52g.main')
TABLES=[(0x43a21a70,643),(0x43a3ce90,643),(0x43a479cc,533),(0x43a507d0,588),(0x43a73ab8,171),(0x43a76894,171),(0x43a79670,139),(0x43a7bbcc,151)]
def offset(a):
 if 0xa02808000<=a<0xa02ab5000:return a-0xa02808000+0x4b13000
 if 0xa02ab5000<=a<0xa02af9950:return a-0xa02ab5000+0x4dc0000
 return a-BASE
def rows(b,table):
 a,n=TABLES[table]
 return {struct.unpack_from('<I',b,a+j*68-BASE+56)[0]:{'address':a+j*68,'words':struct.unpack_from('<17I',b,a+j*68-BASE),'L':struct.unpack_from('<4h',b,a+j*68-BASE+24),'P':struct.unpack_from('<4h',b,a+j*68-BASE+32)} for j in range(n)}
def mapping(b):
 result=[]
 for i in range(82):
  p=struct.unpack_from('<Q',b,offset(0x42ae79a0)+i*8)[0];d=b[offset(p):offset(p)+16];n=d[1];r=struct.unpack_from('<Q',d,8)[0]
  for j in range(n):
   v=struct.unpack_from('<20I',b,offset(r)+j*80)
   result.append({'group':i,'index':j,'address':r+j*80,'photo':v[8:12],'video':v[12:16]})
 return result
if __name__=='__main__':
 b=SOURCE.read_bytes();t=[rows(b,i) for i in range(8)]
 for m in mapping(b):
  if m['video'][0] not in t[0]:continue
  print(m['group'],m['index'],' '.join(hex(x) for x in m['photo']),'|',' '.join(hex(x) for x in m['video']))
  for i in (0,2,3):
   pr=t[i].get(m['photo'][i]);vr=t[i].get(m['video'][i]);print(' ',i,'photo',None if pr is None else (pr['L'],pr['P']),'video',None if vr is None else (vr['L'],vr['P']))
