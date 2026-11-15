from pathlib import Path
import json,hashlib,struct,subprocess
P=Path(__file__).resolve().parent;W=Path('/path/to/Nikon/build-tools/livediag/feat-v53/integration-g-size2/work/src-fix37');T=P/'minsize-tests/transaction';T.mkdir(exist_ok=True);F=P.parents[2]/'feat-v50/writer'
s=(F/'nctg_host_writer.c').read_text();s=s.replace('nctg-work/src-fix37/codec/','').replace('nctg-work/src-fix37/audio/','');(T/'test.c').write_text(s)
cmd=['clang','-O1','-g','-fsanitize=address,undefined','-I'+str(W/'codec'),'-I'+str(W/'audio'),str(T/'test.c')]+[str(W/x)for x in ['codec/live_transaction.c','codec/live_mov.c','codec/live_jpeg.c','codec/preview_writer.c','codec/preview_reader.c','audio/live_audio_mov.c']]+['-o',str(T/'test')]
r=subprocess.run(cmd,capture_output=True,text=True);assert not r.returncode,r.stderr
au=(F/'input.aus').read_bytes();pts=[];at=0
while at<len(au):n,t=struct.unpack_from('<IQ',au,at);pts.append(t);at+=12+n
span=pts[-1]-pts[0]+10928;results=[]
for fault in [0,4,5,2,3]:
 out=T/('sample'+str(fault))
 for suffix in ['.MOV','.MAN','.JPG']:
  q=out.with_suffix(suffix)
  if q.exists():q.unlink()
 r=subprocess.run([str(T/'test'),str(F/'input.aus'),str(F/'input.s16'),str(F/'input-preview.jpg'),str(out),str(fault)],capture_output=True,text=True);assert not r.returncode,r.stderr
 e=dict(fault=fault,stdout=r.stdout)
 if fault in [0,4,5]:
  b=out.with_suffix('.MOV').read_bytes();n=struct.unpack_from('<I',b,len(b)-8)[0];base=len(b)-n;head=struct.unpack_from('<I',b,base+20)[0];dt=struct.unpack_from('<I',b,base+24)[0];assert head==45 and span<=45*dt<span+45
  e.update(span=span,frame_us=dt,preview_span=dt*45,mov_sha256=hashlib.sha256(b).hexdigest())
 results.append(e)
(T/'result.json').write_text(json.dumps(dict(passed=True,actual_pts_span=span,frames=len(pts),cases=results,scope='Actual complete transaction writer with HEVC AU input; success, absent/partial audio fallback, metadata failure and cancel. Source images/timestamps not changed.'),indent=2)+'\n');print(json.dumps(results,indent=2))
