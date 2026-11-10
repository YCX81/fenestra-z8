#!/usr/bin/env python3
"""win-lab 离线 fixture：原厂 NP3 转换 + card-scheme keys，无 QEMU/设备访问。"""
import argparse
import importlib.util
from pathlib import Path
import struct
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--np3', type=Path, help='Optional real NP3; default is make_flexible_np3 fixture')
a = p.parse_args()
root = Path.home() / 'Nikon'
sys.path.insert(0, str(root / 'z8-qemu/tools'))
from z8_slots_core import NativeSlots
from z8_slots_layout import Layout as ImportLayout
from z8_picture_control import make_flexible_np3
layout_path = root / '_bundle-20261102/slotcard/tools/z8_slots_layout.py'
spec = importlib.util.spec_from_file_location('c64label_card_layout', layout_path)
mod = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = mod
spec.loader.exec_module(mod)
card = mod.Layout(64)
native = NativeSlots((root / 'z8-qemu/.local/firmware/z8-c311-main.bin').read_bytes(), ImportLayout(64))
a.out.mkdir(parents=True, exist_ok=False)
for bank in (0, 1):
    for slot in (10, 11):
        blob = a.np3.read_bytes() if a.np3 else make_flexible_np3(f'C{slot} LAB M{bank}', slot+bank*64)
        native.uc.mem_write(native.layout.address('input'), blob)
        if native.call(native.entries['import_np3'], native.layout.address('input'), len(blob), slot, bank) != 1:
            raise RuntimeError('Original NP3 importer rejected fixture')
        raw = bytearray(native.select(slot, bank))
        assert len(raw) == 0x400 and struct.unpack_from('<I', raw)[0] == 1
        struct.pack_into('<3I', raw, 4, *card.keys(slot, bank))
        idx = bank*64 + slot-1
        (a.out / f'{idx:03d}.bin').write_bytes(raw)
        print(bank, slot, hex(card.selector(slot, bank)), [hex(k) for k in card.keys(slot, bank)])
