#!/usr/bin/env python3
"""Execute context gate, native menu-stack getter, and text resolver in ARM64.

No current-page stub: the native getter and stack-top routine execute actual
MAIN bytes against fixtures. State/UI/owner services retain the explicit
stubs of test_menu_arm. This does not render menu pixels.
"""
import argparse
from collections import Counter
import json
from pathlib import Path

from unicorn import arm64_const as A
from test_menu_arm import Machine
from patch_menu_context import patch, HOOK, HELPER, HELPER_BYTES, PRIMARY, LIMIT
from patch_menu_fallback import BASE, LANGUAGES, NFP_FIRST, NFP_LAST, native_text, sha, SITE


class ContextMachine(Machine):
    ARENA = 0xA04400000

    def __init__(self, image):
        self.page_calls = 0
        super().__init__(image)
        self.u.mem_map(0xA032B5000, 0x1000)
        self.u.mem_map(self.ARENA, 0x1000)
        self.page((2, 0xB5))

    def hook(self, uc, address, size, arg):
        if address == 0x403DBDB4:
            self.page_calls += 1
        super().hook(uc, address, size, arg)

    def page(self, scene):
        self.write(0xA032B5160, 0 if scene == 'null_stack' else self.ARENA, 8)
        self.write(self.ARENA, 8, 4)
        self.write(self.ARENA + 8, 0 if scene == 'empty_stack' else 1, 4)
        self.write(self.ARENA + 16, self.ARENA + 0x80, 8)
        self.write(self.ARENA + 0x80, 0 if scene == 'null_page' else self.ARENA + 0x100, 8)
        if isinstance(scene, tuple):
            group, page = scene
            self.write(self.ARENA + 0x100, group | (page << 16), 4)

    def call(self, *args, **kwargs):
        result = super().call(*args, **kwargs)
        assert self.u.reg_read(A.UC_ARM64_REG_SP) == self.STACK + 0xFFF0, 'stack not restored'
        for reg in range(19, 30):
            assert self.u.reg_read(getattr(A, 'UC_ARM64_REG_X' + str(reg))) == 0, f'x{reg} not restored'
        return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--original', type=Path, required=True)
    ap.add_argument('--candidate', type=Path, required=True)
    ap.add_argument('--build-manifest', type=Path, required=True)
    ap.add_argument('--expected-original-sha256', required=True)
    ap.add_argument('--expected-candidate-sha256')
    ap.add_argument('--allow-build-id-update', action='store_true')
    ap.add_argument('--output', type=Path, required=True)
    a = ap.parse_args()
    original, candidate = a.original.read_bytes(), a.candidate.read_bytes()
    build = json.loads(a.build_manifest.read_bytes())
    expected, guard = patch(original, a.expected_original_sha256, build)
    if a.expected_candidate_sha256:
        assert sha(candidate) == a.expected_candidate_sha256, 'candidate SHA mismatch'
    build_id = None
    if a.allow_build_id_update:
        assert a.expected_candidate_sha256, 'build ID allowance requires exact final SHA'
        offset = 0x401FE060 - BASE
        assert len(expected) == len(candidate)
        assert expected[:offset] == candidate[:offset] and expected[offset + 4:] == candidate[offset + 4:]
        build_id = {'address': '0x401fe060', 'before': expected[offset:offset + 4].hex(),
                    'after': candidate[offset:offset + 4].hex()}
    else:
        assert candidate == expected, 'not exact context-only patch'
    assert candidate[SITE - BASE:SITE - BASE + 12] == original[SITE - BASE:SITE - BASE + 12], 'old miss path changed'
    before, after = ContextMachine(original), ContextMachine(candidate)
    groups = Counter()
    modes = ('absent', 'present_miss', 'owned', 'owned_empty', 'native_empty')
    native_scenes = [(2, 0xB5), (2, 1), (2, 0x6FFF), (2, 0x9500),
                     (1, 0x704A), (3, 0xB5), 'null_stack', 'empty_stack', 'null_page']
    extension_scenes = [(2, 0x7000), (2, 0x704A), (2, 0x7100), (2, 0x94FF)]
    for language in range(LANGUAGES):
        for ident in range(NFP_FIRST, NFP_LAST + 1):
            wanted = native_text(original, language, ident)
            for scene in native_scenes:
                after.page(scene)
                for mode in modes:
                    got = after.call(language, ident, mode)
                    assert got[1:] == (len(wanted), wanted), (language, ident, scene, mode, got)
                    groups['native_or_absent_scene_restores_NFP_despite_resident_UI'] += 1
            for scene in extension_scenes:
                after.page(scene)
                for mode in modes:
                    old, new = before.call(language, ident, mode), after.call(language, ident, mode)
                    assert old == new, (language, ident, scene, mode)
                    groups['actual_extension_preserves_dynamic_hit_empty_and_miss'] += 1
            for scene in [(2, 0xB5), (2, 0x704A)]:
                after.page(scene)
                assert after.call(language, ident, active=False) == before.call(language, ident, active=False)
                groups['pre_activation_native_lookup_unchanged'] += 1
        for ident in list(range(3225, 3254)) + [0, 1, 889, 3137, 3153, 3154, 3155, 3168, 3169, 3179]:
            for scene in [(2, 0xB5), (2, 0x704A)]:
                after.page(scene)
                calls = after.page_calls
                for mode in ('absent', 'present_miss'):
                    assert before.call(language, ident, mode) == after.call(language, ident, mode)
                    groups['outside_conflict_range_unchanged'] += 1
                assert after.page_calls == calls, 'current page called for non-conflicting ID'
    # Reproduce the user's blank Log entries with actual native-UI local text
    # fallback, without inventing an owner response for this case.
    resident_repro = []
    after.page((2, 0xB5))
    for ident in range(3217, 3222):
        old = before.call(8, ident, 'native_empty')[2].decode('utf-16le')
        new = after.call(8, ident, 'native_empty')[2].decode('utf-16le')
        assert old == ' ' and new == native_text(original, 8, ident).decode('utf-16le')
        resident_repro.append({'id': ident, 'before': old, 'after': new})
        groups['resident_UI_blank_Log_regression_reproduced_and_fixed'] += 1
    bad_build = dict(build, live_payload_bytes=HELPER - PRIMARY + 4, live_free=LIMIT - HELPER - 4)
    corrupted = bytearray(original); corrupted[HELPER - BASE] = 1
    for image, digest, manifest in [(original, '0' * 64, build), (original, sha(original), bad_build),
                                   (bytes(corrupted), sha(corrupted), build)]:
        try:
            patch(image, digest, manifest)
        except ValueError:
            groups['wrong_SHA_or_overlap_or_nonzero_reservation_rejected'] += 1
        else:
            raise AssertionError('guard accepted unsafe input')
    labels = {}
    for language, name in [(8, 'zh-CN'), (1, 'en')]:
        labels[name] = [{'id': ident, 'text': after.call(language, ident, 'native_empty')[2].decode('utf-16le')}
                        for ident in range(3217, 3222)]
    record = {'passed': True, 'checks': sum(groups.values()), 'groups': dict(groups),
              'input_main_sha256': sha(original), 'candidate_main_sha256': sha(candidate),
              'test_script_sha256': sha(Path(__file__).read_bytes()),
              'patch_script_sha256': sha(Path(__file__).with_name('patch_menu_context.py').read_bytes()),
              'patch_guard': guard, 'separate_build_id_update': build_id,
              'resolved_log_labels_resident_UI_on_native_page': labels,
              'resident_UI_regression': resident_repro,
              'actual_ARM_entries': ['0x401961f8', '0x401fcf40', '0x403dbdb4', '0x403e9a90', '0x401bc67c'],
              'native_current_page_executions': after.page_calls,
              'preserved_original_UI_miss_branch': True,
              'scope': 'Actual composed ARM64 context wrapper, native stack getter/top, dispatcher and native UI resolver; State/UI/owner service stubs. No menu pixel or physical-camera proof.'}
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(record, ensure_ascii=False))


if __name__ == '__main__':
    main()
