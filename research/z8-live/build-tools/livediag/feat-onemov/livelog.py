import struct,sys
N={1:'PRESS',2:'ACCEPT',3:'SHORT',4:'IGNORED',5:'SOFT_CANCEL',6:'RELEASE',7:'C0',8:'POOL',9:'GEOMETRY',10:'ALLOC',11:'FREEZE',12:'FREEZE_FAIL',13:'MUX_BEGIN',14:'MUX_END',15:'WRITER',16:'READY',17:'ENC_ERR',18:'MUX_ABORT',19:'NOTREADY',20:'ICON',21:'MUX_STAGE',22:'DCIM',23:'UI',24:'FLAG',25:'MODE',26:'W5',27:'LOGSAVE',28:'VIEW',29:'PB',30:'AUDIO',31:'SHUTTER',40:'RESUME',41:'WARM',42:'MUX_PAUSE'}
for p in sys.argv[1:]:
    b=open(p,'rb').read();print(p,b[:4],struct.unpack_from('<IQ',b,4),b[16:32].hex())
    rec=[struct.unpack_from('<QIIQ',b,32+24*i) for i in range((len(b)-32)//24)]
    rec=[r for r in rec if r[1]]
    rec.sort()
    for t,ty,a,bb in rec: print('%10.3f %-11s a=%x b=%x'%(t/1e6,N.get(ty,ty),a,bb))
