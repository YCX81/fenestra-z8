"""drop_last_frame.py IN OUT : remove the last video sample from the tables (moov is last in file;
mdat untouched). Also trims the sound track to the new video length and fixes NCTG tag 0x13."""
import struct,sys
exec(open(__file__.rsplit('/',1)[0]+'/movinfo.py').read().split("if __name__")[0])
src,dst=sys.argv[1:3];b=bytearray(open(src,'rb').read())
def one(a,e,name):return next(find(b,a,e,[name]))
ms,me=one(0,len(b),b'moov');assert me==len(b)
chain=[ms-8];vid=None
for ts,te in find(b,ms,me,[b'trak']):
    md=one(ts,te,b'mdia');hd=one(md[0],md[1],b'hdlr');kind=bytes(b[hd[0]+8:hd[0]+12])
    mh=one(md[0],md[1],b'mdhd');mi=one(md[0],md[1],b'minf');st=one(mi[0],mi[1],b'stbl')
    if kind==b'vide':
        vid=(ts,te);tk=one(ts,te,b'tkhd')
        tts=one(st[0],st[1],b'stts');n,c,d=struct.unpack_from('>III',b,tts[0]+4);assert n==1
        struct.pack_into('>I',b,tts[0]+8,c-1);dur=struct.unpack_from('>I',b,mh[0]+16)[0];struct.pack_into('>I',b,mh[0]+16,dur-d)
        frames=c-1
        sc=one(st[0],st[1],b'stsc');k=struct.unpack_from('>I',b,sc[0]+4)[0];o=sc[0]+8+12*(k-1);f,spc,i=struct.unpack_from('>III',b,o);assert spc>1;struct.pack_into('>I',b,o+4,spc-1)
        sz=one(st[0],st[1],b'stsz');ss,cnt=struct.unpack_from('>II',b,sz[0]+4);assert not ss and cnt==c;struct.pack_into('>I',b,sz[0]+8,cnt-1)
        cut=sz[1]-4;chain+=[ts-8,md[0]-8,mi[0]-8,st[0]-8,sz[0]-8]
        us=frames*1001*1000000//30000
        struct.pack_into('>I',b,tk[0]+20,us)   # tkhd duration (movie timescale 1e6)
    if kind==b'soun':
        want=frames*8008//5;dur=struct.unpack_from('>I',b,mh[0]+16)[0];N=dur-want
        if N>0:
            struct.pack_into('>I',b,mh[0]+16,want)
            tts=one(st[0],st[1],b'stts');struct.pack_into('>I',b,tts[0]+8,want)
            sz=one(st[0],st[1],b'stsz');struct.pack_into('>I',b,sz[0]+8,want)
            sc=one(st[0],st[1],b'stsc');k=struct.unpack_from('>I',b,sc[0]+4)[0];o=sc[0]+8+12*(k-1);spc=struct.unpack_from('>I',b,o+4)[0];assert spc>N;struct.pack_into('>I',b,o+4,spc-N)
            tk=one(ts,te,b'tkhd');struct.pack_into('>I',b,tk[0]+20,us)
mv=one(ms,me,b'mvhd');struct.pack_into('>I',b,mv[0]+16,us)
u=one(ms,me,b'udta');i=b.find(b'NCTG',u[0],u[1]);q=i+4;e=i-4+struct.unpack_from('>I',b,i-4)[0]
while q+8<=e:
    tag,typ,cnt=struct.unpack_from('>IHH',b,q);s={1:1,2:1,3:2,4:4,5:8,7:1}.get(typ,1)*cnt
    if tag==0x13:assert struct.unpack_from('>I',b,q+8)[0]==frames+1;struct.pack_into('>I',b,q+8,frames)
    q+=8+s
for o in chain:struct.pack_into('>I',b,o,struct.unpack_from('>I',b,o)[0]-4)
del b[cut:cut+4];open(dst,'wb').write(b);print('frames',frames)
