#!/usr/bin/env python3
"""Small static helpers over the original C3.11 MAIN (read-only analysis).
  mainre.py strings PATTERN            ASCII strings matching a regex, with addresses
  mainre.py xref ADDR [ADDR...]        code that forms ADDR (adrp+add, or literal pointers)
  mainre.py dis ADDR [COUNT]           disassemble COUNT instructions
  mainre.py imm VALUE                  movz/movk pairs that build a 32-bit constant
  mainre.py calls ADDR                 bl sites that call ADDR
MAIN defaults to ~/fs-out/stock.main (override with Z8_MAIN)."""
import os, re, struct, sys
from pathlib import Path
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

LOAD = 0x40160000
data = Path(os.environ.get('Z8_MAIN', str(Path.home() / 'fs-out/stock.main'))).read_bytes()
md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
CODE_END = 0x3000000   # executable part of the image is well below this offset


def words():
    return struct.unpack_from('<%dI' % (CODE_END // 4), data, 0)


def cmd_strings(pattern):
    rx = re.compile(pattern.encode())
    for m in re.finditer(rb'[\x20-\x7e]{4,200}', data):
        if rx.search(m.group()):
            print(hex(LOAD + m.start()), m.group().decode())


def cmd_xref(targets):
    targets = [int(t, 16) for t in targets]
    w = words()
    for i, x in enumerate(w):
        if x & 0x9f000000 == 0x90000000:           # adrp
            rd = x & 31
            imm = ((x >> 5) & 0x7ffff) << 2 | ((x >> 29) & 3)
            if imm & (1 << 20): imm -= 1 << 21
            page = ((LOAD + i * 4) & ~0xfff) + (imm << 12)
            for j in range(i + 1, min(i + 6, len(w))):
                y = w[j]
                if y & 0xffc00000 == 0x91000000 and (y >> 5) & 31 == rd:   # add imm
                    value = page + ((y >> 10) & 0xfff)
                    if value in targets:
                        print('adrp/add', hex(LOAD + i * 4), '->', hex(value))
    for t in targets:
        needle = struct.pack('<Q', t)
        at = data.find(needle)
        while at >= 0:
            print('pointer', hex(LOAD + at), '->', hex(t))
            at = data.find(needle, at + 1)


def cmd_dis(addr, count=40):
    addr = int(addr, 16)
    for ins in md.disasm(data[addr - LOAD:addr - LOAD + 4 * int(count)], addr):
        print(hex(ins.address), ins.mnemonic, ins.op_str)


def cmd_imm(value):
    value = int(value, 16)
    lo, hi = value & 0xffff, value >> 16
    w = words()
    for i, x in enumerate(w[:-1]):
        # movz wN,#lo ; movk wN,#hi,lsl16   or   movz wN,#hi,lsl16 ; movk wN,#lo
        if x & 0x7fe00000 == 0x52800000 and (x >> 5) & 0xffff == lo:
            y = w[i + 1]
            if y & 0x7fe00000 == 0x72a00000 and (y >> 5) & 0xffff == hi and (y & 31) == (x & 31):
                print(hex(LOAD + i * 4))
        if x & 0x7fe00000 == 0x52a00000 and (x >> 5) & 0xffff == hi:
            y = w[i + 1]
            if y & 0x7fe00000 == 0x72800000 and (y >> 5) & 0xffff == lo and (y & 31) == (x & 31):
                print(hex(LOAD + i * 4))


def cmd_calls(addr):
    addr = int(addr, 16)
    for i, x in enumerate(words()):
        if x >> 26 == 37:
            off = x & 0x3ffffff
            if off & (1 << 25): off -= 1 << 26
            if LOAD + i * 4 + off * 4 == addr:
                print(hex(LOAD + i * 4))


if __name__ == '__main__':
    c, a = sys.argv[1], sys.argv[2:]
    {'strings': lambda: cmd_strings(a[0]), 'xref': lambda: cmd_xref(a), 'dis': lambda: cmd_dis(*a),
     'imm': lambda: cmd_imm(a[0]), 'calls': lambda: cmd_calls(a[0])}[c]()
