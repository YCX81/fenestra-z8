"""TEST ONLY: v58 (no rule) with the frozen clip forced to 3k+1 frames: the known-bad class."""
import sys,subprocess
from pathlib import Path
W=Path(sys.argv[1]);subprocess.run([sys.executable,str(Path(__file__).with_name('patch_om6.py')),str(W)],check=True)
p=W/'src-fix37/runtime.c';t=p.read_text();a=" unsigned count=r->stream.frozen_count;int rc=-1;unsigned cleanup=0;";assert t.count(a)==1
p.write_text(t.replace(a,a+"\n while(count>4u&&count%3u!=1u)count--; /* TEST: force 3k+1 */"))
