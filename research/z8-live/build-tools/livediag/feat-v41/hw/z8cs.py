"""z8card.py wrapper for the cardslot firmware (v41+):
 - card.rename starts with a B into the cardslot cave (0x401fe300..0x401fff80): accepted by the code check;
 - SD (B:) reads/writes through the stock wrappers need data-cache maintenance on firmware without the v42 fix:
   the tool's own buffer is cleaned+invalidated around every read and cleaned before every write
   (Fenestra core cache helper 0x401b7b68: op 4 clean, 6 clean+invalidate). Same arguments as z8card.py."""
import sys,struct
sys.path.insert(0,'.')
import z8card,livectl,z8_audio_probe as zp
CACHE=0x401b7b68
_orig=z8card.Card._check_code
def _check(self):
    w=struct.unpack('<I',self.mem_read(0x402dd9fc,4))[0]
    off=w&0x3ffffff;off-=(1<<26) if off&(1<<25) else 0
    if w>>26==0x05 and 0x401fe300<=0x402dd9fc+4*off<0x401fff80:
        z8card.RENAME_SIG=self.mem_read(0x402dd9fc,16)
    return _orig(self)
z8card.Card._check_code=_check
def _op(self,n,op):
    if n>0:zp.native_call(self.p,CACHE,(0,self.buf,min(n,1<<20),op),10000)
_rd=z8card.Card.read_into_buf
def read_into_buf(self,fd,n):
    _op(self,n,6);r=_rd(self,fd,n);_op(self,n,6);return r
_wr=z8card.Card.write_from_buf
def write_from_buf(self,fd,n,*a,**k):
    _op(self,n,4);return _wr(self,fd,n,*a,**k)
z8card.Card.read_into_buf=read_into_buf;z8card.Card.write_from_buf=write_from_buf
if __name__=='__main__':z8card.main()
