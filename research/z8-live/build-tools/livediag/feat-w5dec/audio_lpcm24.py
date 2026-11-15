"""Convert the audio track from 'sowt' 16-bit stereo to the stock C3.11 format: 'lpcm' (sound description v2),
   48 kHz, 2 ch, 24-bit little-endian signed packed, 6 bytes/frame. Samples are shifted left by 8 bits.
   Layout assumed: ftyp, mdat, moov[, free]; audio is one contiguous run after the video data; stco tables."""
import struct,sys
CONT={b'moov',b'trak',b'mdia',b'minf',b'stbl'}
LPCM=bytes.fromhex('000000486c70636d000000000000000100020000000000000003001 0fffe0000'.replace(' ','')+
                   '00010000000000484 0e7700000000000'.replace(' ','')+'000000027f0000000000001800000 00c'.replace(' ','')+'0000000600000001')
def walk(d,lo,hi,path=()):
    p=lo
    while p+8<=hi:
        size,name=struct.unpack('>I4s',d[p:p+8])
        if size<8: break
        yield path+((name,p,size),)
        if name in CONT: yield from walk(d,p+8,p+size,path+((name,p,size),))
        p+=size
def main(src,dst):
    assert len(LPCM)==72
    d=bytearray(open(src,'rb').read())
    nodes=list(walk(d,0,len(d)))
    top=[n[-1] for n in nodes if len(n)==1]; assert [t[0] for t in top[:3]]==[b'ftyp',b'mdat',b'moov']
    mdat=top[1]
    u=lambda n,off,c: struct.unpack(f'>{c}I',d[n[1]+off:n[1]+off+4*c])
    traks=[n[-1][1] for n in nodes if n[-1][0]==b'trak']
    def findn(name,t): return next(n for n in nodes if n[-1][0]==name and any(a[0]==b'trak' and a[1]==t for a in n))
    kind=lambda t: bytes(d[findn(b'hdlr',t)[-1][1]+16:findn(b'hdlr',t)[-1][1]+20])
    at=next(t for t in traks if kind(t)==b'soun'); vt=next(t for t in traks if kind(t)==b'vide')
    stsd,stsz,stco=(findn(x,at) for x in (b'stsd',b'stsz',b'stco'))
    e=stsd[-1][1]+16; es=struct.unpack('>I',d[e:e+4])[0]
    assert d[e+4:e+8]==b'sowt' and struct.unpack('>HH',d[e+24:e+28])==(2,16)
    ssz,total=u(stsz[-1],12,2); assert ssz==4
    n=u(stco[-1],12,1)[0]; offs=u(stco[-1],16,n); base=offs[0]; end=base+total*4
    vco=findn(b'stco',vt)[-1]; assert all(x<base for x in u(vco,16,u(vco,12,1)[0])) and end<=mdat[1]+mdat[2]
    pcm=d[base:end]; out=bytearray(total*6)
    for c in range(2*total):
        out[3*c+1]=pcm[2*c]; out[3*c+2]=pcm[2*c+1]
    delta=len(out)-len(pcm)
    struct.pack_into(f'>{n}I',d,stco[-1][1]+16,*[base+(o-base)*6//4 for o in offs])
    struct.pack_into('>I',d,stsz[-1][1]+12,6)
    struct.pack_into('>I',d,mdat[1],mdat[2]+delta)
    d2=72-es; d[e:e+es]=LPCM
    for a in stsd: struct.pack_into('>I',d,a[1],a[2]+d2)
    res=d[:base]+out+d[end:]
    open(dst,'wb').write(res); print('audio frames',total,'chunks',n,'max chunk bytes',max((b-a) for a,b in zip([base+(o-base)*6//4 for o in offs],[base+(o-base)*6//4 for o in offs][1:]+[base+total*6])),'mdat delta',delta)
main(*sys.argv[1:])
