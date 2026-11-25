#!/usr/bin/env python3
"""Bind the one native automatic-review consumer to a linked wrapper.

Exactly one BL changes. Does not allocate a code bank, rewrite settings, or
recompute build ID. Caller must provide the linked symbol and exact input SHA.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

BASE=0x40160000
SITE=0x40407f84
WINDOW=0x40407f7c
GUARD=bytes.fromhex('e00b1e32e1230091495bff9760120036157a81d2')
POP=0x403deca8
POP_BYTES=bytes.fromhex('08868ed26865a0f24801c0f20800e0f209c96038490100340a7c4093e10000b40b2d8ed26b65a0f24b01c0f20b00e0f26b796af82b0000f91f692a383f010071e0079f1ac0035fd6')

def patch(data,expected,target):
    assert hashlib.sha256(data).hexdigest()==expected,'input SHA mismatch'
    assert data[WINDOW-BASE:WINDOW-BASE+len(GUARD)]==GUARD,'native slot1c consumer window changed'
    assert data[POP-BASE:POP-BASE+len(POP_BYTES)]==POP_BYTES,'native pending pop changed'
    assert 0x40160000<=target<0x40200000 and target%4==0,'wrapper outside expected executable extension area'
    assert any(data[target-BASE:target-BASE+16]),'wrapper target is empty'
    delta=target-SITE
    assert delta%4==0 and -(1<<27)<=delta<(1<<27)
    replacement=struct.pack('<I',0x94000000|((delta//4)&0x3ffffff))
    out=bytearray(data);out[SITE-BASE:SITE-BASE+4]=replacement
    assert out[:SITE-BASE]==data[:SITE-BASE] and out[SITE-BASE+4:]==data[SITE-BASE+4:]
    return bytes(out),{'address':hex(SITE),'before':data[SITE-BASE:SITE-BASE+4].hex(),'after':replacement.hex(),'target':hex(target)}

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input',type=Path,required=True);p.add_argument('--expected-sha256',required=True)
    p.add_argument('--target',type=lambda x:int(x,0),required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--receipt',type=Path,required=True)
    a=p.parse_args();assert not a.output.exists() and not a.receipt.exists()
    original=a.input.read_bytes();changed,entry=patch(original,a.expected_sha256,a.target)
    a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_bytes(changed)
    a.receipt.write_text(json.dumps({'input_sha256':a.expected_sha256,'output_sha256':hashlib.sha256(changed).hexdigest(),
      'hook':entry,'changed_span_bytes':4,'native_property_changed':False,'native_pop_changed':False,
      'build_id_changed':False,'scope':'Single auto-review slot1c BL only; linked implementation/ABI must be independently validated'},indent=2)+'\n')
