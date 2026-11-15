"""Replace files in an isolated regular-file REC card with unmodified F capture media.
No mounted card/device. MOV/JPG bytes unchanged; private MAN basename changes only.
"""
from pathlib import Path
import json,struct,hashlib,sys
sys.path.insert(0,'/path/to/user/fxfix/v52-root/driver-g/retry-driver')
import card_export,exfat_put
D=Path('/path/to/user/fxfix/v53-playback/rec-newmov-g');image=D/'rec-card.raw'
assert image.is_file() and not image.is_symlink() and image.stat().st_size==8589934592
src=Path('/path/to/user/.local/share/nikon-z8-qemu/live-audio/regress/runs/v53f-final-counter-audio-capture-rec/files')
jpg=(src/'DCIM/100NCZ_8/DSC_0001.JPG').read_bytes();newsrc=Path('/path/to/user/.local/share/nikon-z8-qemu/live-audio/regress/runs/v53g-final2-counter-audio-capture-rec/files');mov=(newsrc/'LIVE/100NCZ_8/DSC_0001.MOV').read_bytes();man=(newsrc/'LIVEDATA/100NCZ_8/DSC_0001.MAN').read_bytes().replace(b'DSC_0001',b'LIV_9013')
assert hashlib.sha256(mov).hexdigest()=='340fbc5a1749b0aa8358602e5dc4af776470b67966b3f88e48fc24ec58489ed1'
files={'DCIM/100NCZ_8/LIV_9013.JPG':jpg,'LIVE/100NCZ_8/LIV_9013.MOV':mov,'LIVEDATA/100NCZ_8/LIV_9013.MAN':man};rows=card_export.catalog(image)['files']
with image.open('r+b') as f:
 part=card_export.volume_offset(image);f.seek(part);v=f.read(512);fat,_,heap,count,root=struct.unpack_from('<IIIII',v,80);ss=1<<v[108];sc=ss<<v[109];co=lambda c:part+heap*ss+(c-2)*sc
 f.seek(co(root));rd=bytearray(f.read(sc));be=next(rd[i:i+32] for i in range(0,len(rd),32) if rd[i]==0x81);bm,blen=struct.unpack_from('<IQ',be,20);f.seek(co(bm));bitmap=bytearray(f.read(blen))
 def alloc(n):
  c=next(c for c in range(2,count+2-n) if all(not bitmap[(c+k-2)//8]&(1<<((c+k-2)%8)) for k in range(n)))
  for k in range(n):bitmap[(c+k-2)//8]|=1<<((c+k-2)%8)
  return c
 for path,data in files.items():
  row=next(x for x in rows if x['path']==path);parent=next(x for x in rows if x['path']==str(Path(path).parent));assert row['contiguous'] and parent['contiguous']
  f.seek(co(parent['first_cluster']));entry=bytearray(f.read(parent['bytes']));i=0
  while i<len(entry) and entry[i]:
   if entry[i]==0x85 and struct.unpack_from('<I',entry,i+52)[0]==row['first_cluster']:break
   i+=32*(entry[i+1]+1) if entry[i]==0x85 else 32
  assert i<len(entry) and entry[i]==0x85
  c=alloc((len(data)+sc-1)//sc);f.seek(co(c));f.write(data)
  replacement=exfat_put._entry_set(Path(path).name,0x20,c,len(data));assert len(replacement)==32*(entry[i+1]+1)
  entry[i:i+len(replacement)]=replacement;f.seek(co(parent['first_cluster']));f.write(entry)
  for k in range((row['bytes']+sc-1)//sc):bitmap[(row['first_cluster']+k-2)//8]&=~(1<<((row['first_cluster']+k-2)%8))
 f.seek(co(bm));f.write(bitmap)
for path,data in files.items():
 out=D/Path(path).name;card_export.extract(image,path,out);assert out.read_bytes()==data
at=len(mov)-struct.unpack('<I',mov[-8:-4])[0];n,dt=struct.unpack_from('<II',mov,at+20)
report={'scope':'Unchanged actual G capture MOV with original F JPG; isolated card and basename/MAN native binding only. G MAN uuid preserved, original contacts/driver/assertions unchanged.','source':str(src),'preview_frames':n,'preview_dt_us':dt,'preview_us':n*dt,'media':{k:{'bytes':len(v),'sha256':hashlib.sha256(v).hexdigest()} for k,v in files.items()}}
(D/'fixture.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
