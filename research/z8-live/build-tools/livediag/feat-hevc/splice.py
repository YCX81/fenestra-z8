import hashlib,struct,json,sys
B=0x40160000
BUILD_ID=0x401fe060
def runs(a,b,step=4096):
    out=[];n=len(a);i=0
    while i<n:
        if a[i:i+step]==b[i:i+step]: i+=step; continue
        e=min(n,i+step)
        while i<e:
            if a[i]!=b[i]:
                j=i
                while j<n and a[j]!=b[j]: j+=1
                out.append((i,j)); i=j
            else: i+=1
    return out
v=open('/path/to/user/v37build/v37/v37.main','rb').read()
h=open('/path/to/user/v37build/hevc/v37.main','rb').read()
c=open('b12501ea.main','rb').read()
assert hashlib.sha256(c).hexdigest().startswith('b12501ea')
rh=runs(v,h); rc=runs(v,c)
bid=(BUILD_ID-B, BUILD_ID-B+4)
def ov(a,b): return a[0]<b[1] and b[0]<a[1]
rh2=[r for r in rh if not ov(r,bid)]
clash=[(hex(a[0]+B),hex(a[1]+B),hex(b[0]+B),hex(b[1]+B)) for a in rh2 for b in rc if ov((a[0]-8,a[1]+8),b)]
print('hevc runs',len(rh),'later-layer runs',len(rc),'clashes',clash[:10])
print('hevc span',hex(min(r[0] for r in rh2)+B),hex(max(r[1] for r in rh2)+B))
assert not clash
d=bytearray(c)
for a,b in rh2: d[a:b]=h[a:b]
assert struct.unpack_from('<Q',d,0x401fe068-B)[0]==0x31544f4f425846
old=struct.unpack_from('<I',d,BUILD_ID-B)[0]
struct.pack_into('<I',d,BUILD_ID-B,0)
nid=struct.unpack('<I',hashlib.sha256(bytes(d)).digest()[:4])[0]|0x01000000
struct.pack_into('<I',d,BUILD_ID-B,nid)
open('hevc.main','wb').write(d)
s=hashlib.sha256(d).hexdigest()
json.dump(dict(base='b12501ea',hevc_v37=hashlib.sha256(h).hexdigest(),sha256=s,build_id=hex(nid),previous_build_id=hex(old),
  ranges=[[hex(a+B),hex(b+B)] for a,b in rh2]),open('hevc.main.json','w'),indent=1)
print('out',s[:16],'build id',hex(old),'->',hex(nid))
