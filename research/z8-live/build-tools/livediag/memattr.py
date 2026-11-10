"""Read-only: page attributes (PAR, MAIR byte) of the Live-related buffers."""
import struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import livectl as L
t,p=L.connect()
try:
    eng=L.engine(p);q=lambda a:struct.unpack('<Q',p.debug.read(a,8))[0]
    hw=q(eng+L.O_HW)
    region=[q(hw+56+8*i) for i in range(11)];slot=[q(hw+144+8*i) for i in range(4)];arena=q(hw+48)
    rows=[('runtime(heap)',eng),('hw struct',hw),('hw->au',hw+0x1000),('arena',arena),('slot0',slot[0]),('bs0',region[3]),
          ('lv buf',0xa1f5f9800),('lv buf2',0xa1fec3800),('i420 (stream pixels)',q(eng+1600+56) if False else 0)]
    for name,a in rows:
        if not a:continue
        r=p.debug.translate(a);print(f'{name:22s} va={a:#x} pa={r["physical"]} attr={r["attribute"]:#04x} fault={r["fault"]}')
    # look for other VAs mapping the slot / lv buffer physical page (common alias offsets)
    for name,a in (('slot0',slot[0]),('lv buf',0xa1f5f9800)):
        pa=int(p.debug.translate(a)['physical'],16)
        for va in (pa, pa+0x800000000, pa+0xa00000000, pa-0x800000000+0xa00000000, pa+0x400000000, pa+0xc00000000):
            if va<=0 or va>=1<<48:continue
            r=p.debug.translate(va)
            if not r['fault']:print(f'  alias? {name} pa={pa:#x} va={va:#x} -> pa={r["physical"]} attr={r["attribute"]:#04x}')
finally:
    t.__exit__(None,None,None)
