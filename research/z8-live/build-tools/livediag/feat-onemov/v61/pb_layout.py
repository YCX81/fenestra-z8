#!/usr/bin/env python3
"""Generate AArch64 PbRuntime offsets from the exact final header."""
import argparse
import hashlib
import json
import struct
import subprocess
import tempfile
from pathlib import Path

from elftools.elf.elffile import ELFFile

FIELDS = '''magic bytes key media_epoch shutdown key_epoch current_id cached_id
cache_valid lookup_count lookup_error native_path manifest.native_path
manifest.native_size lookup_epoch lookup_us nb_epoch nb_request nb_stage
nb_photo nb_movie nb_starts nb_returns nb_redirects nb_w nb_h nb_bit nb_fail
nb_node nb_from nb_to nb_cleanups nb_refused consumed_rec ui_leaving
render_request preview_fd pair_lock card_epoch[0] card_epoch[1] nb_card_epoch'''.split()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--clang', default='clang-21')
    a = ap.parse_args()
    code = '#include <stddef.h>\n#include "runtime.h"\nconst unsigned layout[]={sizeof(PbRuntime),\n'
    code += ',\n'.join('offsetof(PbRuntime,%s),sizeof(((PbRuntime*)0)->%s)' % (x, x) for x in FIELDS)
    code += '\n};\n'
    with tempfile.TemporaryDirectory(prefix='v61-pb-layout-') as tmp:
        c, obj = Path(tmp) / 'layout.c', Path(tmp) / 'layout.o'
        c.write_text(code)
        subprocess.run([a.clang, '--target=aarch64-none-elf', '-c', '-I' + str(a.source),
                        '-I' + str(a.source / 'core-include'),
                        str(c), '-o', str(obj)], check=True)
        with obj.open('rb') as f:
            data = ELFFile(f).get_section_by_name('.rodata').data()
        values = struct.unpack('<' + 'I' * (len(data) // 4), data)
        assert len(values) == 1 + 2 * len(FIELDS)
    result = {'struct_bytes': values[0], 'offsets': dict(zip(FIELDS, values[1::2])),
              'widths': dict(zip(FIELDS, values[2::2])),
              'runtime_header_sha256': hashlib.sha256((a.source / 'runtime.h').read_bytes()).hexdigest(),
              'layout_source_sha256': hashlib.sha256(code.encode()).hexdigest()}
    a.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'struct_bytes': values[0], 'fields': len(FIELDS), 'output': str(a.output)}))


if __name__ == '__main__':
    main()
