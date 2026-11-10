#!/usr/bin/env python3
"""Decode LIVEW5A/B.BIN (v33 hand-back records): w5log.py FILE..."""
import struct, sys
N = {1:'hook enter',2:'busy over',10:'before STOP',11:'before TRIGGER',12:'picture done',13:'before drain',14:'worker 5',15:'before FINISH',16:'after FINISH',17:'before EXIT',18:'EXIT returned',19:'VDI down',30:'close returned',40:'tick'}
for f in sys.argv[1:]:
    d = open(f,'rb').read(); print('==', f, len(d))
    for i in range(0, min(len(d),2048), 64):
        seq, st, t, a, b, wk, u, ist, vdi, up, m, slot, core, busy, pad = struct.unpack_from('<IIQ12I', d, i) if False else (lambda v:(v[0],v[1],v[2],*v[3:]))(struct.unpack_from('<IIQ12I', d, i))
        if seq: print(f'{seq:3d} {t/1e6:10.3f}s {N.get(st,st):15} a={a:#x} b={b:#x} worker={wk} inst=({u},{ist:#x}) vdi={vdi:#x} up={up} map4={m} slot={slot} coreA={core} busy={busy}')
