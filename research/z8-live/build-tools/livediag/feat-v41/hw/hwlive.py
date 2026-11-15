"""Read Live UI + runtime state on hw (same pointer chain as the QEMU driver snapshot)."""
import sys,json,struct
sys.path.insert(0,'.')
import livectl
OFF=json.load(open('push/offsets-v37.json'))
t,p=livectl.connect()
rd=lambda a,n:int.from_bytes(bytes(p.debug.read(a,n)),'little')
s={}
root=rd(0xa032b50c8+16,8);state=rd(root+663*96+80,8);s['state']=hex(state)
s.update(core_magic=hex(rd(state+64,4)),installed=rd(state+68,4),activation=rd(state+0x48,4),shutdown=rd(state+0x54,4),photo_movie=rd(0xa03bd062c,4))
u=rd(state+2528,8)
if rd(u,4)==0x47525846:s['registry']=hex(u);u=rd(u+8,8)
s['ui']=hex(u)
s.update(ui_magic=hex(rd(u,4)),command=rd(u+16,4),reload=rd(u+20,4),shutdown_ui=rd(u+24,4),save_error=rd(u+28,4),loaded=rd(u+32,4),slot=rd(u+36,4),display_status=hex(rd(u+40,4)),generation=rd(u+48,8),
         indicator_attached=rd(u+12296,4),indicator_frames=rd(u+12300,4),indicator_error=rd(u+12312,4))
eng=rd(u+8,8);s['engine']=hex(eng)
blob=bytes(p.debug.read(eng,256))
for k in range(0,OFF['struct_bytes'],1024):
    pass
wide={'poll_count','full_presses','writer_returns','jpeg_returns','capture.irq_seq','capture.copied','capture.dropped','capture.accepted_presses','encode_started_us','encode_finished_us','frame_encode_started_us','frame_encode_finished_us','capture.press_us','capture.rejected_presses'}
stream32={'width','height','allocation_requested','bitrate','idr_interval_us','job','frozen_count','frozen_size','last_duration_us','error','cleanup_failed','mux_started','mux_error'}
rt={}
for name,off in OFF.items():
    if name=='struct_bytes':continue
    if name in ('last_native_path','debug_ticket_path','debug_dir_path'):
        L=128 if name=='debug_dir_path' else 32
        rt[name]=bytes(p.debug.read(eng+off,L)).split(b'\0')[0].decode('ascii','replace');continue
    w=8 if name in wide or (name.startswith('stream.') and name.split('.',1)[1] not in stream32) else 4
    rt[name]=rd(eng+off,w)
s['runtime']=rt
print(json.dumps(s,indent=1))
