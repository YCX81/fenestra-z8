"""Edits for the portrait movie brightness display (histogram/waveform) bug.

The C3.11 OSD layout exists twice: table A 0x43a21a70 and table B 0x43a3ce90, 644 records of
68 bytes each, byte-identical in stock (descriptors at 0x44d1b2a8 and 0x44d1c838). The portrait
layout layer only filled the portrait fields of table A. The brightness widget 0x1f41 refreshes its
rect from table B every frame, finds an empty portrait rect and falls back to landscape (474,254),
which wraps across the 480-wide portrait canvas (hardware read 2026-11-11: portrait GUI object
0xa03287ea0 rewritten to 474,254,636,336 within milliseconds).

Fix: table B := table A, and the 0x1f41 portrait rect := (252,368,414,450) in both tables, the
non-overlapping spot nearest the stock photo-mode portrait histogram (218,448,380,530).
Portrait fields are only read while the video OSD may rotate (portrait gate on).
Usage: osd_tableb_fix.py IN.main OUT.json"""
import json,struct,sys
from pathlib import Path
B=0x40160000;A,TB,N,R=0x43a21a70,0x43a3ce90,644,68
WID,RECT=0x1f41,(252,368,414,450)
d=Path(sys.argv[1]).read_bytes()
assert struct.unpack_from('<2Q',d,0x44d1b2a8-B)==(A,A+N*R) and struct.unpack_from('<2Q',d,0x44d1c838-B)==(TB,TB+N*R)
ta=bytearray(d[A-B:A-B+N*R]);tb=bytes(d[TB-B:TB-B+N*R])
hits=0
for i in range(N):
    o=i*R
    assert struct.unpack_from('<I',ta,o+56)==struct.unpack_from('<I',tb,o+56),i   # same widget order
    if struct.unpack_from('<I',ta,o+56)[0]==WID:
        land=struct.unpack_from('<4h',ta,o+24)
        assert land==(474,254,636,336) and struct.unpack_from('<4h',ta,o+32)==(272,402,434,484),(i,land)
        struct.pack_into('<4h',ta,o+32,*RECT);hits+=1
assert hits==1
edits=[]
for base,new,old in ((A,bytes(ta),d[A-B:A-B+N*R]),(TB,bytes(ta),tb)):
    i=0
    while i<len(new):
        if new[i]!=old[i]:
            j=i
            while j<len(new) and new[j]!=old[j]:j+=1
            edits.append([hex(base+i),old[i:j].hex(),new[i:j].hex()]);i=j
        else:i+=1
Path(sys.argv[2]).write_text(json.dumps(edits))
print('edits',len(edits),'bytes',sum(len(e[2])//2 for e in edits))
