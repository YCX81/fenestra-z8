"""Replace one test movie inside an isolated regular exFAT volume, within its allocation.
No block devices, mounts, new allocation, FAT changes, or directory creation.
Afterward run fsck -n and the independent read-only extractor.
"""
import os,stat,math,struct,sys,hashlib
from pathlib import Path
script,volume,target,replacement,expected=sys.argv[1:]
volume=Path(volume);assert stat.S_ISREG(volume.stat().st_mode) and volume.suffix=='.raw'
# Keep the existing bounded/checksummed parser; annotate each validated set with
# physical positions so the same reader can locate it without a second search.
code=Path(script).read_text().replace('chunks=[];total=0','chunks=[];locations=[];total=0')
code=code.replace('chunks.append(read(heap*sector+(c-2)*cluster,cluster))','locations.append(heap*sector+(c-2)*cluster);chunks.append(read(locations[-1],cluster))')
code=code.replace('rows.append(row)',"row['entry_offsets']=[locations[(i+k)//cluster]+(i+k)%cluster for k in range(0,len(group),32)];rows.append(row)")
ns={'__name__':'exfat_catalog'};exec(compile(code,script,'exec'),ns)
rows=ns['catalog'](volume)['files'];hits=[r for r in rows if r['path']==target and not r['directory']];assert len(hits)==1
r=hits[0];assert r['valid_bytes']==r['bytes']
new=Path(replacement).read_bytes()
with volume.open('r+b') as f:
 boot=f.read(512);sector=1<<boot[108];cluster=sector<<boot[109]
 fat,_,heap,count,_=struct.unpack_from('<IIIII',boot,80);n=math.ceil(r['bytes']/cluster)
 assert 0<len(new)<=n*cluster
 c=r['first_cluster'];chain=[]
 for j in range(n):
  assert 2<=c<count+2 and c not in chain;chain.append(c)
  if j+1<n:
   if r['contiguous']:c+=1
   else:f.seek(fat*sector+c*4);c=struct.unpack('<I',f.read(4))[0]
 old=bytearray()
 for c in chain:f.seek(heap*sector+(c-2)*cluster);old+=f.read(cluster)
 assert hashlib.sha256(old[:r['bytes']]).hexdigest()==expected,'unexpected source file'
 for j,c in enumerate(chain):
  data=new[j*cluster:(j+1)*cluster]
  if data:f.seek(heap*sector+(c-2)*cluster);assert f.write(data)==len(data)
 group=bytearray()
 for at in r['entry_offsets']:f.seek(at);group+=f.read(32)
 struct.pack_into('<Q',group,40,len(new));struct.pack_into('<Q',group,56,len(new))
 checksum=0
 for j,v in enumerate(group):
  if j not in (2,3):checksum=(((checksum>>1)|((checksum&1)<<15))+v)&0xffff
 struct.pack_into('<H',group,2,checksum)
 for j,at in enumerate(r['entry_offsets']):f.seek(at);assert f.write(group[j*32:j*32+32])==32
 f.flush();os.fsync(f.fileno())
newrow=next(r for r in ns['catalog'](volume)['files'] if r['path']==target)
assert newrow['bytes']==len(new)==newrow['valid_bytes']
print('replaced existing allocated test file',target,len(new),hashlib.sha256(new).hexdigest())
