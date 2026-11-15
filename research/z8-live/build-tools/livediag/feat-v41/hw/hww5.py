import sys,struct,json
sys.path.insert(0,'.')
import livectl
t,p=livectl.connect()
rd=lambda a,n:int.from_bytes(bytes(p.debug.read(a,n)),'little')
root=rd(0xa032b50c8+16,8);state=rd(root+663*96+80,8);u=rd(state+2528,8)
if rd(u,4)==0x47525846:u=rd(u+8,8)
eng=rd(u+8,8);enc=eng+9056
e=dict(persist=hex(rd(enc,8)),scratch=hex(rd(enc+8,8)),width=rd(enc+16,4),height=rd(enc+20,4),frames=rd(enc+24,4),hw=hex(rd(enc+40,8)))
print('encoder',e)
w=rd(enc+40,8)
g=dict(VDI_BASE=hex(rd(0xa02d3d980,8)),pool_gate=hex(rd(0xa03f31a68,4)),photo_movie=rd(0xa03bd062c,4),client_map=bytes(p.debug.read(0xa02e98480,8)).hex(),slot_used=[rd(0xa02d30118+s*12,4) for s in range(4)])
print('globals',g)
if w:
    h=bytes(p.debug.read(w,48))
    names='magic stream width height slot_bytes hw_index batch_open frames busy release_req preempts busy_drops'.split()
    print('w5',{k:(hex(v) if k=='magic' else v) for k,v in zip(names,struct.unpack('<12I',h))},'arena',hex(rd(w+48,8)))
    tail={k:rd(w+o,4) for k,o in dict(reopen_blocked=1053536,closes=1053540,eos_ok=1053544,fin_rc=1053552,close_failed=1053564,flags=1053568,hevc_magic=1053600,hevc_open_rc=1053604,hevc_hdr_types=1053608,hevc_force=1053612).items()}
    print('w5 tail',{k:hex(v) for k,v in tail.items()})
