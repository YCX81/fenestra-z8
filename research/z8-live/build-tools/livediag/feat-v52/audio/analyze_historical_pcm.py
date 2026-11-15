"""Read-only audit of the retained real-camera recording and gain clipping."""
from pathlib import Path
import hashlib,json,math,struct,sys
HERE=Path(__file__).resolve().parent;LIVE=HERE.parents[1]
sys.path.insert(0,str(LIVE/'feat-mov'))
import mov_fix as m
def stats(samples):
 n=len(samples);peak=max(map(abs,samples),default=0);rms=math.sqrt(sum(x*x for x in samples)/n) if n else 0
 clips=sum(x in (-32768,32767) for x in samples)
 return {'samples':n,'rms':rms,'rms_dbfs':20*math.log10(rms/32768) if rms else None,'peak_abs':peak,'peak_dbfs':20*math.log10(peak/32768) if peak else None,'clipped_samples':clips,'clipped_ratio':clips/n if n else 0,'zero_ratio':sum(x==0 for x in samples)/n if n else 0}
rows=[];reference=None
for name in ['hw-DSC_0028.MOV','hw-DSC_0028-camera.MOV','LIV_9005-aud.MOV']:
 p=LIVE/'feat-w5dec'/name;d=p.read_bytes();moov=m.direct_child(m.parse(d),b'moov');tr=next(t for t in moov.children if t.typ==b'trak' and t.one(b'mdia').one(b'hdlr').data[8:12]==b'soun');st=tr.one(b'mdia').one(b'minf').one(b'stbl')
 desc=st.one(b'stsd').data[8:];assert desc[4:8]==b'sowt' and struct.unpack_from('>HH',desc,24)==(2,16)
 stride,frames=struct.unpack_from('>II',st.one(b'stsz').data,4);assert stride==4
 table=next(x for x in st.children if x.typ in (b'stco',b'co64'));assert m.u32(table.data,4)==1
 off=int.from_bytes(table.data[8:],'big');pcm=d[off:off+frames*4];assert len(pcm)==frames*4
 values=struct.unpack('<'+'h'*(frames*2),pcm)
 if reference is None:reference=values
 else:assert values==reference,'container derivative has different PCM'
 rows.append({'path':str(p),'file_sha256':hashlib.sha256(d).hexdigest(),'pcm_sha256':hashlib.sha256(pcm).hexdigest(),'frames':frames,'seconds':frames/48000,'all':stats(values),'channels':[stats(values[c::2]) for c in range(2)]})
# A counterfactual is labelled explicitly. The old callback boosted only the
# first `got` samples of each 2*got stereo read; infer its read capacity by exact
# divisibility, then amplify ONLY the old unboosted part. Never 4x the whole file.
models=[]
for block in (16384,16385):
 reconstructed=list(reference);boosted=[];plain=[]
 for f in range(0,len(reference)//2,block):
  n=min(block,len(reference)//2-f);lo=f*2
  boosted.extend(reference[lo:lo+n]);plain.extend(reference[lo+n:lo+2*n])
  for k in range(lo+n,lo+2*n):reconstructed[k]=max(-32768,min(32767,reference[k]*4))
 nonzero=[x for x in boosted if x]
 models.append({'callback_capacity_frames':block,'boosted_nonzero_not_multiple_of_4':sum(x%4 and x not in (-32768,32767) for x in nonzero),'boosted_nonzero_samples':len(nonzero),'already_boosted':stats(boosted),'old_unboosted':stats(plain),'predicted_uniform_4x':stats(reconstructed),'prediction_only':True})
report={'scope':'Historical real-camera hw-DSC_0028 and byte-identical PCM derivatives; no new v51/v52 microphone recording','recordings':rows,'gain_models':models,'conclusion':'Do not infer present microphone loudness/noise from this one historical clip. Uniform gain predictions are not measured v52 output.'}
(HERE/'historical-pcm.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'recorded':rows[0]['all'],'channels':rows[0]['channels'],'gain_models':models},indent=2))
