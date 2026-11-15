"""TEST ONLY: v59 with the frozen clip forced to 3k+1 frames BEFORE the rule, so the drop branch runs."""
import sys,subprocess
from pathlib import Path
W=Path(sys.argv[1]);subprocess.run([sys.executable,str(Path(__file__).with_name('patch_om7.py')),str(W)],check=True)
p=W/'src-fix37/runtime.c';t=p.read_text();a=" if(count>3u&&count%3u==1u)count--; /* v59: stock player end-of-stream */";assert t.count(a)==1
p.write_text(t.replace(a," while(count>4u&&count%3u!=1u)count--; /* TEST: force 3k+1 */\n"+a))
