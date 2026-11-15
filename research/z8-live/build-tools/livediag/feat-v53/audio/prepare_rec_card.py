"""Private emulator fixture only: real guest JPG + nonzero LVJB C-writer MOV.
Builds an isolated regular-file card; never accesses a mounted device.
"""
from pathlib import Path
import sys,struct,json,hashlib,math
sys.path.insert(0,'/path/to/user/fxfix/v52-root/driver-g/retry-driver')
import card_export,exfat_put
D=Path('/path/to/user/fxfix/v53-audio/rec-candidate');image=D/'card.raw'
assert image.is_file() and image.stat().st_size==8589934592
jpgpath=Path('/path/to/user/.local/share/nikon-z8-qemu/live-audio/regress/runs/v52g-final-rec/files/DCIM/100NCZ_8/DSC_0001.JPG')
jpg=jpgpath.read_bytes();movie=bytearray((D/'cfr-audio.MOV').read_bytes())
a=movie.rfind(b'LVJB');pcm_at,frames,hz=struct.unpack_from('<III',movie,a+4);assert hz==48000 and frames==152152
# The host writer test's JPEG stub used 45 fixed 66.667ms frames while its CFR
# AV window is 95/29.97. Make this test-only JPEG clock span the existing PCM.
atom=len(movie)-struct.unpack_from('<I',movie,len(movie)-8)[0];preview_n,dt=struct.unpack_from('<II',movie,atom+20)
new_dt=math.ceil(frames*1000000/hz/preview_n);struct.pack_into('<I',movie,atom+24,new_dt)
(D/'rec-fixture.MOV').write_bytes(movie)
manifest=f'native=A:\\DCIM\\100NCZ_8\\LIV_9013.JPG\nsize={len(jpg)}\nuuid=11111111-2222-3333-4444-555555555555\nvolume_generation=1\n'.encode()
rows=card_export.catalog(image)['files'];folder=next(x for x in rows if x['path']=='DCIM/100NCZ_8');assert folder['contiguous']
assert not any(x['path'].startswith(('LIVE/','LIVEDATA/100NCZ_8')) for x in rows)
with image.open('r+b') as f:
 part=card_export.volume_offset(image);f.seek(part);v=f.read(512);fat,_,heap,count,root=struct.unpack_from('<IIIII',v,80);ss=1<<v[108];sc=ss<<v[109];co=lambda c:part+heap*ss+(c-2)*sc
 f.seek(co(root));rd=bytearray(f.read(sc));be=next(rd[i:i+32] for i in range(0,len(rd),32) if rd[i]==0x81);bm,blen=struct.unpack_from('<IQ',be,20);f.seek(co(bm));bitmap=bytearray(f.read(blen))
 def alloc(n):
  c=next(c for c in range(2,count+2-n) if all(not bitmap[(c+k-2)//8]&(1<<((c+k-2)%8)) for k in range(n)))
  for k in range(n):bitmap[(c+k-2)//8]|=1<<((c+k-2)%8)
  return c
 def blob(data):
  c=alloc(max(1,(len(data)+sc-1)//sc));f.seek(co(c));f.write(data);return c
 def directory(data):
  entries=bytearray(sc);i=0
  for name,value in data.items():
   if isinstance(value,dict):c=directory(value);attr=0x10;size=sc
   else:c=blob(value);attr=0x20;size=len(value)
   e=exfat_put._entry_set(name,attr,c,size);assert i+len(e)+32<=sc;entries[i:i+len(e)]=e;i+=len(e)
  return blob(entries)
 f.seek(co(folder['first_cluster']));fd=bytearray(f.read(folder['bytes']));i=0
 while fd[i]:
  n=fd[i+1]+1 if fd[i]==0x85 else 1
  if fd[i]==0x85:
   c=struct.unpack_from('<I',fd,i+52)[0];r=next(x for x in rows if x['path'].startswith('DCIM/100NCZ_8/') and x['first_cluster']==c);assert r['contiguous'] and not r['directory']
   for k in range((r['bytes']+sc-1)//sc):bitmap[(c+k-2)//8]&=~(1<<((c+k-2)%8))
   for j in range(n):fd[i+j*32]&=0x7f
  i+=n*32
 # Retired all prior DCF media/cache; one actual photo remains so playback selects it.
 e=exfat_put._entry_set('LIV_9013.JPG',0x20,blob(jpg),len(jpg));assert i+len(e)+32<=len(fd);fd[i:i+len(e)]=e;f.seek(co(folder['first_cluster']));f.write(fd)
 j=0
 while rd[j]:j+=32*(rd[j+1]+1) if rd[j]==0x85 else 32
 for name,data in [('LIVE',{'100NCZ_8':{'LIV_9013.MOV':bytes(movie)}}),('LIVEDATA',{'100NCZ_8':{'LIV_9013.MAN':manifest}})]:
  old=next((x for x in rows if x['path']==name and x['directory']),None)
  if old:
   assert old['bytes']==sc
   f.seek(co(old['first_cluster']));od=bytearray(f.read(sc));k=0
   while od[k]:k+=32*(od[k+1]+1) if od[k]==0x85 else 32
   for child,values in data.items():
    e=exfat_put._entry_set(child,0x10,directory(values),sc);assert k+len(e)+32<=sc;od[k:k+len(e)]=e;k+=len(e)
   f.seek(co(old['first_cluster']));f.write(od)
  else:
   e=exfat_put._entry_set(name,0x10,directory(data),sc);assert j+len(e)+32<=sc;rd[j:j+len(e)]=e;j+=len(e)
 f.seek(co(root));f.write(rd);f.seek(co(bm));f.write(bitmap)
report=card_export.catalog(image);pcm=bytes(movie[pcm_at:pcm_at+frames*6]);pcm16=b''.join(pcm[i+1:i+3] for i in range(0,len(pcm),3));samples=struct.unpack('<'+'h'*(len(pcm16)//2),pcm16)
report.update(fixture_scope='Actual guest photo; C-writer nonzero LVJB audio; private preview-dt adjusted to cover its existing CFR PCM window',source_jpg=str(jpgpath),jpg_sha256=hashlib.sha256(jpg).hexdigest(),movie_sha256=hashlib.sha256(movie).hexdigest(),preview_frames=preview_n,old_dt_us=dt,new_dt_us=new_dt,pcm_frames=frames,pcm16_sha256=hashlib.sha256(pcm16).hexdigest(),pcm16_nonzero=sum(x!=0 for x in samples),pcm16_rms=math.sqrt(sum(x*x for x in samples)/len(samples)))
(D/'fixture.json').write_text(json.dumps(report,indent=2)+'\n');print(report['fixture_scope'],report['pcm_frames'],report['pcm16_nonzero'])
