from pathlib import Path
import hashlib,json,difflib
D=Path(__file__).resolve().parent;B=D.parents[1]/'feat-v50/integration-writer/v51d-work'
names=['src-fix37/capture/live_stream.c','src-fix37/capture/live_stream.h','src-fix37/codec/live_codec.h','src-fix37/codec/live_w5enc.c','src-fix37/runtime.c','src-fix37/runtime.h']
out=[];patch=[]
for name in names:
 old=(B/name).read_bytes();new=(D/'work'/name).read_bytes();assert old!=new
 out.append({'path':name,'baseline_sha256':hashlib.sha256(old).hexdigest(),'candidate_sha256':hashlib.sha256(new).hexdigest()})
 patch.extend(difflib.unified_diff(old.decode().splitlines(True),new.decode().splitlines(True),fromfile='a/'+name,tofile='b/'+name))
(D/'handoff.patch').write_text(''.join(patch));(D/'changed-files.json').write_text(json.dumps(out,indent=2)+'\n')
print('handoff patch:',len(out),'owned files; audio dependency excluded')
