"""v57 = om4 + the bridge is closed lazily by the worker.
The return-to-photo hook (UI task) no longer takes DCF locks or clears the member bit; it only marks
'closing'. Redirect/facts stay valid while the stock player finishes stop/teardown (which on some clips
comes AFTER the page switch); the worker restores the index 1.5 s later."""
import sys,subprocess
from pathlib import Path
W=Path(sys.argv[1]);subprocess.run([sys.executable,str(Path(__file__).with_name('patch_om4.py')),str(W)],check=True)
p=W/'pb-fix37/runtime.c';t=p.read_text()
def rep(a,b):
    global t;assert t.count(a)==1,a[:70];t=t.replace(a,b)
rep("return r&&LOAD(&r->nb_stage)==2u&&path&&!memcmp(path,r->nb_from,30);}","return r&&LOAD(&r->nb_stage)>=2u&&path&&!memcmp(path,r->nb_from,30);}")
rep(" if(!o||!r||LOAD(&r->nb_stage)!=2u)return o;"," if(!o||!r||LOAD(&r->nb_stage)<2u)return o;")
a=t.index("static void nb_end(PbRuntime*r){");b=t.index("uint32_t pb_native_return(uint32_t kind,uint32_t page){")
t=t[:a]+'''static void nb_end(PbRuntime*r){
 if(LOAD(&r->nb_stage)<2u)return;
 FN(0x4085d348,void(*)(void))();
 if(LOAD(&r->nb_stage)>=2u){if(r->nb_bit)nb_mask(r->nb_photo,0);r->nb_bit=0;STORE(&r->nb_stage,0);}
 FN(0x4085d364,void(*)(void))();
 pb_nb_observe(&r->nb_request,3);
}
'''+t[b:]
rep("  nb_end(r);r->nb_returns++;STORE(&r->ui_leaving,0);r->blocked_key_epoch=0;\n",
    "  r->nb_idle_us=now_us();STORE(&r->nb_stage,3);r->nb_returns++;STORE(&r->ui_leaving,0);r->blocked_key_epoch=0;pb_nb_observe(&r->nb_request,4);\n")
rep(" if(LOAD(&r->nb_stage))return;\n"," if(LOAD(&r->nb_stage)==2u)return;\n")
a=t.index(" r->nb_photo=id;r->nb_movie=(id&~15u)|5u;\n");b=t.index(" pb_ui_leave();\n FN(0x403ec3a0,void(*)(uint32_t,uint32_t))(0x4910,r->nb_movie);")
t=t[:a]+''' FN(0x4085d348,void(*)(void))();
 if(LOAD(&r->nb_stage)==3u&&r->nb_bit&&r->nb_photo!=id)nb_mask(r->nb_photo,0);
 uint16_t*m=nb_mask(id,1);r->nb_node=(uintptr_t)m;r->nb_bit=m!=0;
 r->nb_photo=id;r->nb_movie=(id&~15u)|5u;r->nb_idle_us=0;
 STORE(&r->nb_stage,m?2u:0u);
 FN(0x4085d364,void(*)(void))();
 if(!m){r->nb_fail=2;pb_nb_observe(&r->nb_request,1);return;}
 r->nb_starts++;r->nb_fail=0;
'''+t[b:]
rep(" r->nb_idle_us=0;STORE(&r->nb_stage,2);pb_nb_observe(&r->nb_request,2);\n"," pb_nb_observe(&r->nb_request,2);\n")
rep("   else if(now_us()-r->nb_idle_us>400000u){nb_end(r);r->nb_cleanups++;STORE(&r->ui_leaving,0);}\n  }\n",
    "   else if(now_us()-r->nb_idle_us>400000u){nb_end(r);r->nb_cleanups++;STORE(&r->ui_leaving,0);}\n  }\n  else if(LOAD(&r->nb_stage)==3u&&now_us()-r->nb_idle_us>1500000u)nb_end(r);\n")
rep("r->cache_valid&&!LOAD(&r->nb_stage)&&","r->cache_valid&&LOAD(&r->nb_stage)!=2u&&")
rep(" if(r&&LOAD(&r->nb_stage)==2u&&id==r->nb_movie){r->nb_refused++;return 0x0432008bu;}\n",
    " if(r&&LOAD(&r->nb_stage)==2u&&id==r->nb_movie){r->nb_refused++;return 0x0432008bu;}\n if(r)nb_end(r);\n")
p.write_text(t);print('om5 patched')
