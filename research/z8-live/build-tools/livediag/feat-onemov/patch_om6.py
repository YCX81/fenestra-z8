"""v58 = om5 + the Live badge is not re-enabled while the stock page is still being torn down.
v54-v57 cleared ui_leaving (badge allowed) either in the return hook or, worse, from the worker 0.4 s
after the stock code set 4910 back to the photo - which on clips where the player stops itself first
happens BEFORE the page switch, with a full-size JPEG decode in between on hardware."""
import sys,subprocess
from pathlib import Path
W=Path(sys.argv[1]);subprocess.run([sys.executable,str(Path(__file__).with_name('patch_om5.py')),str(W)],check=True)
p=W/'pb-fix37/runtime.c';t=p.read_text()
def rep(a,b):
    global t;assert t.count(a)==1,a[:70];t=t.replace(a,b)
rep("  r->nb_idle_us=now_us();STORE(&r->nb_stage,3);r->nb_returns++;STORE(&r->ui_leaving,0);r->blocked_key_epoch=0;pb_nb_observe(&r->nb_request,4);\n",
    "  r->nb_idle_us=now_us();STORE(&r->nb_stage,3);r->nb_returns++;pb_nb_observe(&r->nb_request,4);\n")
a=t.index("  if(LOAD(&r->nb_stage)==2u){\n   if(playback_owner()");b=t.index("  if(lock(r)){live_native_sync_step(&r->journal,&r->sync);unlock(r);}")
t=t[:a]+'''  if(LOAD(&r->nb_stage)==2u){
   /* left playback altogether (mode change, power): restore after 1 s. Still in the playback view
    * but 4910 no longer names the clip for 5 s: the page was left by another route, close lazily. */
   int gone=!playback_owner()||LOAD(&r->shutdown);
   if(!gone&&FN(0x403ec398,uint32_t(*)(uint32_t))(0x4910)==r->nb_movie)r->nb_idle_us=0;
   else if(!r->nb_idle_us)r->nb_idle_us=now_us();
   else if(now_us()-r->nb_idle_us>(gone?1000000u:5000000u)){nb_end(r);r->nb_cleanups++;if(!gone){STORE(&r->ui_leaving,0);r->blocked_key_epoch=0;}}
  }
  else if(LOAD(&r->nb_stage)==3u&&now_us()-r->nb_idle_us>2500000u){nb_end(r);STORE(&r->ui_leaving,0);r->blocked_key_epoch=0;}
'''+t[b:]
p.write_text(t);print('om6 patched')
