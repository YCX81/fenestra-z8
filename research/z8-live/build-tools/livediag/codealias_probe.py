"""Read-only: look for a writable VA alias of the Live payload's physical pages (AT only, no writes)."""
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import livectl as L
t,p=L.connect()
try:
    r=p.debug.translate(0x401e2000);pa=int(r['physical'],16);print('payload pa',hex(pa),'attr',hex(r['attribute']))
    for off in (0,0x400000000,0x800000000,0xa00000000-0x40000000,0x9c0000000,0x200000000,0x100000000,0x1000000000):
        va=pa+off
        a=p.debug.translate(va);w=p.debug.translate(va,True)
        print(f'va={va:#x} read_fault={a["fault"]} pa={a["physical"]} attr={a["attribute"]:#04x} write_fault={w["fault"]}')
finally:
    t.__exit__(None,None,None)
