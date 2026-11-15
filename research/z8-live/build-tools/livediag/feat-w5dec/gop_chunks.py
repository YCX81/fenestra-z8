"""Rewrite the video track's stsc/stco so one chunk = one GOP (stock C3.11 check 0x40ce6c84):
   stss[1]-stss[0] == stsc[0].samples_per_chunk, and chunk_count == stsc[1].first_chunk."""
import struct,sys
CONT={b'moov',b'trak',b'mdia',b'minf',b'stbl'}
def walk(d,lo,hi,path=()):
    p=lo
    while p+8<=hi:
        size,name=struct.unpack('>I4s',d[p:p+8])
        yield path+((name,p,size),)
        if name in CONT: yield from walk(d,p+8,p+size,path+((name,p,size),))
        p+=size
def main(src,dst):
    d=bytearray(open(src,'rb').read())
    nodes=list(walk(d,0,len(d)))
    def find(name,trak): return next(n for n in nodes if n[-1][0]==name and any(a[0]==b'trak' and a[1]==trak for a in n))
    traks=[n[-1][1] for n in nodes if n[-1][0]==b'trak']
    for t in traks:
        hd=find(b'hdlr',t)[-1] if any(n[-1][0]==b'hdlr' and any(a[1]==t for a in n) for n in nodes) else None
        if hd and d[hd[1]+16:hd[1]+20]==b'vide': vt=t
    u=lambda n,off,c: struct.unpack(f'>{c}I',d[n[1]+off:n[1]+off+4*c])
    stss,stsc,stco,stsz=(find(x,vt) for x in (b'stss',b'stsc',b'stco',b'stsz'))
    sync=u(stss[-1],16,u(stss[-1],12,1)[0]); offs=u(stco[-1],16,u(stco[-1],12,1)[0])
    n=u(stsz[-1],16,1)[0]; sizes=u(stsz[-1],20,n)
    assert u(stsc[-1],12,4)==(1,1,1,1) and len(offs)==n
    assert all(offs[i]+sizes[i]==offs[i+1] for i in range(n-1)),'samples not contiguous'
    gop=sync[1]-sync[0]; assert sync==tuple(range(1,n+1,gop)),sync
    starts=list(range(0,n,gop)); last=n-starts[-1]
    ent=[(1,gop,1)]+([(len(starts),last,1)] if last!=gop else [])
    if len(ent)==1: ent.append((len(starts),gop,1)) if len(starts)>1 else None
    new_stsc=struct.pack('>I4sII',16+12*len(ent),b'stsc',0,len(ent))+b''.join(struct.pack('>3I',*e) for e in ent)
    new_stco=struct.pack('>I4sII',16+4*len(starts),b'stco',0,len(starts))+b''.join(struct.pack('>I',offs[s]) for s in starts)
    edits=sorted([(stsc,new_stsc),(stco,new_stco)],key=lambda e:-e[0][-1][1])
    delta=0
    for node,new in edits:
        _,p,size=node[-1]; d[p:p+size]=new; delta+=len(new)-size
    for a in stsc[:-1]:   # ancestors (same for stco)
        struct.pack_into('>I',d,a[1],a[2]+delta)
    # keep file length: absorb into trailing free box if present
    open(dst,'wb').write(d); print('gop',gop,'chunks',len(starts),'stsc',ent,'delta',delta)
main(*sys.argv[1:])
