"""Diagnostic NCTG/JPEG split only; never a production donor generator."""
from pathlib import Path
import struct,json,hashlib
R=Path(__file__).resolve().parent/'parser'
def boxes(d):
 p=0
 while p<len(d):
  n,t=struct.unpack_from('>I4s',d,p);assert n>=8 and p+n<=len(d)
  yield t,d[p+8:p+n];p+=n
 assert p==len(d)
def box(t,d):return struct.pack('>I4s',len(d)+8,t)+d
def recs(d):
 p=0;w=(0,1,1,2,4,8,1,1,2,4,8)
 while p<len(d):
  t,k,c=struct.unpack_from('>IHH',d,p);n=8+c*w[k];assert p+n<=len(d)
  yield t,d[p:p+n];p+=n
 assert p==len(d)
def ncdt(d):return dict(boxes(dict(boxes(dict(boxes(dict(boxes(d))[b'moov']))[b'udta']))[b'NCDT']))
def replace(d,path,payload):
 out=[];count=0
 for t,b in boxes(d):
  if t==path[0]:
   count+=1;b=replace(b,path[1:],payload) if len(path)>1 else payload
  out.append(box(t,b))
 assert count==1,(path,count)
 return b''.join(out)
def sh(d):return hashlib.sha256(d).hexdigest()
a=(R/'HOST_4K.MOV').read_bytes();b=(R/'LIV_9013-META.MOV').read_bytes();aa=ncdt(a);bb=ncdt(b);ta=dict(recs(aa[b'NCTG']));tb=dict(recs(bb[b'NCTG']))
variants={}
# A: All original donor fields except current identity/geometry values. Keep current JPEGs.
# Preserve donor identity too, so the isolated factor is entire NCTG metadata.
fields=dict(tb)
for tag in (0x22,0x23,0x101,0x102):fields[tag]=ta[tag]
variants['native-fields.MOV']=replace(a,[b'moov',b'udta',b'NCDT',b'NCTG'],b''.join(fields.values()))
# B: Current seven fields, dimensions matching donor JPEGs; only JPEG encoding/content changes.
images=dict(ta)
for tag in (0x101,0x102):images[tag]=tb[tag]
out=replace(a,[b'moov',b'udta',b'NCDT',b'NCTG'],b''.join(images.values()))
for tag in (b'NCTH',b'NCVW',b'NCM1',b'NCM2'):out=replace(out,[b'moov',b'udta',b'NCDT',tag],bb[tag])
variants['native-jpegs.MOV']=out
report=[]
for name,d in variants.items():
 top_a=list(boxes(a));top_d=list(boxes(d));assert [(t,v) for t,v in top_a if t!=b'moov']==[(t,v) for t,v in top_d if t!=b'moov']
 ma=dict(top_a)[b'moov'];md=dict(top_d)[b'moov'];assert [(t,v) for t,v in boxes(ma) if t!=b'udta']==[(t,v) for t,v in boxes(md) if t!=b'udta']
 ua=dict(boxes(ma))[b'udta'];ud=dict(boxes(md))[b'udta'];assert [(t,v) for t,v in boxes(ua) if t!=b'NCDT']==[(t,v) for t,v in boxes(ud) if t!=b'NCDT']
 nc=ncdt(d);assert nc[b'NCHD']==aa[b'NCHD'] and nc[b'NCDB']==aa[b'NCDB']
 if name=='native-fields.MOV':assert all(nc[t]==aa[t] for t in [b'NCTH',b'NCVW',b'NCM1',b'NCM2'])
 (R/name).write_bytes(d);report.append(dict(name=name,sha256=sh(d),bytes=len(d),nctg_records=len(dict(recs(nc[b'NCTG']))),video_audio_tracks_and_mdat_unchanged=True))
(R/'ncdt-variants.json').write_text(json.dumps(dict(source=sh(a),donor=sh(b),scope='diagnostic fixtures only, no production donor bytes',variants=report),indent=2)+'\n')
print(json.dumps(report,indent=2))
