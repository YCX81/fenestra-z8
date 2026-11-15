#!/usr/bin/env python3
"""Strict 8-byte diagnostic core-profile overlay. Does not build updater files."""
import argparse,hashlib,json,struct
from pathlib import Path
LOAD=0x40160000
SITE=0x401def00
OLD=0x403efed8
SYMBOL='live_ui_evf_submit'
def patch(data, target):
    assert target%4==0 and (0x401e2000<=target<0x401fd000 or 0x401fd218<=target<0x401fe000),hex(target)
    off=SITE-LOAD
    assert data[off:off+8]==struct.pack('<Q',OLD),('unexpected original core submit pointer',data[off:off+8].hex())
    out=bytearray(data);out[off:off+8]=struct.pack('<Q',target)
    assert out[:off]==data[:off] and out[off+8:]==data[off+8:]
    return bytes(out)
def main():
    from elftools.elf.elffile import ELFFile
    p=argparse.ArgumentParser();p.add_argument('main',type=Path);p.add_argument('elf',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    assert a.output.resolve()!=a.main.resolve(),'never modify input'
    data=a.main.read_bytes()
    with a.elf.open('rb') as f:
        e=ELFFile(f);ss=e.get_section_by_name('.symtab').get_symbol_by_name(SYMBOL);assert ss and len(ss)==1
        target=ss[0]['st_value']
        # Requires the caller to have already composed this exact linked payload.
        for s in e.iter_sections():
            if not (s['sh_flags']&2 and s['sh_size']):continue
            off=s['sh_addr']-LOAD;assert off>=0 and data[off:off+s['sh_size']]==s.data(),('payload mismatch',s.name)
    out=patch(data,target);a.output.write_bytes(out)
    report=dict(schema='v53.evf-passive-profile/v1',address=hex(SITE),size=8,expected=struct.pack('<Q',OLD).hex(),replacement=struct.pack('<Q',target).hex(),symbol=SYMBOL,input_sha256=hashlib.sha256(data).hexdigest(),output_sha256=hashlib.sha256(out).hexdigest(),scope='Only the Fenestra core shared-profile submit call is counted; not all stock compositor submits or physical scanout.')
    a.output.with_suffix(a.output.suffix+'.evf.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
if __name__=='__main__':main()
