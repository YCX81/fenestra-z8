from pathlib import Path
import json,hashlib,difflib
D=Path(__file__).resolve().parent;B=D.parents[1]/'feat-v50/integration-writer/v51d-work';C=D/'work'
names=['pb-fix37/global_output.h','pb-fix37/native_ui.c','pb-fix37/native_ui.h','pb-fix37/native_profile.c','pb-fix37/icon32_alpha.h','src-fix37/ui/icon32_alpha.h','src-fix37/ui/live_ui.c']
manifest=[];diff=[]
for n in names:
 old=(B/n).read_bytes() if (B/n).exists() else b'';new=(C/n).read_bytes();assert old!=new
 manifest.append(dict(path=n,baseline_sha256=hashlib.sha256(old).hexdigest() if old else None,candidate_sha256=hashlib.sha256(new).hexdigest()))
 diff.extend(difflib.unified_diff(old.decode().splitlines(True),new.decode().splitlines(True),fromfile='a/'+n if old else '/dev/null',tofile='b/'+n))
(D/'evf.patch').write_text(''.join(diff));(D/'changed-files.json').write_text(json.dumps(manifest,indent=2)+'\n')
