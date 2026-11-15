"""Experiment: replace the video samples of a camera-format Live MOV (ftyp, mdat, moov; video contiguous at
   mdat start; GOP chunks) with an Annex-B HEVC stream of the same frame count (AUD + slice per sample),
   rebuilding hvcC and the declared picture size. Used to test stock-size (1920x1080) playback."""
import struct,sys,re
CONT={b'moov',b'trak',b'mdia',b'minf',b'stbl',b'udta'}
def walk(d,lo,hi,path=()):
    p=lo
    while p+8<=hi:
        size,name=struct.unpack('>I4s',d[p:p+8])
        if size<8: break
        yield path+((name,p,size),)
        if name in CONT: yield from walk(d,p+8,p+size,path+((name,p,size),))
        p+=size
def nals(b):
    pos=[m.start() for m in re.finditer(b'\x00\x00\x01',b)]
    out=[]
    for i,p in enumerate(pos):
        e=pos[i+1] if i+1<len(pos) else len(b)
        n=b[p+3:e]
        while i+1<len(pos) and n.endswith(b'\x00'): n=n[:-1]
        out.append(n)
    return out
def unesc(n): return re.sub(b'\x00\x00\x03',b'\x00\x00',n)
def main(src,h265,dst,W,H):
    W=int(W);H=int(H)
    d=bytearray(open(src,'rb').read())
    nodes=list(walk(d,0,len(d)))
    top=[n[-1] for n in nodes if len(n)==1]; assert [t[0] for t in top[:3]]==[b'ftyp',b'mdat',b'moov']
    mdat=top[1]
    u=lambda n,off,c: struct.unpack(f'>{c}I',d[n[1]+off:n[1]+off+4*c])
    traks=[n[-1][1] for n in nodes if n[-1][0]==b'trak']
    def findn(name,t): return next(n for n in nodes if n[-1][0]==name and any(a[0]==b'trak' and a[1]==t for a in n))
    find=lambda name,t: findn(name,t)[-1]
    vt=next(t for t in traks if d[find(b'hdlr',t)[1]+16:find(b'hdlr',t)[1]+20]==b'vide')
    stsz,stco,stsc,stss,stsd,tkhd=(find(x,vt) for x in (b'stsz',b'stco',b'stsc',b'stss',b'stsd',b'tkhd'))
    n=u(stsz,16,1)[0]; sizes=u(stsz,20,n); offs=u(stco,16,u(stco,12,1)[0])
    ent=[u(stsc,16+12*i,3) for i in range(u(stsc,12,1)[0])]
    sync=u(stss,16,u(stss,12,1)[0])
    v0=offs[0]; vend=v0+sum(sizes); assert v0==mdat[1]+8
    spc=[]
    for i,(first,cnt,_) in enumerate(ent):
        last=ent[i+1][0]-1 if i+1<len(ent) else len(offs)
        spc+=[cnt]*(last-first+1)
    # new stream
    aus=[];ps={}
    for nal in nals(open(h265,'rb').read()):
        t=(nal[0]>>1)&0x3f
        if t==35: aus.append([nal])
        elif t in (32,33,34): ps.setdefault(t,nal)
        elif t<32: aus[-1].append(nal)
    assert len(aus)==n,(len(aus),n)
    for i,a in enumerate(aus):
        idr=any(((x[0]>>1)&0x3f) in (19,20) for x in a); assert idr==((i+1) in sync),i
    samp=[b''.join(struct.pack('>I',len(x))+x for x in a) for a in aus]
    out=bytearray();new_offs=[];k=0
    for c in spc:
        new_offs.append(v0+len(out))
        for _ in range(c): out+=samp[k];k+=1
    delta=len(out)-(vend-v0)
    struct.pack_into(f'>{n}I',d,stsz[1]+20,*[len(s) for s in samp])
    struct.pack_into(f'>{len(new_offs)}I',d,stco[1]+16,*new_offs)
    for t in traks:
        if t==vt: continue
        co=find(b'stco',t); c=u(co,12,1)[0]; o=u(co,16,c)
        struct.pack_into(f'>{c}I',d,co[1]+16,*[x+delta for x in o])
    struct.pack_into('>I',d,mdat[1],mdat[2]+delta)
    # declared size
    struct.pack_into('>II',d,tkhd[1]+tkhd[2]-8,W<<16,H<<16)
    ent0=stsd[1]+16; assert d[ent0+4:ent0+8]==b'hvc1'
    struct.pack_into('>HH',d,ent0+32,W,H)
    hp=d.find(b'hvcC',ent0,stsd[1]+stsd[2])-4; hs=struct.unpack('>I',d[hp:hp+4])[0]
    old=bytes(d[hp+8:hp+hs])
    sps=unesc(ps[33]); ptl=sps[3:15]
    hv=bytes([1])+ptl+old[13:22]+bytes([3])
    for t in (32,33,34): hv+=bytes([0x80|t])+struct.pack('>HH',1,len(ps[t]))+ps[t]
    newbox=struct.pack('>I4s',8+len(hv),b'hvcC')+hv
    d2=len(newbox)-hs
    # NCTG picture size tags
    for tag,val in ((0x22,W),(0x23,H)):
        q=d.find(struct.pack('>IHH',tag,3,1),top[2][1]); assert q>0,tag
        struct.pack_into('>H',d,q+8,val)
    d[hp:hp+hs]=newbox
    struct.pack_into('>I',d,ent0,struct.unpack('>I',d[ent0:ent0+4])[0]+d2)
    for a in findn(b'stsd',vt): struct.pack_into('>I',d,a[1],a[2]+d2)
    res=d[:v0]+out+d[vend:]
    open(dst,'wb').write(res); print('frames',n,'mdat delta',delta,'hvcC delta',d2,'ptl',ptl.hex(),'size',len(res))
main(*sys.argv[1:])
