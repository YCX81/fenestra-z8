from pathlib import Path
import sys,struct,hashlib,json,math
import numpy as np
p=Path(__file__).resolve().parent;live=p.parents[2];sys.path.insert(0,str(live/'feat-mov'));import mov_fix as m
src=live/'feat-v53/card-v52';inv=json.loads((src/'inventory.json').read_text());rows=[]
def stat(a):
 a=np.asarray(a,dtype=np.float64);rms=float(np.sqrt(np.mean(a*a))) if a.size else 0;peak=float(np.max(np.abs(a))) if a.size else 0
 return dict(samples=a.size,rms=rms,rms_dbfs=20*math.log10(rms/32768) if rms else None,peak=int(peak),peak_dbfs=20*math.log10(peak/32768) if peak else None,clip_count=int(np.sum((a==32767)|(a==-32768))),near_fullscale_count=int(np.sum(np.abs(a)>=32700)),zero_ratio=float(np.mean(a==0)),mean=float(np.mean(a)))
def runs(a):
 a=np.asarray(a,dtype=bool);edges=np.flatnonzero(np.diff(np.r_[False,a,False]));return [(int(lo),int(hi)) for lo,hi in zip(edges[::2],edges[1::2])]
for f in sorted((src/'LIVE/108NCZ_8').glob('*.MOV')):
 d=f.read_bytes();sha=hashlib.sha256(d).hexdigest();assert sha==next(r['sha256'] for r in inv['files'] if r['path']==str(f.relative_to(src)))
 moov=m.direct_child(m.parse(d),b'moov');tr=next(t for t in moov.children if t.typ==b'trak' and t.one(b'mdia').one(b'hdlr').data[8:12]==b'soun');st=tr.one(b'mdia').one(b'minf').one(b'stbl')
 desc=st.one(b'stsd').data[8:];stride,frames=struct.unpack_from('>II',st.one(b'stsz').data,4);assert stride==6
 table=next(x for x in st.children if x.typ in (b'stco',b'co64'));nc=m.u32(table.data,4);width=4 if table.typ==b'stco' else 8
 offsets=[int.from_bytes(table.data[8+i*width:8+(i+1)*width],'big') for i in range(nc)]
 sc=st.one(b'stsc').data;entries=[struct.unpack_from('>III',sc,8+i*12) for i in range(m.u32(sc,4))]
 chunkframes=[next(e[1] for e in reversed(entries) if e[0]<=i+1) for i in range(nc)];assert sum(chunkframes)==frames
 assert all(offsets[i]+chunkframes[i]*6==offsets[i+1] for i in range(nc-1))
 off=offsets[0];pcm=b''.join(d[o:o+n*6] for o,n in zip(offsets,chunkframes));assert len(pcm)==frames*6
 raw=np.frombuffer(pcm,np.uint8).reshape(-1,3);assert np.all(raw[:,0]==0)
 pcm16=raw[:,1:3].copy().tobytes();v=np.frombuffer(pcm16,'<i2').reshape(-1,2);assert len(v)==frames
 marker=d.rfind(b'LVJB');assert marker>0;pre_off,pre_frames,hz=struct.unpack_from('<III',d,marker+4);assert (pre_off,pre_frames,hz)==(off,frames,48000)
 atom=len(d)-struct.unpack_from('<I',d,len(d)-8)[0];pn,dt=struct.unpack_from('<II',d,atom+20)
 zero=[dict(start_s=lo/48000,end_s=hi/48000,ms=(hi-lo)/48) for lo,hi in runs(np.all(v==0,axis=1)) if hi-lo>=96]
 chunk_stats=[]
 for frame0 in range(0,frames,10922): # actual transaction 65536B/6 frames
  a=v[frame0:frame0+10922];n=len(a);chunk_stats.append(dict(frame=frame0,frames=n,first_half=stat(a[:n//2]),second_half=stat(a[n//2:])))
 row=dict(name=f.name,sha256=sha,audio_sample_entry=desc[4:8].decode(),frames=frames,seconds=frames/48000,pcm16_sha256=hashlib.sha256(pcm16).hexdigest(),pcm24_low_byte_all_zero=True,lvjb_same_track=True,preview_frames=pn,preview_step_us=dt,preview_seconds=pn*dt/1e6,overall=stat(v),channels=[stat(v[:,i]) for i in range(2)],nonzero_not_multiple4=int(np.sum((v!=0)&(v%4!=0))),zero_runs_ge2ms=zero,io_chunks=chunk_stats)
 # Legacy bug signature: alternation by each read's half-stereo-array. New writer
 # capacity=10922frames, historical capacity16385frames; keep amplitudes descriptive.
 row['legacy_phase_rms']={}
 for block in (16385,10922):
  a=v.ravel();first=[];second=[]
  for i in range(0,frames,block):
   n=min(block,frames-i);first.extend(a[2*i:2*i+n]);second.extend(a[2*i+n:2*(i+n)])
  sa,sb=stat(first),stat(second);row['legacy_phase_rms'][str(block)]=dict(first=sa['rms'],second=sb['rms'],ratio=sa['rms']/sb['rms'] if sb['rms'] else None)
 np.savetxt(p/(f.stem+'-10ms.csv'),np.array([[i/48000,stat(v[i:i+480])['rms'],np.max(np.abs(v[i:i+480].astype(np.int32)))] for i in range(0,frames,480)]),delimiter=',',header='start_s,rms16,peak16',comments='')
 (p/(f.stem+'.s16le')).write_bytes(pcm16);rows.append(row)
report=dict(scope='Four immutable real-camera v52 G recordings copied read-only by root. No comparison assumes identical scene/sound.',files=rows)
(p/'analysis.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps([dict(name=r['name'],seconds=r['seconds'],preview_seconds=r['preview_seconds'],stats=r['overall'],nonmultiple4=r['nonzero_not_multiple4'],zeros=r['zero_runs_ge2ms'],legacy_phase_rms=r['legacy_phase_rms']) for r in rows],indent=2))
