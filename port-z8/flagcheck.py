"""Raw FENESTRA directory entries (live and deleted) of a card image; optionally extract a live file."""
import struct,sys
def main(path,extract=None,out=None):
    with open(path,'rb') as f:
        part=struct.unpack_from('<I',f.read(512),0x1c6)[0]*512
        f.seek(part);vbr=f.read(512);_,_,heap,_,root=struct.unpack_from('<IIIII',vbr,80)
        ss=1<<vbr[108];sc=ss<<vbr[109];co=lambda c:part+heap*ss+(c-2)*sc
        f.seek(co(root));r=f.read(sc);i=0;dc=None
        while i<sc and r[i]:
            if r[i]==0x85:
                n=r[i+1];name=b''.join(r[i+32*k+2:i+32*k+32] for k in range(2,n+1)).decode('utf-16le')[:r[i+32+3]]
                if name=='FENESTRA':dc=struct.unpack_from('<I',r,i+32+20)[0]
                i+=32*(n+1)
            else:i+=32
        f.seek(co(dc));d=f.read(sc);i=0
        while i<sc and d[i]:
            t=d[i]
            if t in (0x85,0x05):
                n=d[i+1];s=d[i+32:i+64];name=b''.join(d[i+32*k+2:i+32*k+32] for k in range(2,n+1)).decode('utf-16le')[:s[3]]
                size=struct.unpack_from('<Q',s,8)[0];c=struct.unpack_from('<I',s,20)[0]
                print('live' if t==0x85 else 'DELETED',name,size,'cluster',c)
                if t==0x85 and name==extract:
                    f.seek(co(c));open(out,'wb').write(f.read(size));print('extracted',out)
                i+=32*(n+1)
            else:i+=32
main(*sys.argv[1:])
