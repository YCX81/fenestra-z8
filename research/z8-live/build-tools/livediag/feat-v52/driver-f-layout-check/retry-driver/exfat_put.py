"""Add one directory and optional files to a freshly formatted exFAT image (MBR partition), without mounting."""
import struct

def _name_hash(name):
    h=0
    for b in name.upper().encode('utf-16le'):
        h=((h<<15)|(h>>1))&0xffff; h=(h+b)&0xffff
    return h

def _entry_set(name,attr,cluster,size):
    names=[name[i:i+15] for i in range(0,len(name),15)]
    e0=bytearray(32);e0[0]=0x85;e0[1]=1+len(names);struct.pack_into('<H',e0,4,attr)
    ts=struct.pack('<I',(46<<25)|(10<<21)|(30<<16)|(12<<11))  # fixed 2026-10-30 12:00
    e0[8:12]=ts;e0[12:16]=ts;e0[16:20]=ts
    e1=bytearray(32);e1[0]=0xc0;e1[1]=0x03;e1[3]=len(name);struct.pack_into('<H',e1,4,_name_hash(name))
    struct.pack_into('<QIIQ',e1,8,size,0,cluster,size)
    rows=[e0,e1]
    for n in names:
        r=bytearray(32);r[0]=0xc1;r[2:2+2*len(n)]=n.encode('utf-16le');rows.append(r)
    blob=bytearray(b''.join(bytes(r) for r in rows))
    c=0
    for i,b in enumerate(blob):
        if i in (2,3):continue
        c=((c<<15)|(c>>1))&0xffff;c=(c+b)&0xffff
    struct.pack_into('<H',blob,2,c)
    return bytes(blob)

def put(path,dirname,files):
    with open(path,'r+b') as f:
        f.seek(0);mbr=f.read(512);part=struct.unpack_from('<I',mbr,0x1c6)[0]*512
        f.seek(part);vbr=f.read(512);assert vbr[3:11]==b'EXFAT   '
        fatoff,fatlen,heap,count,root=struct.unpack_from('<IIIII',vbr,80)
        ss=1<<vbr[108];sc=ss<<vbr[109]
        co=lambda c:part+heap*ss+(c-2)*sc
        f.seek(co(root));rd=bytearray(f.read(sc))
        bm=None;i=0
        while rd[i]:
            if rd[i]==0x81:bm=struct.unpack_from('<IQ',rd,i+20)
            i+=32
        fc,blen=bm;f.seek(co(fc));bitmap=bytearray(f.read(blen))
        nxt=2
        def alloc(n):
            nonlocal nxt
            while True:
                if all(not bitmap[(nxt+k-2)//8]&(1<<((nxt+k-2)%8)) for k in range(n)):
                    s=nxt
                    for k in range(n):bitmap[(s+k-2)//8]|=1<<((s+k-2)%8)
                    nxt=s+n;return s
                nxt+=1
        dc=alloc(1);db=bytearray(sc);j=0
        for name,data in files.items():
            n=max(1,(len(data)+sc-1)//sc);c=alloc(n)
            f.seek(co(c));f.write(data)
            s=_entry_set(name,0x20,c,len(data));db[j:j+len(s)]=s;j+=len(s)
        f.seek(co(dc));f.write(db)
        s=_entry_set(dirname,0x10,dc,sc);rd[i:i+len(s)]=s
        f.seek(co(root));f.write(rd)
        f.seek(co(fc));f.write(bitmap)
    return dict(directory=dirname,files={k:len(v) for k,v in files.items()},first_cluster=dc)


def listing(path,dirname):
    """Names and sizes in a top-level directory, read from the image (contiguous directories only)."""
    with open(path,'rb') as f:
        f.seek(0);part=struct.unpack_from('<I',f.read(512),0x1c6)[0]*512
        f.seek(part);vbr=f.read(512)
        _,_,heap,_,root=struct.unpack_from('<IIIII',vbr,80)
        ss=1<<vbr[108];sc=ss<<vbr[109];co=lambda c:part+heap*ss+(c-2)*sc
        def rows(buf):
            i=0
            while i+32<=len(buf) and buf[i]:
                if buf[i]==0x85:
                    n=buf[i+1];e1=buf[i+32:i+64];name=b''.join(buf[i+32*k+2:i+32*k+32] for k in range(2,n+1)).decode('utf-16le')[:e1[3]]
                    yield name,struct.unpack_from('<Q',e1,8)[0],struct.unpack_from('<I',e1,20)[0],struct.unpack_from('<H',buf,i+4)[0]
                    i+=32*(n+1)
                else:i+=32
        f.seek(co(root));top=list(rows(f.read(sc)))
        for name,size,c,attr in top:
            if name==dirname:
                f.seek(co(c));return list(rows(f.read(sc)))
    return None

if __name__=='__main__':
    import sys
    print(listing(sys.argv[1],sys.argv[2]))
