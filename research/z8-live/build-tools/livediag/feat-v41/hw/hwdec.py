import sys,time
sys.path.insert(0,'.')
import livectl,z8_audio_probe as zp
t,p=livectl.connect()
call=lambda a,*x:zp.native_call(p,a,x,20000)
ALLOC,FREE,DEC,NOOP=0x40d2c408,0x40d2c448,0x401f4850,0x40c028ac
jpg=open('push/pv20.jpg','rb').read()
bufs=[]
try:
    dec=call(ALLOC,16384,0);src=call(ALLOC,65536,0);out=call(ALLOC,1024*682*3//2+64,1);bufs=[dec,src,out]
    assert all(bufs),bufs
    for o in range(0,len(jpg),1000):p.debug.write(src+o,jpg[o:o+1000])
    def timeit(fn,n):
        t0=time.perf_counter()
        for _ in range(n):r=fn()
        return (time.perf_counter()-t0)/n,r
    tn,_=timeit(lambda:call(NOOP),10)
    td,r=timeit(lambda:call(DEC,dec,src,len(jpg),out,1024,682),10)
    print('decode rc',r&0xffffffff,'null call %.1f ms'%(tn*1000),'decode call %.1f ms'%(td*1000),'=> decode ~%.1f ms'%((td-tn)*1000))
    y=bytes(p.debug.read(out+1024*300+500,16));print('sample Y',y.hex())
finally:
    for b in bufs:
        if b:call(FREE,b)
