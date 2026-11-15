import sys
sys.path.insert(0,'.')
import livectl,z8_audio_probe as zp
t,p=livectl.connect()
call=lambda a,*x:zp.native_call(p,a,x,10000)
buf=call(0x40d2c408,256,0)
try:
    for path in ('B:\\FENESTRA\\NOPE.BIN','A:\\FENESTRA\\NOPE.BIN','B:\\NODIR\\NOPE.BIN','A:\\FENESTRA\\LIVE-Z8.0'):
        p.debug.write(buf,path.encode()+b'\0')
        fd=call(0x402dd4f8,buf,0,0)&0xffffffff
        print(path,hex(fd))
        if fd<0x80000000:call(0x402dd558,fd)
finally:call(0x40d2c448,buf)
