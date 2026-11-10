#!/usr/bin/env python3
"""Local v3b/reference/native-context and final lamp placement evidence.
This is not an extension of verify_link's CFG proof, nor hardware acceptance.
"""
import argparse,hashlib,io,json
from pathlib import Path
from capstone import Cs,CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN
from elftools.elf.elffile import ELFFile
BASE=0x40160000
REFERENCE_SHA='6daae383c5b81cc55c97c76cb79eeb953a51eaa911ea0d3bf86ed5f7e087033e'

def inspect(root,linked):
    reference=root/'v3b/linked-recorder'
    old=(reference/'live-fixed.emulation-only.main').read_bytes()
    current=(linked/'live-fixed.emulation-only.main').read_bytes()
    assert hashlib.sha256(old).hexdigest()==REFERENCE_SHA
    def elf_symbols(path,main):
        e=ELFFile(io.BytesIO(path.read_bytes()))
        for sec in e.iter_sections():
            if sec['sh_flags']&2 and sec['sh_size']:
                assert sec.data()==main[sec['sh_addr']-BASE:sec['sh_addr']-BASE+sec['sh_size']]
        return {s.name:s['st_value'] for s in e.get_section_by_name('.symtab').iter_symbols()}
    oldsy=elf_symbols(reference/'payload.elf',old)
    sy=elf_symbols(linked/'payload.elf',current)
    d=Cs(CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN)
    def dis(data,a,n):return list(d.disasm(data[a-BASE:a-BASE+n],a))
    def one(data,a):return dis(data,a,4)[0]
    base=(root/'bg34.main').read_bytes()
    for a in (0x402f5870,0x402f58cc,0x402f4c1c,0x402f4560):
        assert current[a-BASE:a-BASE+4]==base[a-BASE:a-BASE+4],hex(a)
    assert one(old,0x402f3068).op_str=='x19, x30, [sp, #0x70]'
    assert one(old,0x402f2d68).op_str=='x19, x30, [sp, #0x70]'
    assert one(old,0x402f2d6c).op_str=='w19, w0'
    assert one(old,0x402f2b8c).op_str=='w1, [x9, w8, uxtw]'
    assert one(current,sy['fr_notify_call']).mnemonic=='bl'
    assert one(current,sy['fr_notify_call']).op_str=='#'+hex(sy['live_w5_bringup_notify_lamp'])
    assert sy['fr_lamp_r']==sy['fr_notify_call']+4
    assert sy['fr_lamp_i']<sy['fr_notify_call']
    assert 'fr_enabled' not in sy and 'fr_encoder_snapshot' not in sy and 'live_encoder_fr_snapshot' not in sy
    assert one(current,sy['fr_stock_3']).bytes.hex()=='f37b47a9'
    assert one(current,sy['fr_stock_3']+4).op_str=='#0x402f306c'
    # Full fixed GPIO and DELAY implementations are identical to the reference.
    for a,n in ((0x40187b00,0x170),(0x40188530,0xc8),(0x40c15c3c,0x44)):
        assert current[a-BASE:a-BASE+n]==old[a-BASE:a-BASE+n]
    gpio=dis(old,0x40c15c3c,0x44)
    assert gpio[-1].mnemonic=='ret' and not any(i.mnemonic in ('bl','blr') for i in gpio)
    delay={i.address:(i.mnemonic,i.op_str) for i in dis(old,0x40187b00,0x170)}
    assert delay[0x40187b2c]==('cbnz','w9, #0x40187c48')
    assert delay[0x40187b34]==('tbnz','w8, #7, #0x40187c48')
    assert delay[0x40187b54]==('cbz','w8, #0x40187c5c')
    assert delay[0x40187c48]==('mov','w0, #-0x19')
    assert delay[0x40187c10]==('add','x2, x9, w19, uxtw')
    assert delay[0x40187c30]==('bl','#0x40189828')
    # DELAY installs this expiry callback; it publishes a zero return status.
    assert one(old,0x40188548).mnemonic=='str' and one(old,0x40188548).op_str=='wzr, [x19, #0x3c]'
    assert delay[0x40187c34]==('ldr','w0, [x20, #0x3c]')
    names=['live_w5_bringup_entry','fr_lamp_i','fr_notify_call','fr_lamp_r','fr_stock_1',
           'fr_entry_2','fr_stock_2','fr_entry_3','fr_stock_3']
    report=dict(reference_sha256=REFERENCE_SHA,final_sha256=hashlib.sha256(current).hexdigest(),
        addresses={k:hex(sy[k]) for k in names},old_bringup=hex(oldsy['live_w5_bringup_entry']),
        tests='PASS: reference ELF/MAIN, original S/M stack/bind, I/R linked ordering, removed hook bytes, unchanged GPIO/DELAY',
        limitation='Static context and runtime DELAY guard only; task lock ownership, scheduling/IRQ behavior and physical light not hardware tested')
    (linked/'lamp-audit.json').write_text(json.dumps(report,indent=2)+'\n')
    evidence=[]
    for label,data,ranges in [('v3b',old,[(0x40187b00,0x170),(0x40188530,0xc8),(0x40c15c3c,0x44),(0x402f2fd8,0xac),(0x402f2b7c,0x18)]),
        ('lamp-v4',current,[(sy['fr_lamp_i']-8,0x48),(sy['fr_stock_1'],8),(sy['fr_stock_2'],12),(sy['fr_stock_3'],8)])]:
        for at,n in ranges:
            evidence.append(f'\n{label} {at:#x}')
            evidence.extend(f'{i.address:08x} {i.bytes.hex()} {i.mnemonic} {i.op_str}' for i in dis(data,at,n))
    (linked/'lamp-disassembly.txt').write_text('\n'.join(evidence)+'\n')
    print(json.dumps(report,indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('linked',type=Path)
    a=p.parse_args();inspect(Path(__file__).resolve().parents[2],a.linked)
