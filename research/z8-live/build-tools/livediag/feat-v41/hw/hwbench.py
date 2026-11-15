import sys,time
sys.path.insert(0,'.')
import livectl,z8_audio_probe as zp
t,p=livectl.connect()
call=lambda a,*x:zp.native_call(p,a,x,30000)
L=open('push/v44sig.txt').read().split()
ALLOC,FREE,NOOP=0x40d2c408,0x40d2c448,0x40c028ac;DEC,CONV=int(L[0],16),int(L[1],16);sig=L[2:]
assert bytes(p.debug.read(DEC,32)).hex()==sig[0],'decoder bytes differ'
assert bytes(p.debug.read(CONV,32)).hex()==sig[1],'conv bytes differ'
jpg=open('push/pv20.jpg','rb').read()
W,H=1024,682;FB=W*H*3//2;PX=24+W*H*4
bufs=[]
def A(n,pool):
    a=call(ALLOC,n,pool);assert a,(n,pool);bufs.append(a);return a
try:
    dec=A(16384,0);src=A(65536,0);sc=A(4096,0);out=A(FB+64,1);px=call(ALLOC,PX+64,1)
    if px:bufs.append(px)
    for o in range(0,len(jpg),1000):p.debug.write(src+o,jpg[o:o+1000])
    def T(fn,n):
        t0=time.perf_counter()
        for _ in range(n):r=fn()
        return (time.perf_counter()-t0)/n*1000,r
    tn,_=T(lambda:call(NOOP),10)
    td,r=T(lambda:call(DEC,dec,src,len(jpg),out,W,H),10)
    tc,r2=T(lambda:call(CONV,sc,sc+256,px,PX,out,FB),5) if px else (float('nan'),-1)
    print('null call %.1f ms | decode %.1f ms (rc %d) | yuv->rgba %.1f ms (rc %d)'%(tn,td-tn,r&0xffffffff,tc-tn,r2&0xffffffff))
finally:
    for b in bufs:call(FREE,b)
