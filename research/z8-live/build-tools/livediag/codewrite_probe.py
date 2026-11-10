"""Read-only: can the Live code region be written at EL1? (AT S1E1W via the debug monitor; no write.)"""
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import livectl as L
t,p=L.connect()
try:
    for name,a in (('live payload',0x401e2000),('payload tail pad',0x401fff00),('main code',0x40d60ec8),('heap',0xa10d4a000)):
        r=p.debug.translate(a);w=p.debug.translate(a,True)
        print(f'{name:18s} {a:#x} read: attr={r["attribute"]:#04x} fault={r["fault"]}  write: fault={w["fault"]} par={w["par"]}')
finally:
    t.__exit__(None,None,None)
