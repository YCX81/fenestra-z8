import struct,sys
def atoms(b,a,e):
    while a+8<=e:
        n,t=struct.unpack_from('>I4s',b,a);h=8
        if n==1:n=struct.unpack_from('>Q',b,a+8)[0];h=16
        if n<h or a+n>e:break
        yield t,a+h,a+n;a+=n
def find(b,a,e,path):
    for t,s,x in atoms(b,a,e):
        if t==path[0]:
            if len(path)==1: yield s,x
            else: yield from find(b,s,x,path[1:])
def info(p,dump=None):
    b=open(p,'rb').read();top=[(t,s-8,x) for t,s,x in atoms(b,0,len(b))]
    print(p,len(b),[(t.decode(),x-s) for t,s,x in top])
    ms,me=next(find(b,0,len(b),[b'moov']))
    mv=next(find(b,ms,me,[b'mvhd']));ts,dur=struct.unpack_from('>II',b,mv[0]+12);print(' mvhd',ts,dur,dur/ts)
    out=[]
    for ts_,te in find(b,ms,me,[b'trak']):
        md=next(find(b,ts_,te,[b'mdia',b'mdhd']));mts,mdur=struct.unpack_from('>II',b,md[0]+12)
        hd=next(find(b,ts_,te,[b'mdia',b'hdlr']));kind=b[hd[0]+8:hd[0]+12]
        st=next(find(b,ts_,te,[b'mdia',b'minf',b'stbl']))
        def box(n):
            r=list(find(b,st[0],st[1],[n]));return r[0] if r else None
        sd=box(b'stsd');fmt=b[sd[0]+12:sd[0]+16]
        stts=box(b'stts');n=struct.unpack_from('>I',b,stts[0]+4)[0];tt=[struct.unpack_from('>II',b,stts[0]+8+8*i) for i in range(n)]
        stsz=box(b'stsz');ss,cnt=struct.unpack_from('>II',b,stsz[0]+4)
        sizes=[ss]*cnt if ss else list(struct.unpack_from('>%dI'%cnt,b,stsz[0]+12))
        stsc=box(b'stsc');n=struct.unpack_from('>I',b,stsc[0]+4)[0];sc=[struct.unpack_from('>III',b,stsc[0]+8+12*i) for i in range(n)]
        co=box(b'stco');n=struct.unpack_from('>I',b,co[0]+4)[0];offs=list(struct.unpack_from('>%dI'%n,b,co[0]+8))
        el=list(find(b,ts_,te,[b'edts',b'elst']));elst=None
        if el:
            k=struct.unpack_from('>I',b,el[0][0]+4)[0];elst=[struct.unpack_from('>IiI',b,el[0][0]+8+12*i) for i in range(k)]
        total=sum(c*d for c,d in tt)
        print('  trak',kind,fmt,'mdhd',mts,mdur,'%.4fs'%(mdur/mts),'samples',cnt,'stts',tt[:6],len(tt),'sum %.4fs'%(total/mts),'chunks',len(offs),'stsc',sc[:4],len(sc),'elst',elst)
        out.append((kind,fmt,mts,sizes,sc,offs,b))
    return out
def pcm(tr):
    kind,fmt,mts,sizes,sc,offs,b=tr
    # expand chunks
    data=b''
    per=[]
    for i,(first,spc,_) in enumerate(sc):
        last=sc[i+1][0] if i+1<len(sc) else len(offs)+1
        per+= [spc]*(last-first)
    for o,spc in zip(offs,per): data+=b[o:o+spc*6]
    return data
if __name__=='__main__':
    import array
    for p in sys.argv[1:]:
        trs=info(p)
        for tr in trs:
            if tr[0]==b'soun':
                d=pcm(tr);n=len(d)//6
                # 24-bit LE stereo: take left top 16 bits
                L=[int.from_bytes(d[6*i+1:6*i+3],'little',signed=True) for i in range(n)]
                # zero runs > 5 ms
                runs=[];s=None
                for i,v in enumerate(L):
                    if v==0 and d[6*i:6*i+6]==b'\0'*6:
                        if s is None:s=i
                    else:
                        if s is not None and i-s>=240:runs.append((s,i-s))
                        s=None
                if s is not None and n-s>=240:runs.append((s,n-s))
                print('   pcm frames',n,'%.4fs'%(n/48000),'zero runs(start s,len s):',[('%.3f'%(a/48000),'%.3f'%(l/48000)) for a,l in runs])
                # rms per 100ms
                import math
                r=[int(math.sqrt(sum(x*x for x in L[i:i+4800])/4800)) for i in range(0,n-4799,4800)]
                print('   rms/100ms',r)
