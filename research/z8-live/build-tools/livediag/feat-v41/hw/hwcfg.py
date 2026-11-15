import sys,json
sys.path.insert(0,'.')
import livectl
t,p=livectl.connect()
rd=lambda a,n:int.from_bytes(bytes(p.debug.read(a,n)),'little')
root=rd(0xa032b50c8+16,8);state=rd(root+663*96+80,8);u=rd(state+2528,8)
if rd(u,4)==0x47525846:u=rd(u+8,8)
print('ui loaded',rd(u+32,4),'slot',rd(u+36,4),'generation',rd(u+48,8),'save_error',rd(u+28,4),'command',hex(rd(u+16,4)),'reload',rd(u+20,4))
b=bytes(p.debug.read(0xa1171b000,0x48));import struct
v=struct.unpack_from('<4I',b,0);r=struct.unpack_from('<10I',b,32);f,l=struct.unpack_from('<2Q',b,16)
print('cardslot drive',v[1],'reason',hex(v[2]),'probes',v[3],'rewrites',r[6],'refused',r[7],'decided after %.2f s'%((l-f)/24e6))
