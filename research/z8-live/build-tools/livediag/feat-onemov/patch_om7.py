"""v60 = om6 + the frozen clip never has 3k+1 frames (rule applied where the clip is frozen).
Hardware (controlled, plain DCIM movies): 97 and 100 frames freeze the stock player at the end,
98 and 99 do not; 91 froze earlier. The last frame is dropped and the clip end moved to its
timestamp, so the picture stays 30000/1001 CFR and the sound window ends with the picture."""
import sys,subprocess,os
from pathlib import Path
W=Path(sys.argv[1]);subprocess.run([sys.executable,str(Path(__file__).with_name('patch_om6.py')),str(W)],check=True)
p=W/'src-fix37/capture/live_stream.c';t=p.read_text()
a=" if(!count||s->frozen[count-1].pts_us<s->clip_press)goto fail;\n";assert t.count(a)==1
force=" while(count>4u&&count%3u!=1u){count--;bytes-=(unsigned)s->frozen[count].bytes;s->clip_end=s->frozen[count].pts_us;} /* TEST: force 3k+1 */\n" if os.environ.get('OM_FORCE') else ""
rule=" if(count>3u&&count%3u==1u&&s->frozen[count-2].pts_us>=s->clip_press){count--;bytes-=(unsigned)s->frozen[count].bytes;s->clip_end=s->frozen[count].pts_us;} /* v60: stock player end-of-stream */\n" if not os.environ.get('OM_NORULE') else ""
p.write_text(t.replace(a,a+force+rule));print('om7 patched',bool(force),bool(rule))
