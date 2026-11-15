#!/usr/bin/env python3
"""Execute original/patched MAIN's actual Fenestra text resolver in Unicorn.

Only the current State getter, legacy UI getter and owner-text provider are
explicit stubs. The native UI text routine, dispatcher and NFP table lookup
execute their delivered AArch64 bytes. This is not a font-rendering test.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as A
from patch_menu_fallback import (BASE, NFP_FIRST, NFP_LAST, LANGUAGES, NATIVE_COUNT,
                                TABLE_DIRECTORY, POOL_DIRECTORY, SITE, patch,
                                native_text, replacement, sha, validate)

ENTRY = 0x401961F8             # original prologue trampoline -> core dispatcher +4
STATE_GET = 0x401CC314
UI_GET = 0x401CD014
OWNER_TEXT = 0x401C090C


class Machine:
    STACK = 0xA04000000
    STATE = 0xA04100000
    UI = 0xA04200000
    OUT = 0xA04300000
    STOP = 0x40100000

    def __init__(self, image):
        self.u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.u.mem_map(BASE, (len(image) + 4095) & ~4095)
        self.u.mem_write(BASE, image)
        for address, size in [(self.STACK, 0x10000), (self.STATE, 0x10000),
                              (self.UI, 0x10000), (self.OUT, 0x1000),
                              (self.STOP, 0x1000), (0xA0281C000, 0x1000),
                              (0xA02F2A000, 0x1000)]:
            self.u.mem_map(address, size)
        self.write(0xA0281CD5C, NATIVE_COUNT, 4)
        self.write(0xA02F2AC38, POOL_DIRECTORY, 8)
        self.write(0xA02F2AC48, TABLE_DIRECTORY, 8)
        self.dynamic = 'Owned UI: 测试'.encode('utf-16le')
        self.u.mem_write(self.OUT + 0x100, self.dynamic + b'\0\0')
        self.active, self.mode = True, 'absent'
        self.calls = Counter()
        self.u.hook_add(UC_HOOK_CODE, self.hook)

    def write(self, address, value, width):
        self.u.mem_write(address, value.to_bytes(width, 'little'))

    def ret(self, value):
        self.u.reg_write(A.UC_ARM64_REG_X0, value)
        self.u.reg_write(A.UC_ARM64_REG_PC, self.u.reg_read(A.UC_ARM64_REG_X30))

    def hook(self, uc, address, size, _):
        if address == self.STOP:
            uc.emu_stop()
        elif address == STATE_GET:
            self.calls['state'] += 1
            self.ret(self.STATE if self.active else 0)
        elif address == UI_GET:
            self.calls['ui_get'] += 1
            self.ret(0 if self.mode == 'absent' else self.UI)
        elif address == OWNER_TEXT:
            self.calls['owner_text'] += 1
            if self.mode in ('owned', 'owned_empty'):
                pointer = self.OUT + 0x100
                count = len(self.dynamic) if self.mode == 'owned' else 0
                self.write(uc.reg_read(A.UC_ARM64_REG_X2), pointer, 8)
                self.write(uc.reg_read(A.UC_ARM64_REG_X3), count, 4)
                self.ret(1)
            else:
                self.ret(0)

    def call(self, language, ident, mode='absent', active=True):
        self.active, self.mode = active, mode
        self.write(self.UI + 84, 5000 if mode == 'present_miss' else NFP_FIRST, 4)
        for register in range(31):
            self.u.reg_write(getattr(A, 'UC_ARM64_REG_X' + str(register)), 0)
        self.write(self.OUT, 0, 8)
        self.write(self.OUT + 8, 0xFFFFFFFF, 4)
        for register, value in enumerate([language, ident, self.OUT, self.OUT + 8]):
            self.u.reg_write(getattr(A, 'UC_ARM64_REG_X' + str(register)), value)
        self.u.reg_write(A.UC_ARM64_REG_SP, self.STACK + 0xFFF0)
        self.u.reg_write(A.UC_ARM64_REG_X30, self.STOP)
        self.u.emu_start(ENTRY, self.STOP + 4, count=10000)
        assert self.u.reg_read(A.UC_ARM64_REG_PC) == self.STOP, 'instruction budget exhausted'
        pointer = int.from_bytes(self.u.mem_read(self.OUT, 8), 'little')
        count = int.from_bytes(self.u.mem_read(self.OUT + 8, 4), 'little')
        assert count <= 4096 and count % 2 == 0, (language, ident, mode, count)
        data = bytes(self.u.mem_read(pointer, count)) if count else b''
        return pointer, count, data


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--original', type=Path, required=True)
    ap.add_argument('--candidate', type=Path, required=True)
    ap.add_argument('--expected-original-sha256', required=True)
    ap.add_argument('--expected-candidate-sha256')
    ap.add_argument('--allow-build-id-update', action='store_true',
                    help='allow only the separately generated 4-byte build ID at 0x401fe060; requires exact candidate SHA')
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    original, candidate = args.original.read_bytes(), args.candidate.read_bytes()
    expected, tables = patch(original, args.expected_original_sha256)
    if args.expected_candidate_sha256:
        assert sha(candidate) == args.expected_candidate_sha256, 'candidate SHA mismatch'
    build_id = None
    if args.allow_build_id_update:
        assert args.expected_candidate_sha256, 'final build ID allowance requires exact candidate SHA'
        offset = 0x401FE060 - BASE
        assert len(candidate) == len(expected)
        assert candidate[:offset] == expected[:offset] and candidate[offset + 4:] == expected[offset + 4:], \
            'candidate differs outside the guarded menu patch and explicit build ID'
        build_id = {'address': '0x401fe060', 'before': expected[offset:offset + 4].hex(),
                    'after': candidate[offset:offset + 4].hex(),
                    'note': 'Parent-generated build ID; exact final SHA bound; all other bytes match the 12-byte patch.'}
    else:
        assert candidate == expected, 'candidate is not exactly the guarded menu patch'
    assert original[ENTRY - BASE:ENTRY - BASE + 8] == bytes.fromhex('fd7bbca97ddc0014')
    before, after = Machine(original), Machine(candidate)
    groups = Counter()
    # Expose the real failure, not merely compare two host implementations.
    for language in range(LANGUAGES):
        for ident in range(NFP_FIRST, NFP_LAST + 1):
            wanted = native_text(original, language, ident)
            assert wanted
            assert before.call(language, ident)[1:] == (0, b'')
            groups['baseline_NFP_swallowed_to_empty'] += 1
            for mode in ('absent', 'present_miss'):
                got = after.call(language, ident, mode)
                assert got[1:] == (len(wanted), wanted), (language, ident, mode)
                groups['NFP_restored_through_actual_tables'] += 1
            for mode in ('owned', 'owned_empty'):
                a, b = before.call(language, ident, mode), after.call(language, ident, mode)
                assert a == b
                assert b[2] == (after.dynamic if mode == 'owned' else b'')
                groups['valid_dynamic_UI_hit_preserved_including_empty'] += 1
            assert before.call(language, ident, active=False) == after.call(language, ident, active=False)
            groups['pre_activation_native_fallback_unchanged'] += 1
        # count==3225 is exclusive. Native ID 0 is nonempty: an unconditional
        # fallback here would leak unrelated original strings into stale UI.
        assert native_text(original, language, 0)
        for ident in range(NFP_LAST + 1, 3254):
            for mode in ('absent', 'present_miss'):
                assert before.call(language, ident, mode)[1:] == (0, b'')
                assert after.call(language, ident, mode)[1:] == (0, b'')
                groups['higher_UI_miss_stays_empty'] += 1
        for ident in [0, 1, 889, 3137, 3153, 3154, 3155, 3168, 3169]:
            assert before.call(language, ident) == after.call(language, ident)
            groups['stock_and_fixed_Fenestra_labels_unchanged'] += 1
    # The patcher's boundaries and input binding must fail closed.
    for image, digest in [(original, '0' * 64),
                          (original[:SITE - BASE] + bytes([original[SITE - BASE] ^ 1]) + original[SITE - BASE + 1:], None)]:
        digest = digest or sha(image)
        try:
            patch(image, digest)
        except ValueError:
            groups['wrong_SHA_or_instruction_guard_rejected'] += 1
        else:
            raise AssertionError('guard accepted wrong input')
    labels = {}
    for language, name in [(8, 'zh-CN'), (1, 'en')]:
        labels[name] = [{'id': ident, 'before': before.call(language, ident)[2].decode('utf-16le'),
                         'after': after.call(language, ident)[2].decode('utf-16le')}
                        for ident in range(3217, 3222)]
    record = {'passed': True, 'input_main_sha256': sha(original), 'candidate_main_sha256': sha(candidate),
              'test_script_sha256': sha(Path(__file__).read_bytes()),
              'patch_script_sha256': sha(Path(__file__).with_name('patch_menu_fallback.py').read_bytes()),
              'groups': dict(groups), 'checks': sum(groups.values()), 'table_guards': tables,
              'changed_instruction_bytes': 12, 'candidate_changed_byte_count': sum(a != b for a, b in zip(original, candidate)),
              'separate_build_id_update': build_id,
              'resolved_log_labels_UI_absent': labels,
              'executed_entries': {'original_core_trampoline': hex(ENTRY), 'actual_fx_native_ui_text': '0x401bc67c'},
              'stubs': {'state_getter': hex(STATE_GET), 'legacy_UI_getter': hex(UI_GET), 'UI_owner_text_provider': hex(OWNER_TEXT)},
              'scope': 'Actual delivered AArch64 resolver, native UI text routine and all 36 NFP language tables. Explicit State/UI-owner stubs; not a QEMU raster or physical-camera proof.',
              'remaining_namespace_boundary': 'Valid Fenestra UI hits intentionally win even for overlapping NFP IDs. IDs 3154..3179 are fixed Fenestra strings and remain unchanged; module IDs >=3254 are outside this NFP table.'}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(record, ensure_ascii=False))


if __name__ == '__main__':
    main()
