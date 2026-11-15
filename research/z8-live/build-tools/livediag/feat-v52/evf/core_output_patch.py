#!/usr/bin/env python3
"""Pinned v51d core tuple adapter patch; apply after linking PB and composing payloads."""
import struct
BASE=0x40160000
# Every byte here is independently pinned to final v51d, not the evolving payloads.
EXPECTED={0x401a38c8:'e8031faa',0x401a337c:'a80e41b9',0x401a3380:'1f050071',0x401a3384:'21230054',
          0x401deec0:'d409344000000000',0x401deec8:'a409344000000000',0x401deed8:'640d344000000000'}
def branch(source,target,link=False):
    d=target-source
    assert d%4==0 and -(1<<27)<=d<(1<<27)
    return struct.pack('<I',(0x94000000 if link else 0x14000000)|((d>>2)&0x3ffffff))
def apply(buf,symbols):
    b=bytearray(buf);changes=[]
    for a,h in EXPECTED.items():assert b[a-BASE:a-BASE+len(bytes.fromhex(h))]==bytes.fromhex(h),(hex(a),b[a-BASE:a-BASE+len(bytes.fromhex(h))].hex(),h)
    for n in ['pb_output_initialize_global','pb_output_valid','pb_output_backing_rectangle','pb_output_target_enabled','pb_output_target_enable']:
        assert 0x40196000<=symbols[n]<0x401a0000,(n,symbols[n])
    repl={0x401a38c8:branch(0x401a38c8,symbols['pb_output_initialize_global']),
          0x401a337c:struct.pack('<I',0xaa1503e0), # mov x0,x21
          0x401a3380:branch(0x401a3380,symbols['pb_output_valid'],True),
          0x401a3384:struct.pack('<I',0x34000000|(((0x401a37e8-0x401a3384)>>2)<<5)),
          0x401deec0:struct.pack('<Q',symbols['pb_output_target_enabled']),
          0x401deec8:struct.pack('<Q',symbols['pb_output_target_enable']),
          0x401deed8:struct.pack('<Q',symbols['pb_output_backing_rectangle'])}
    for a,v in repl.items():
        o=a-BASE;old=bytes(b[o:o+len(v)]);b[o:o+len(v)]=v
        changes.append(dict(address=hex(a),old=old.hex(),new=v.hex()))
    assert sum(len(v) for v in repl.values())==40
    return b,changes
if __name__=='__main__':
    import argparse,json,hashlib
    from pathlib import Path
    ap=argparse.ArgumentParser();ap.add_argument('input');ap.add_argument('symbols');ap.add_argument('output');args=ap.parse_args()
    sy=json.loads(Path(args.symbols).read_text());sy=sy.get('symbols',sy);sy={k:int(v,0) if isinstance(v,str) else v for k,v in sy.items()}
    b,changes=apply(Path(args.input).read_bytes(),sy);Path(args.output).write_bytes(b)
    Path(args.output+'.evf-core.json').write_text(json.dumps(dict(changes=changes,sha256=hashlib.sha256(b).hexdigest()),indent=2)+'\n')
