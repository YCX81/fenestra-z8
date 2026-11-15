"""Prefix every HEVC video sample with an access unit delimiter NAL.
   Stock C3.11 player (0x40d00670..) requires the first NAL of every picture to be <=4 bytes (the AUD)
   and splits a GOP chunk into pictures at 3-byte NAL type 35.
   Assumes layout ftyp, mdat, moov[, free], video samples contiguous at mdat start, stco (not co64) everywhere."""
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
    top=[n[-1] for n in walk(d,0,len(d)) if len(n)==1]
    assert [t[0] for t in top[:3]]==[b'ftyp',b'mdat',b'moov'],top
    mdat,moov=top[1],top[2]
    nodes=list(walk(d,0,len(d)))
    u=lambda n,off,c: struct.unpack(f'>{c}I',d[n[1]+off:n[1]+off+4*c])
    traks=[n[-1][1] for n in nodes if n[-1][0]==b'trak']
    def find(name,t): return next(n[-1] for n in nodes if n[-1][0]==name and any(a[0]==b'trak' and a[1]==t for a in n))
    vt=next(t for t in traks if d[find(b'hdlr',t)[1]+16:find(b'hdlr',t)[1]+20]==b'vide')
    stsz,stco,stsc=find(b'stsz',vt),find(b'stco',vt),find(b'stsc',vt)
    n=u(stsz,16,1)[0]; sizes=u(stsz,20,n); offs=u(stco,16,u(stco,12,1)[0])
    ent=[u(stsc,16+12*i,3) for i in range(u(stsc,12,1)[0])]
    v0=offs[0]; vend=v0+sum(sizes); assert v0==mdat[1]+8
    # samples per chunk
    spc=[]
    for i,(first,cnt,_) in enumerate(ent):
        last=ent[i+1][0]-1 if i+1<len(ent) else len(offs)
        spc+= [cnt]*(last-first+1)
    assert sum(spc)==n
    out=bytearray(); p=v0; new_sizes=[]; new_offs=[]; k=0
    for c in spc:
        new_offs.append(v0+len(out))
        for _ in range(c):
            s=sizes[k]; body=d[p:p+s]; ln=struct.unpack('>I',body[:4])[0]; t=(body[4]>>1)&0x3f
            assert ln+4==s,'expected one NAL per sample'
            aud=b'\x00\x00\x00\x03\x46\x01'+(b'\x10' if 16<=t<=21 else b'\x30')
            out+=aud+body; new_sizes.append(s+7); p+=s; k+=1
    delta=len(out)-(vend-v0)
    struct.pack_into(f'>{n}I',d,stsz[1]+20,*new_sizes)
    struct.pack_into(f'>{len(new_offs)}I',d,stco[1]+16,*new_offs)
    for t in traks:
        if t==vt: continue
        try: co=find(b'stco',t)
        except StopIteration: continue
        c=u(co,12,1)[0]; o=u(co,16,c); assert all(x>=vend for x in o)
        struct.pack_into(f'>{c}I',d,co[1]+16,*[x+delta for x in o])
    struct.pack_into('>I',d,mdat[1],mdat[2]+delta)
    res=d[:v0]+out+d[vend:]
    if len(top)>3 and top[3][0]==b'free' and top[3][2]-delta>=8:
        fp=top[3][1]+delta; struct.pack_into('>I',res,fp,top[3][2]-delta); res=res[:fp+top[3][2]-delta]
    open(dst,'wb').write(res); print('samples',n,'delta',delta,'size',len(res))
main(*sys.argv[1:])
