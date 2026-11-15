import sys,json,struct
sys.path.insert(0,'.')
import livectl,z8_audio_probe as zp
O=json.load(open('push/ndoffs.json'));U=json.load(open('push/uioffs.json'))
t,p=livectl.connect()
rd=lambda a,n:int.from_bytes(bytes(p.debug.read(a,n)),'little')
call=lambda a,*x:zp.native_call(p,a,x,10000)
root=rd(0xa032b50c8+16,8);state=rd(root+663*96+80,8);u=rd(state+2528,8)
if rd(u,4)==0x47525846:u=rd(u+8,8)
P=O['profile'];fp=lambda k:rd(P+O[k],8)
surface=rd(u+O['d_surface'],8);tg=struct.unpack('<3I',bytes(p.debug.read(u+O['d_target'],12)));obj=u+U['d_object']
idx=rd(P+O['p_global_surface_index'],8)
r=dict(attached=rd(u+U['d_attached'],4),global_overlay=rd(u+O['d_global_overlay'],4),surface=hex(surface),target=[hex(x) for x in tg],object=hex(obj),
 original_visible=rd(u+O['d_original_visible'],4),target_was_enabled=rd(u+O['d_target_was_enabled'],4))
r['now_surface']=hex(call(fp('p_global_surface'),0,idx))
r['visible']=call(fp('p_gui_get_visible'),surface)&0xffffffff
r['target_enabled']=call(fp('p_target_enabled'),*tg)&0xffffffff
r['row_in_list']=hex(call(fp('p_find_row'),0x7fe0,surface))
r['surface_obj40']=hex(rd(obj+0x40,8))
print(json.dumps(r,indent=1))
