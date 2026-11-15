import json,sys,struct
obs=None
last=None;rep=0
for l in open(sys.argv[1]):
    try:d=json.loads(l)
    except:continue
    pc=d['pc'];r=d['regs']
    if pc=='402dd4f8':
        s=bytes.fromhex(d.get('x0_mem','')).split(b'\0')[0]
        if not(b'DCIM' in s or b'LIVE\\' in s): continue
        line='open %s lr=%s'%(s.decode(),r[8])
    elif pc.startswith('4019'):
        m=bytes.fromhex(d['x0_mem']);v=struct.unpack_from('<14I',m)
        names='request stage photo movie starts returns redirects view cur stats w h bit fail'.split()
        line='OBS where=%s '%r[1]+' '.join('%s=%x'%(n,x) for n,x in zip(names,v))+' node=%x from=%s to=%s'%(struct.unpack_from('<Q',m,56)[0],m[64:128].split(b'\0')[0].decode(),m[128:192].split(b'\0')[0].decode())
    else:
        line='%s x0=%s x1=%s x2=%s lr=%s'%(pc,r[0],r[1],r[2],r[8])
        if pc in('403ad6a4','403f7244'): line+=' fi='+d.get('file_info','')[16:44]
        if pc=='40cf7d0c': line+=' crop='+d.get('crop','')[32:56]
    if line==last: rep+=1; continue
    if rep: print('   x%d'%(rep+1))
    rep=0;last=line;print(line)
if rep: print('   x%d'%(rep+1))
