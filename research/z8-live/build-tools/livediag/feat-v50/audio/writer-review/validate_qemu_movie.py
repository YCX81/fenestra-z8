#!/usr/bin/env python3
"""Validate the full MOV produced by the final firmware QEMU capture."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('movie',type=Path)
p.add_argument('--manifest',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();d=a.movie.read_bytes()
u=lambda at:struct.unpack_from('>I',d,at)[0]
def boxes(lo,hi):
    while lo<hi:
        assert lo+8<=hi
        n,t=struct.unpack_from('>I4s',d,lo)
        assert n>=8 and lo+n<=hi,(lo,n,hi)
        yield t,lo,n
        lo+=n
    assert lo==hi
containers={b'moov',b'trak',b'mdia',b'minf',b'stbl',b'udta',b'NCDT'}
def walk(lo,hi,path=()):
    for t,p,n in boxes(lo,hi):
        yield path+((t,p,n),)
        if t in containers:yield from walk(p+8,p+n,path+((t,p,n),))
nodes=list(walk(0,len(d)));top=list(boxes(0,len(d)))
assert [t for t,_,_ in top]==[b'ftyp',b'mdat',b'moov',b'free']
assert d[20:24]==b'niko'
tracks=[p for t,p,n in (node[-1] for node in nodes) if t==b'trak']
def find(t,kind):return next(node[-1] for node in nodes if node[-1][0]==kind and any(v[0]==b'trak' and v[1]==t for v in node))
kind=lambda t:d[find(t,b'hdlr')[1]+16:find(t,b'hdlr')[1]+20]
assert sorted(kind(t) for t in tracks) in ([b'meta',b'vide'],[b'meta',b'soun',b'vide'])
def samples(t):
    p=find(t,b'stsz')[1];stride,count=struct.unpack_from('>II',d,p+12)
    sizes=[stride]*count if stride else list(struct.unpack_from('>'+str(count)+'I',d,p+20))
    p=find(t,b'stco')[1];offsets=list(struct.unpack_from('>'+str(u(p+12))+'I',d,p+16))
    p=find(t,b'stsc')[1];entries=[struct.unpack_from('>III',d,p+16+i*12) for i in range(u(p+12))]
    assert entries[0][0]==1 and all(x[2]==1 for x in entries)
    assert all(entries[i][0]<entries[i+1][0] for i in range(len(entries)-1))
    out=[];chunks=[];starts=[];idx=0
    for j,off in enumerate(offsets,1):
        row=next(x for x in reversed(entries) if x[0]<=j);num=row[1]
        starts.append(idx+1);chunks.append(num)
        for _ in range(num):
            assert idx<len(sizes);n=sizes[idx]
            assert top[1][1]+8<=off and off+n<=sum(top[1][1:])
            out.append(d[off:off+n]);idx+=1;off+=n
    assert idx==len(sizes)
    return out,chunks,starts
v=next(t for t in tracks if kind(t)==b'vide')
vs,chunks,starts=samples(v)
sync=find(v,b'stss')[1]
sync=list(struct.unpack_from('>'+str(u(sync+12))+'I',d,sync+16))
assert starts==sync and 1<=len(vs)<=128
nal_counts=[]
for index,sample in enumerate(vs,1):
    p=0;types=[]
    while p<len(sample):
        assert p+4<=len(sample);n=int.from_bytes(sample[p:p+4],'big')
        assert n>=2 and p+4+n<=len(sample)
        unit=sample[p+4:p+4+n];types.append((unit[0]>>1)&63);p+=4+n
    assert p==len(sample) and types[0]==35 and types.count(35)==1
    assert sample[:6]==b'\0\0\0\x03\x46\x01'
    assert sample[6]==(0x10 if index in sync else 0x30)
    assert any(t<=31 for t in types);nal_counts.append(len(types))
m=next(t for t in tracks if kind(t)==b'meta')
assert samples(m)[0]==[b'\0\0\0\x09\0\0\0\x01\xff']
manifest=a.manifest.read_text();uuid=re.search(r'^uuid=(.+)$',manifest,re.M).group(1)
assert len(uuid)==36 and uuid.encode() in d
assert b'com.apple.quicktime.content.identifier' in d and b'com.apple.quicktime.still-image-time' in d
nc=next((p,n) for t,p,n in (node[-1] for node in nodes) if t==b'NCDT')
children={t:(p,n) for t,p,n in boxes(nc[0]+8,sum(nc))}
assert set(children)=={b'NCHD',b'NCTG',b'NCTH',b'NCVW',b'NCM1',b'NCM2',b'NCDB'}
p,n=children[b'NCTG'];at=p+8;tags={}
while at<p+n:
    tag,typ,count=struct.unpack_from('>IHH',d,at);length=count*(0,1,1,2,4,8,1,1,2,4,8)[typ]
    tags[tag]=d[at+8:at+8+length];at+=8+length
assert at==p+n
assert tags[0x101]==struct.pack('>4H',160,120,1280,720)
assert tags[0x102]==struct.pack('>8H',640,360,1920,1080,0,0,0,0)
images=[]
for tag,wh in zip((b'NCTH',b'NCVW',b'NCM1',b'NCM2'),((160,120),(1280,720),(640,360),(1920,1080))):
    p,n=children[tag];jpeg=d[p+8:p+n]
    assert jpeg[:2]==b'\xff\xd8' and jpeg[-2:]==b'\xff\xd9'
    q=2
    while jpeg[q:q+2]!=b'\xff\xc0':
        assert q+4<len(jpeg);q+=2+int.from_bytes(jpeg[q+2:q+4],'big')
    hh,ww=struct.unpack_from('>HH',jpeg,q+5);assert (ww,hh)==wh
    sampling=[jpeg[q+11],jpeg[q+14],jpeg[q+17]]
    if len(tags)==19:assert sampling==[0x21,0x11,0x11]
    images.append({'tag':tag.decode(),'width':ww,'height':hh,'bytes':n-8,'sampling':sampling})
assert d[-4:]==b'LVJT'
audio=next((t for t in tracks if kind(t)==b'soun'),None)
audio_chunks=samples(audio)[1] if audio else []
if audio:assert max(audio_chunks)<=24024
report={'status':'PASS','movie_sha256':hashlib.sha256(d).hexdigest(),'bytes':len(d),
        'niko':True,'video_frames':len(vs),'gop_chunks':chunks,'aud_per_frame':1,
        'apple_uuid':uuid,'apple_still_image_time':True,'ncdt_jpegs':images,
        'audio_track':bool(audio),'audio_chunks':audio_chunks,'physical_camera_verified':False}
a.output.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
