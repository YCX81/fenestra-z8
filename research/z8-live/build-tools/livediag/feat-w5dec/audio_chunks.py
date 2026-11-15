"""Split the audio track into chunks: one per video chunk (GOP), time-aligned, or (third argument N) a fixed
   N frames per chunk like the stock writer (24024). Done by rewriting only the audio
   stsc/stco (PCM data stays where it is). Stock C3.11 reader (0x40344f40..) reads audio chunk N together
   with video chunk N and fails when the audio chunk is larger than its buffer.
   Layout assumed: mdat before moov, audio is one contiguous run, constant sample size, stco tables."""
import struct,sys
CONT={b'moov',b'trak',b'mdia',b'minf',b'stbl'}
def walk(d,lo,hi,path=()):
    p=lo
    while p+8<=hi:
        size,name=struct.unpack('>I4s',d[p:p+8])
        if size<8: break
        yield path+((name,p,size),)
        if name in CONT: yield from walk(d,p+8,p+size,path+((name,p,size),))
        p+=size
def main(src,dst,fixed=0):
    fixed=int(fixed)
    d=bytearray(open(src,'rb').read())
    nodes=list(walk(d,0,len(d)))
    top=[n[-1] for n in nodes if len(n)==1]; assert [t[0] for t in top[:3]]==[b'ftyp',b'mdat',b'moov']
    u=lambda n,off,c: struct.unpack(f'>{c}I',d[n[1]+off:n[1]+off+4*c])
    traks=[n[-1][1] for n in nodes if n[-1][0]==b'trak']
    def findn(name,t): return next(n for n in nodes if n[-1][0]==name and any(a[0]==b'trak' and a[1]==t for a in n))
    find=lambda name,t: findn(name,t)[-1]
    kind=lambda t: bytes(d[find(b'hdlr',t)[1]+16:find(b'hdlr',t)[1]+20])
    vt=next(t for t in traks if kind(t)==b'vide'); at=next(t for t in traks if kind(t)==b'soun')
    # video chunk start times
    vstsc,vstco,vstts,vmdhd=(find(x,vt) for x in (b'stsc',b'stco',b'stts',b'mdhd'))
    vscale=u(vmdhd,20,1)[0]
    ent=[u(vstsc,16+12*i,3) for i in range(u(vstsc,12,1)[0])]; nch=u(vstco,12,1)[0]
    spc=[]
    for i,(first,cnt,_) in enumerate(ent):
        last=ent[i+1][0]-1 if i+1<len(ent) else nch
        spc+=[cnt]*(last-first+1)
    durs=[]
    for i in range(u(vstts,12,1)[0]):
        c,dl=u(vstts,16+8*i,2); durs+=[dl]*c
    assert len(durs)==sum(spc)
    starts=[];k=0;t=0
    for c in spc:
        starts.append(t); t+=sum(durs[k:k+c]); k+=c
    # audio
    astsc,astco,astsz,amdhd=(findn(x,at) for x in (b'stsc',b'stco',b'stsz',b'mdhd'))
    ascale=u(amdhd[-1],20,1)[0]; ssz,total=u(astsz[-1],12,2); assert ssz and u(astco[-1],12,1)[0]==1 and u(astsc[-1],12,4)[:3]==(1,1,total)
    base=u(astco[-1],16,1)[0]
    idx=[min(total,round(s*ascale/vscale)) for s in starts]+[total]
    cnt=[idx[i+1]-idx[i] for i in range(nch)]; assert all(c>0 for c in cnt) and sum(cnt)==total
    if fixed:   # stock layout: constant frames per chunk (24024 = 0.5005 s; 24->32 bit output buffer holds 24064)
        idx=list(range(0,total,fixed))+[total]; nch=len(idx)-1; cnt=[idx[i+1]-idx[i] for i in range(nch)]
    ents=[]
    for i,c in enumerate(cnt):
        if not ents or ents[-1][1]!=c: ents.append((i+1,c,1))
    new_stsc=struct.pack('>I4sII',16+12*len(ents),b'stsc',0,len(ents))+b''.join(struct.pack('>3I',*e) for e in ents)
    new_stco=struct.pack('>I4sII',16+4*nch,b'stco',0,nch)+b''.join(struct.pack('>I',base+idx[i]*ssz) for i in range(nch))
    delta=0
    for node,new in sorted([(astsc,new_stsc),(astco,new_stco)],key=lambda e:-e[0][-1][1]):
        _,p,size=node[-1]; d[p:p+size]=new; delta+=len(new)-size
    for a in astsc[:-1]: struct.pack_into('>I',d,a[1],a[2]+delta)
    moov=top[2]
    if len(top)>3 and top[3][0]==b'free' and top[3][2]-delta>=8:
        fp=top[3][1]+delta; struct.pack_into('>I',d,fp,top[3][2]-delta); d=d[:fp+top[3][2]-delta] if fp+top[3][2]-delta<=len(d) else d
    open(dst,'wb').write(d); print('audio chunks',nch,'samples',cnt,'max bytes',max(cnt)*ssz,'stsc entries',len(ents),'delta',delta)
main(*sys.argv[1:])
