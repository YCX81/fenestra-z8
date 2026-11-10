"""Read Live state from the camera over Fenestra USB debug (win-lab, read-only peeks). --diag: also decode the diagnostic block."""
import subprocess,struct,re,sys,json,time,os
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
import livediag_fields as F
def peek(a,n):
    out=subprocess.run(['ssh','win-lab',f'cd /d C:\\Users\\ycx\\Nikon-probe\\audio-probe && python z8_audio_probe.py peek --address {a:#x} --count {n}'],capture_output=True).stdout.decode('gbk','replace')
    b=b''
    for l in out.splitlines():
        m=re.match(r'^0x[0-9a-f]+\s+([0-9a-f]{16})\s+([0-9a-f]{16})',l)
        if m:b+=struct.pack('<QQ',int(m[1],16),int(m[2],16))
    if len(b)<n:raise SystemExit('read failed:\n'+out[-800:])
    return b[:n]
q=lambda b,o:struct.unpack_from('<Q',b,o)[0];w=lambda b,o:struct.unpack_from('<I',b,o)[0]
root=q(peek(0xa032b50c8,32),16);core=q(peek(root+663*96+80,8),0)
c=peek(core+64,24);reg=q(peek(core+2528,8),0);r=peek(reg,32);ui=q(r,8)
u=peek(ui,56);eng=q(u,8)
R=peek(eng+0x640,0xc0);S=peek(eng+0x31c8,0x100)
o=lambda x:x-0x640;s=lambda x:x-0x31c8
d=dict(core=hex(core),magic=hex(w(c,0)),installed=w(c,4),activation=w(c,8),phase=w(c,12),error=w(c,16),
 ui=hex(ui),ui_magic=hex(w(u,0)),display_status=hex(w(u,40)),generation=q(u,48),engine=hex(eng),
 actual=hex(w(R,o(0x648))),count=w(R,o(0x650)),cap_phase=w(R,o(0x654)),reason=w(R,o(0x658)),copied=q(R,o(0x660)),dropped=q(R,o(0x668)),
 rejected=q(R,o(0x670)),accepted=q(R,o(0x678)),last_gen=q(R,o(0x680)),irq_seq=q(R,o(0x698)),irq_ts=q(R,o(0x6a0)),
 cap_req=w(R,o(0x6b0)),cancel_epoch=w(R,o(0x6b4)),req=w(R,o(0x6c0)),task_started=w(R,o(0x6c4)),task_error=w(R,o(0x6c8)),
 shutdown=w(R,o(0x6cc)),card_epoch=w(R,o(0x6d0)),poll=q(R,o(0x6d8)),full_presses=q(R,o(0x6e0)),
 width=w(S,0),height=w(S,4),alloc=w(S,8),job=w(S,s(0x31dc)),last_pts=q(S,s(0x31f8)),encoded=q(S,s(0x3218)),encode_last_us=q(S,s(0x3230)),
 first_pts=q(S,s(0x3238)),backlog_dropped=q(S,s(0x3260)),copy_dropped=q(S,s(0x3268)),ignored=q(S,s(0x3270)),
 geometry_changes=q(S,s(0x32a8)),serr=w(S,s(0x32b0)),mux_started=w(S,s(0x32b8)),mux_error=w(S,s(0x32bc)))
if '--diag' in sys.argv:
    D=F.decode(peek(eng+F.DIAG_OFFSET,F.DIAG_SIZE_V4 if '--v4' in sys.argv else F.DIAG_SIZE_V3 if '--v3' in sys.argv else F.DIAG_SIZE_V2 if '--v2' in sys.argv else F.DIAG_SIZE))
    d={k:d[k] for k in ('cap_phase','copied','dropped','irq_seq','poll','width','height','encoded','serr')};d['diag']=D
if '--full' in sys.argv:
    offs=json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)),'fix1-offsets.json')));offs.pop('size')
    lo=min(o for o,n in offs.values());hi=max(o+n for o,n in offs.values())
    B=peek(eng+lo,(hi-lo+15)//16*16);full={}
    for k,(o,n) in offs.items():
        v=B[o-lo:o-lo+n]
        full[k]=v.split(b'\0')[0].decode('latin1') if n>8 else int.from_bytes(v,'little')
    d['full']=full
if '--enc' in sys.argv:
    hw=q(peek(eng+0x2388,16),0)
    if 0xa00000000<=hw<0xb00000000 or 0x800000000<=hw<0x900000000:
        E=peek(hw+0x101310,80);k=['t_in','t_wait','t_out','t_parse','s_in','s_wait','s_out','s_parse','n_timed']
        e=dict(zip(k,struct.unpack('<9Q',E[:72])));n=e['n_timed'] or 1
        e.update({a+'_avg':e['s_'+a[2:]]//n for a in ('t_in','t_wait','t_out','t_parse')});e['hw']=hex(hw);d['enc']=e
    else:d['enc']={'hw':hex(hw)}
print(json.dumps(d,indent=None if '--diag' not in sys.argv and '--full' not in sys.argv else 1))
