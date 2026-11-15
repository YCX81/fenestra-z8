import struct,sys
sys.path.insert(0,'.')
import livectl
t,p=livectl.connect()
b=bytes(p.debug.read(0xa1171b000,0x48))
names=['magic','drive','reason','probes']
v=struct.unpack_from('<4I',b,0);d=dict(zip(names,[hex(x) for x in v]))
d['first'],d['last']=struct.unpack_from('<2Q',b,16)
rest=struct.unpack_from('<10I',b,32)
d.update(dict(zip(['code_a','code_b','root_a','root_b','reg_a','reg_b','rewrites','refused','class_a','class_b'],[hex(x) for x in rest])))
d['secs']=(d['last']-d['first'])/24e6
print(d)
