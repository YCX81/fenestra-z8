"""one-MOV v1: patch_e3 (stock-player bridge) + production details + writer without preview atom."""
import sys,subprocess
from pathlib import Path
W=Path(sys.argv[1])
subprocess.run([sys.executable,str(Path(__file__).with_name('patch_e3.py')),str(W)],check=True)
def sub(path,old,new,count=1):
    p=W/path;t=p.read_text();assert t.count(old)==count,(path,old[:60],t.count(old));p.write_text(t.replace(old,new))
R='pb-fix37/runtime.c'
# 1. badge returns after the clip; 4910 is set before the facts hooks go live
sub(R,"  nb_end(r);r->nb_returns++;\n","  nb_end(r);r->nb_returns++;STORE(&r->ui_leaving,0);r->blocked_key_epoch=0;\n")
sub(R," pb_ui_leave();\n STORE(&r->nb_stage,2);pb_nb_observe(&r->nb_request,2);\n FN(0x403ec3a0,void(*)(uint32_t,uint32_t))(0x4910,r->nb_movie);\n",
      " pb_ui_leave();\n FN(0x403ec3a0,void(*)(uint32_t,uint32_t))(0x4910,r->nb_movie);\n r->nb_idle_us=0;STORE(&r->nb_stage,2);pb_nb_observe(&r->nb_request,2);\n")
sub('pb-fix37/runtime.h',"    uintptr_t nb_node;\n","    uintptr_t nb_node;\n    uint64_t nb_idle_us;\n    uint32_t nb_cleanups,nb_refused;\n")
# 2. a stale bridge is closed before a new one
sub(R," if(LOAD(&r->nb_stage)||!playback_owner()||"," if(LOAD(&r->nb_stage))return;\n if(!playback_owner()||")
# 3. REC is not ours while the stock page plays
sub(R,"int owns_rec=body[9]&&LOAD(&r->key)==9&&r->cache_valid&&","int owns_rec=body[9]&&LOAD(&r->key)==9&&r->cache_valid&&!LOAD(&r->nb_stage)&&")
# 4. worker: leave by any other route (mode change, power, another page) -> restore the index
sub(R,"  if(LOAD(&r->nb_stage)==2u){FxNativeView v=pb_native_current_view();r->nb_view=(uint32_t)v.id|(uint32_t)v.state<<8|(uint32_t)active<<16|(v.address==UINT64_C(0xa0284a0e0)?1u<<24:0);r->nb_cur=id;}\n",
"""  if(LOAD(&r->nb_stage)==2u){
   if(playback_owner()&&!LOAD(&r->shutdown)&&FN(0x403ec398,uint32_t(*)(uint32_t))(0x4910)==r->nb_movie)r->nb_idle_us=0;
   else if(!r->nb_idle_us)r->nb_idle_us=now_us();
   else if(now_us()-r->nb_idle_us>400000u){nb_end(r);r->nb_cleanups++;STORE(&r->ui_leaving,0);}
  }
""")
# 5. deleting from inside the stock movie page would act on a member that has no DCIM file
sub(R," uint32_t status=pb_delete_original(id,flags);\n"," if(r&&LOAD(&r->nb_stage)==2u&&id==r->nb_movie){r->nb_refused++;return 0x0432008bu;}\n uint32_t status=pb_delete_original(id,flags);\n")
# 6. writer: the MOV ends at moov, no preview pictures atom
C='src-fix37/codec/live_transaction.c'
t=(W/C).read_text()
a=t.index(" if(t->preview_frame){\n  /* After moov, in the same file: one free atom every MOV reader skips.")
b=t.index(" int r=f->close(f->opaque,video);video=-1;if(r)goto done;\n if(t->mov_bytes_out)")
t=t[:a]+t[b:];(W/C).write_text(t)
print('om1 patched')
