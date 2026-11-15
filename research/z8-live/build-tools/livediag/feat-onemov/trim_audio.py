"""trim_audio.py IN OUT N : drop the last N PCM frames of the sound track (tables only, file size unchanged)."""
import struct,sys
exec(open(__file__.rsplit('/',1)[0]+'/movinfo.py').read().split("if __name__")[0])
src,dst,N=sys.argv[1],sys.argv[2],int(sys.argv[3]);b=bytearray(open(src,'rb').read())
ms,me=next(find(b,0,len(b),[b'moov']))
for ts,te in find(b,ms,me,[b'trak']):
    hd=next(find(b,ts,te,[b'mdia',b'hdlr']))
    if b[hd[0]+8:hd[0]+12]!=b'soun':continue
    md=next(find(b,ts,te,[b'mdia',b'mdhd']));dur=struct.unpack_from('>I',b,md[0]+16)[0];struct.pack_into('>I',b,md[0]+16,dur-N)
    st=next(find(b,ts,te,[b'mdia',b'minf',b'stbl']))
    stts=next(find(b,st[0],st[1],[b'stts']));n,c,d=struct.unpack_from('>III',b,stts[0]+4);assert n==1 and c==dur;struct.pack_into('>I',b,stts[0]+8,c-N)
    sz=next(find(b,st[0],st[1],[b'stsz']));ss,cnt=struct.unpack_from('>II',b,sz[0]+4);assert ss and cnt==dur;struct.pack_into('>I',b,sz[0]+8,cnt-N)
    sc=next(find(b,st[0],st[1],[b'stsc']));n=struct.unpack_from('>I',b,sc[0]+4)[0];o=sc[0]+8+12*(n-1);f,spc,i=struct.unpack_from('>III',b,o)
    co=next(find(b,st[0],st[1],[b'stco']));nc=struct.unpack_from('>I',b,co[0]+4)[0]
    assert f==nc and spc>N,(f,nc,spc);struct.pack_into('>I',b,o+4,spc-N)
    print('audio',dur,'->',dur-N,'last chunk',spc,'->',spc-N)
open(dst,'wb').write(b)
