#!/usr/bin/env python3
"""Execute the final linked PB bridge in Unicorn with explicit stock-service stubs.

This tests compiled bridge control flow, path redirection, DCF mask ownership,
and return IDs. It does not emulate SD/CFexpress, GUI scheduling, or codecs.
Run with the existing lab venv (unicorn, keystone, pyelftools).
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

from elftools.elf.elffile import ELFFile
from keystone import Ks, KS_ARCH_ARM64, KS_MODE_LITTLE_ENDIAN
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as A


def sha(b):
    return hashlib.sha256(b).hexdigest()


class Bridge:
    STATE = 0xA01000000
    STACK = 0xA02000000
    SCRATCH = 0xA03000000
    SLOT = 0xA10000000
    STOP = 0x40100000

    def __init__(self, main, symbols, layout, source_id, source_path, target_path):
        self.u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.symbols = symbols
        self.layout = layout
        self.maps = set()
        self.calls = []
        self.source_id = source_id
        self.selected = source_id
        self.source_path = source_path
        self.target_path = target_path
        self.node = self.SLOT + 0x1000
        self.time_us = 10_000_000
        self.stubs = {}
        self.map(0x40196000, 0xA000)
        self.u.mem_write(0x40196000, main[0x36000:0x40000])
        # These are linked core helpers outside the PB cave; execute the exact
        # final MAIN bytes rather than substituting host memcpy semantics.
        for name in ['memcpy', 'memmove', 'memset', 'memcmp', 'strlen', 'strcmp']:
            address = symbols.get(name, 0)
            if 0x40160000 <= address < 0x40160000 + len(main):
                page = address & ~4095
                self.map(page, 8192)
                at = page - 0x40160000
                self.u.mem_write(page, main[at:at + 8192])
        self.map(self.STATE, 0x10000)
        self.map(self.STACK, 0x10000)
        self.map(self.SCRATCH, 0x10000)
        self.map(self.SLOT, 0x224000)
        self.map(self.STOP, 0x1000)
        for addr in [0xA032280C8, 0xA04822AF8]:
            self.map(addr, 16)
        self.write(0xA032280C8, 0xA0284A0E0, 8)
        self.write(0xA04822AF8 + ((source_id >> 29) & 3) * 8, self.SLOT, 8)
        self.write(self.SCRATCH, source_id)
        self.write(self.node + 0x24, 1 << (source_id & 15), 2)
        for field, value in {
            'magic': 0x50425549, 'bytes': layout['struct_bytes'], 'key': 9,
            'media_epoch': 1, 'lookup_epoch': 1, 'cache_valid': 1,
            'current_id': source_id, 'cached_id': source_id,
            'nb_w': 1024, 'nb_h': 682, 'nb_request': 1,
            'preview_fd': 0xFFFFFFFF,
        }.items():
            self.set(field, value)
        self.string(self.field('native_path'), source_path)
        self.string(self.field('manifest.native_path'), target_path)
        self.install_stubs()
        ks = Ks(KS_ARCH_ARM64, KS_MODE_LITTLE_ENDIAN)
        self.counter_ops = {}
        for ins, value in [('mrs x0, cntpct_el0', self.time_us),
                           ('mrs x0, cntfrq_el0', 1_000_000)]:
            encoded, _ = ks.asm(ins)
            self.counter_ops[int.from_bytes(bytes(encoded), 'little') & ~31] = value
        self.u.hook_add(UC_HOOK_CODE, self.hook)

    def map(self, at, size):
        for page in range(at & ~4095, (at + size + 4095) & ~4095, 4096):
            if page not in self.maps:
                self.u.mem_map(page, 4096)
                self.maps.add(page)

    def field(self, name):
        return self.STATE + self.layout['offsets'][name]

    def set(self, name, value):
        self.write(self.field(name), value, self.layout['widths'][name])

    def get(self, name):
        return self.read(self.field(name), self.layout['widths'][name])

    def write(self, at, value, size=4):
        self.u.mem_write(at, int(value).to_bytes(size, 'little'))

    def read(self, at, size=4):
        return int.from_bytes(self.u.mem_read(at, size), 'little')

    def string(self, at, text):
        self.u.mem_write(at, text.encode() + b'\0')

    def cstring(self, at):
        return bytes(self.u.mem_read(at, 64)).split(b'\0')[0].decode()

    def x(self, n):
        return self.u.reg_read(getattr(A, 'UC_ARM64_REG_X' + str(n)))

    def ret(self, value=0, second=None):
        self.u.reg_write(A.UC_ARM64_REG_X0, value)
        if second is not None:
            self.u.reg_write(A.UC_ARM64_REG_X1, second)
        self.u.reg_write(A.UC_ARM64_REG_PC, self.x(30))

    def stub(self, target, fn):
        at = self.symbols[target] if isinstance(target, str) else target
        self.map(at, 4)
        self.stubs[at] = fn

    def install_stubs(self):
        self.stub('pb_runtime_get', lambda: self.ret(self.STATE))
        self.stub('pb_boot', lambda: self.ret())
        self.stub('pb_ui_old_poll', lambda: self.ret(0))
        self.stub('pb_native_current_view', lambda: self.ret(0xA0284A0E0, 2 | (1 << 32)))
        self.stub(0x40393F6C, lambda: self.ret(2))
        self.stub(0x40393F28, lambda: self.ret(1))
        self.stub(0x407B4E78, lambda: self.ret(self.SCRATCH))
        self.stub(0x403EC398, lambda: self.ret(0 if self.x(0) == 0x491A else self.selected))

        def setting():
            self.calls.append(['setting', self.x(0), self.x(1)])
            if self.x(0) == 0x4910:
                self.selected = self.x(1)
            self.ret()
        self.stub(0x403EC3A0, setting)

        def page():
            self.calls.append(['page', self.x(0), self.x(1)])
            self.ret()
        self.stub(0x403AD6A4, page)
        self.stub(0x4085E91C, lambda: self.ret(self.SLOT + 0x800))
        self.stub(0x4086C378, lambda: self.ret(self.node))
        for address in [0x4085D348, 0x4085D364, 0x40875834, 0x408758B4,
                        0x406364D0, 0x403FB660]:
            self.stub(address, lambda: self.ret())

        def file_call(kind):
            self.calls.append([kind, self.cstring(self.x(0))])
            self.ret(123 if kind == 'open' else 0)
        self.stub(0x402DD4F8, lambda: file_call('open'))
        self.stub(0x402DADF4, lambda: file_call('stat'))
        self.stub('pb_display_detach', lambda: self.ret())

        def key_original():
            self.native_key_body = bytes(self.u.mem_read(self.x(0), 124))
            self.ret(0x9876)
        self.stub('pb_key_original', key_original)

    def hook(self, uc, address, size, _):
        if address == self.STOP:
            uc.emu_stop()
        elif address in self.stubs:
            self.stubs[address]()
        else:
            word = self.read(address)
            if word & ~31 in self.counter_ops:
                uc.reg_write(getattr(A, 'UC_ARM64_REG_X' + str(word & 31)),
                             self.counter_ops[word & ~31])
                uc.reg_write(A.UC_ARM64_REG_PC, address + 4)

    def call(self, name, *args):
        for i, value in enumerate(args):
            self.u.reg_write(getattr(A, 'UC_ARM64_REG_X' + str(i)), value)
        self.u.reg_write(A.UC_ARM64_REG_SP, self.STACK + 0xFFF0)
        self.u.reg_write(A.UC_ARM64_REG_X30, self.STOP)
        self.u.emu_start(self.symbols[name], self.STOP + 4, count=200_000)
        assert self.u.reg_read(A.UC_ARM64_REG_PC) == self.STOP, 'instruction limit reached'
        return self.x(0)


def run(main, symbols, layout):
    cases = []
    for fmt, source, target in [
        (0, r'A:\DCIM\108NCZ_8\DSC_9085.JPG', r'A:\DCIM\108NCZ_8\DSC_9085.JPG'),
        (2, r'A:\DCIM\108NCZ_8\DSC_9085.NEF', r'B:\DCIM\108NCZ_8\DSC_9085.JPG'),
    ]:
        ident = (108 << 19) | (9085 << 4) | fmt
        m = Bridge(main, symbols, layout, ident, source, target)
        original_mask = m.read(m.node + 0x24, 2)
        m.set('nb_request', 0)
        body = bytearray(124)
        body[9] = body[0x14] = 1
        m.u.mem_write(m.SCRATCH + 0x800, bytes(body))
        assert m.call('pb_key_decode_wrap', m.SCRATCH + 0x800, 0x1234, 0x5678) == 0x9876
        assert bytes(m.u.mem_read(m.SCRATCH + 0x800, 124)) == bytes(body)
        assert m.get('consumed_rec') == 1 and m.get('nb_request') == 1
        assert m.native_key_body[9] == 0 and m.native_key_body[0x14] == 1
        m.call('pb_ui_poll_wrap', 0)
        assert m.get('nb_stage') == 2 and m.get('nb_photo') == ident
        assert m.get('nb_movie') == (ident & ~15) | 5
        assert m.selected == (ident & ~15) | 5
        assert ['page', 3, 0x56C] in m.calls
        virtual_movie = source[:-4] + '.MOV'
        actual_movie = target.replace('\\DCIM\\', '\\LIVE\\')[:-4] + '.MOV'
        at = m.SCRATCH + 0x100
        m.string(at, virtual_movie)
        m.call('pb_open_wrap', at, 0, 0x20)
        m.call('pb_stat_wrap', at, m.SCRATCH + 0x200)
        assert ['open', actual_movie] in m.calls
        assert ['stat', actual_movie] in m.calls
        assert m.read(m.node + 0x24, 2) == original_mask | 0x20
        m.call('pb_native_return', 3, 0x552)
        assert m.selected == ident and m.get('nb_stage') == 3
        m.call('nb_end', m.STATE)
        assert m.get('nb_stage') == 0 and not m.get('nb_bit')
        assert m.read(m.node + 0x24, 2) == original_mask
        cases.append({'name': 'RAW peer return' if fmt else 'JPG return', 'passed': True,
                      'source_id': hex(ident), 'source': source, 'target': target,
                      'rec_consumed_once_input_preserved': True,
                      'calls': m.calls})

        m = Bridge(main, symbols, layout, ident, source, target)
        m.write(m.node + 0x24, original_mask | 0x20, 2)
        m.call('pb_ui_poll_wrap', 0)
        assert m.get('nb_stage') == 0 and m.get('nb_fail') == 2
        assert m.read(m.node + 0x24, 2) == original_mask | 0x20
        assert not any(x[0] == 'page' for x in m.calls)
        cases.append({'name': ('RAW' if fmt else 'JPG') + ' existing MOV preserved', 'passed': True})

        m = Bridge(main, symbols, layout, ident, source, target)
        m.set('lookup_epoch', 0)
        m.call('pb_ui_poll_wrap', 0)
        assert not m.get('nb_stage') and m.get('nb_fail') == 1
        assert m.read(m.node + 0x24, 2) == original_mask
        cases.append({'name': ('RAW' if fmt else 'JPG') + ' stale epoch rejected', 'passed': True})

        m = Bridge(main, symbols, layout, ident, source, target)
        m.set('cached_id', ident + 16)
        m.call('pb_ui_poll_wrap', 0)
        assert not m.get('nb_stage') and m.get('nb_fail') == 1
        assert m.read(m.node + 0x24, 2) == original_mask
        cases.append({'name': ('RAW' if fmt else 'JPG') + ' stale photo cache rejected', 'passed': True})

        m = Bridge(main, symbols, layout, ident, source, target)
        m.call('pb_ui_poll_wrap', 0)
        message = m.SCRATCH + 0x300
        m.write(message + 32, 24)
        m.write(message + 40, 1)
        m.call('pb_system_notify', message)
        assert m.get('media_epoch') == 2 and m.get('card_epoch[0]') == 0
        assert m.get('card_epoch[1]') == 1 and not m.get('cache_valid')
        m.string(m.SCRATCH + 0x100, virtual_movie)
        m.call('pb_open_wrap', m.SCRATCH + 0x100, 0, 0x20)
        assert m.calls[-1] == ['open', virtual_movie], 'stale peer redirect remained active'
        m.call('pb_native_return', 3, 0x552)
        assert m.selected == ident, 'unchanged source card did not regain original photo'
        m.call('nb_end', m.STATE)
        assert m.read(m.node + 0x24, 2) == original_mask
        assert not m.get('nb_stage') and not m.get('nb_bit')
        cases.append({'name': ('RAW' if fmt else 'JPG') + ' other-card removal restores source', 'passed': True})

        m = Bridge(main, symbols, layout, ident, source, target)
        m.call('pb_ui_poll_wrap', 0)
        message = m.SCRATCH + 0x300
        m.write(message + 32, 24)
        m.write(message + 40, 0)
        m.call('pb_system_notify', message)
        before = m.read(m.node + 0x24, 2)
        m.call('pb_native_return', 3, 0x552)
        assert m.selected == (ident & ~15) | 5, 'removed source photo was reselected'
        m.call('nb_end', m.STATE)
        assert m.read(m.node + 0x24, 2) == before, 'old epoch altered replacement node'
        assert not m.get('nb_stage') and not m.get('nb_bit')
        cases.append({'name': ('RAW' if fmt else 'JPG') + ' source-card replacement is not mutated', 'passed': True})
    return cases


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--main', type=Path, required=True)
    ap.add_argument('--elf', type=Path, required=True)
    ap.add_argument('--layout', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    image = args.main.read_bytes()
    with args.elf.open('rb') as f:
        elf = ELFFile(f)
        symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
        for sec in elf.iter_sections():
            if sec['sh_flags'] & 2 and sec['sh_size'] and sec['sh_type'] != 'SHT_NOBITS':
                at = sec['sh_addr'] - 0x40160000
                assert image[at:at + sec['sh_size']] == sec.data(), ('ELF/MAIN mismatch', sec.name)
    result = {'main_sha256': sha(image), 'elf_sha256': sha(args.elf.read_bytes()),
              'layout_sha256': sha(args.layout.read_bytes()),
              'scope': 'Final AArch64 bridge with explicit stock/GUI service stubs; cached pairing is supplied. No dual-card hardware, lookup identity, scheduler or codec acceptance.',
              'tests': run(image, symbols, json.loads(args.layout.read_text()))}
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'passed': len(result['tests']), 'output': str(args.output)}))


if __name__ == '__main__':
    main()
