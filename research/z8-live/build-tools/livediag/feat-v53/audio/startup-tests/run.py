from pathlib import Path
import re,subprocess,hashlib,json
p=Path(__file__).resolve().parent;s=p.parent/'capture-work/src-fix37';d=(s/'runtime.c').read_text()
def function(name):
 i=d.index('static void '+name+'(');b=d.index('{',i);depth=1;j=b+1
 while depth:
  if d[j]=='{':depth+=1
  elif d[j]=='}':depth-=1
  j+=1
 return d[i:j]
(p/'runtime_selected.inc').write_text(function('live_audio_step')+'\n'+function('live_audio_ready_step')+'\n')
# Structural worker check complements compiled source behavior; does not claim native scheduling.
w=d[d.index('static void worker('):d.index('void live_runtime_boot(')]
assert w.index('if(!la_cancelled(&r->capture))live_audio_step') < w.index('live_stream_step') < w.rindex('live_audio_step')
assert 'LOAD(&r->audio_warm) || LOAD(&r->stream.job)' in d
backend=(s/'audio/live_audio_z8.c').read_text().replace('section(".live_extra"),','')
(p/'backend-host.c').write_text(backend)
cmd=['clang','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(s),'-I'+str(p),'-I'+str(s/'audio'),str(p/'test_startup.c'),str(s/'audio/live_audio.c'),str(p/'backend-host.c'),str(s/'codec/au_ring.c'),'-o',str(p/'test_startup')]
subprocess.run(cmd,check=True);r=subprocess.run([str(p/'test_startup')],capture_output=True,text=True);print(r.stdout,r.stderr);assert not r.returncode
(p/'result.json').write_text(json.dumps(dict(passed=True,scope='Actual core/backend and verbatim runtime audio functions compiled with ASan+UBSan; native message/ADC/codec boundaries simulated. Worker ordering checked structurally; not hardware audio evidence.',source_hashes={str(x.relative_to(s)):hashlib.sha256(x.read_bytes()).hexdigest() for x in [s/'runtime.c',s/'runtime.h',s/'audio/live_audio.c',s/'audio/live_audio.h',s/'audio/live_audio_z8.c',s/'audio/live_audio_z8.h']},stdout=r.stdout),indent=2)+'\n')
