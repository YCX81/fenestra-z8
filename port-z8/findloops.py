"""Locate the Z8 UI main loops the way native_ui_dispatch_sites.verify_original defines them."""
import struct
import numpy as np
from addrmap import Mapper, BASE
m=Mapper()
def m1(a): return m.code(a)['z8']
W={k:m1(v) for k,v in dict(poll=0x403d2158,wait=0x403d22d4,event_wait=0x403d69f4,event_group=0x403d6908,event_set=0x403d69d4).items()}
print({k:hex(v) for k,v in W.items()})
def callers(w,t,mark=0x94000000):
    bl=np.nonzero((w>>26)==0x25)[0]; im=(w[bl]&0x3ffffff).astype(np.int64); im[im>=1<<25]-=1<<26
    return [int(i) for i,d in zip(bl,im) if BASE+4*int(i)+4*int(d)==t]
for name,w,poll,wait,grp,ew in (('z9',m.w9,0x403d2158,0x403d22d4,0x403d6908,0x403d69f4),('z8',m.w8,W['poll'],W['wait'],W['event_group'],W['event_wait'])):
    P=callers(w,poll); Q=callers(w,wait)
    print(name,'poll callers',[hex(BASE+4*i) for i in P]); print(name,'wait callers',[hex(BASE+4*i) for i in Q])
    for q in Q:
        ok=(int(w[q-2])>>26==0x25) and (int(w[q+4])>>26==0x25) and int(w[q+1])==0x2a0003e2
        near=[p for p in P if 0<p-q<80]
        print('  wait',hex(BASE+4*q),'edge',ok,'poll',[hex(BASE+4*p) for p in near])
