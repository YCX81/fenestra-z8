"""Execute NCDT query0 and stock file-info construction after real NCDT parse."""
from pathlib import Path
import io
import json
import os
import struct
import subprocess
import sys
import tempfile
from elftools.elf.elffile import ELFFile

HERE = Path(__file__).resolve().parent
LIVE = HERE.parents[2]
WRITER = HERE.parents[1] / 'writer'
sys.path.insert(0, str(LIVE / 'feat-mov'))
import mov_fix as m

source = (LIVE / 'feat-mov/evidence/ncdt-replay.S').read_text()
anchor = '    mov x25, #0\nnext_image:\n'
assert source.count(anchor) == 1
source = source.replace(anchor, '''    mov x0, #0
    mov x1, #0
    ldr x2, =0x4e003000
    ldr x16, =0x406ce354
    blr x16
    ldr x20, =0x4e003000
    mov w21, #18
dump_query:
    ldr w0, [x20], #4
    bl print_word
    subs w21, w21, #1
    b.ne dump_query
    ldr x0, =0x4e003100
    ldr x16, =0x406bfed8
    blr x16
    ldr x20, =0x4e003100
    mov w21, #7
dump_file_info:
    ldr w0, [x20], #4
    bl print_word
    subs w21, w21, #1
    b.ne dump_file_info
    b next_case
    mov x25, #0
next_image:
''')
names = ['HOST_4K.MOV', 'LIV_9013-META.MOV', 'native-fields.MOV', 'native-jpegs.MOV',
         'native-donor-ncdt.MOV', 'native-min-fields.MOV', 'native-core-timing.MOV',
         'native-core-format.MOV', 'native-core-all.MOV']
cases = [(name, None) for name in names]
cases += [('native-min-fields.MOV', size) for size in [(1024, 682), (1280, 852), (1920, 1080)]]
cases += [('HOST_4K.MOV', None)]
if len(sys.argv) > 1:
    cases = [(name, None) for name in sys.argv[1:]]
    cases += [cases[0]]
names = [name if size is None else f'{name}:{size[0]}x{size[1]}' for name, size in cases]

def geometry(udta, width, height):
    outer = m.parse(udta)[0]
    children = m.parse(outer.data)
    ncdt = m.direct_child(children, b'NCDT')
    parts = m.parse(ncdt.data)
    nctg = m.direct_child(parts, b'NCTG')
    data = bytearray(nctg.data)
    pos = 0
    seen = set()
    widths = (0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8)
    while pos < len(data):
        tag, kind, count = struct.unpack_from('>IHH', data, pos)
        if tag in (0x22, 0x23):
            assert (kind, count) == (3, 1)
            struct.pack_into('>H', data, pos + 8, width if tag == 0x22 else height)
            seen.add(tag)
        pos += 8 + widths[kind] * count
    assert seen == {0x22, 0x23} and pos == len(data)
    nctg.data = bytes(data)
    ncdt.data = b''.join(x.encode() for x in parts)
    outer.data = b''.join(x.encode() for x in children)
    return outer.encode()

packet = bytearray()
for name, size in cases:
    roots = m.parse((WRITER / 'parser' / name).read_bytes())
    udta = m.direct_child(m.direct_child(roots, b'moov').children, b'udta').encode()
    if size is not None:
        udta = geometry(udta, *size)
    packet += struct.pack('<I', len(udta)) + udta + bytes(-len(udta) % 4)
packet += bytes(8)
with tempfile.TemporaryDirectory(prefix='file-info-', dir=HERE) as td:
    tmp = Path(td)
    subprocess.run(['clang', '--target=aarch64-none-elf', '-x', 'assembler', '-c',
                    '-o', str(tmp / 'probe.o'), '-'], input=source.encode(), check=True)
    elf = ELFFile(io.BytesIO((tmp / 'probe.o').read_bytes()))
    assert elf.get_section_by_name('.rela.text') is None
    (tmp / 'probe.bin').write_bytes(elf.get_section_by_name('.text').data())
    (tmp / 'inputs.bin').write_bytes(packet)
    command = ['qemu-system-aarch64', '-M', 'virt', '-cpu', 'cortex-a57', '-accel', 'tcg',
               '-m', '1280M', '-display', 'none', '-nodefaults', '-nic', 'none',
               '-semihosting-config', 'enable=on,target=native',
               '-device', f'loader,file={WRITER / "parser/bg34.main"},addr=0x40160000,force-raw=on',
               '-device', f'loader,file={tmp / "inputs.bin"},addr=0x48000000,force-raw=on',
               '-device', f'loader,file={tmp / "probe.bin"},addr=0x47000000,force-raw=on,cpu-num=0']
    output = subprocess.check_output(command, stderr=subprocess.STDOUT, timeout=20).decode()
words = iter(int(line, 16) for line in output.splitlines())
results = []
for name in names:
    outer, status = next(words), next(words)
    query = b''.join(struct.pack('<I', next(words)) for _ in range(18))
    file_info = b''.join(struct.pack('<I', next(words)) for _ in range(7))
    results.append({'file': name, 'outer': outer, 'identity_status': status,
                    'query_hex': query.hex(), 'file_info_hex': file_info.hex(),
                    'query_u32': {hex(off): struct.unpack_from('<I', query, off)[0] for off in range(0, 72, 4)},
                    'file_info_u32': {hex(off): struct.unpack_from('<I', file_info, off)[0] for off in range(0, 28, 4)}})
assert next(words, None) is None
assert results[0] == results[-1], 'parser fields leaked across open/reset'
out = HERE / ('container-variants/file-info-candidate.json' if len(sys.argv) > 1 else 'container-variants/file-info.json')
out = Path(os.environ.get('FILE_INFO_REPORT', str(out)))
out.write_text(json.dumps({'scope': 'Actual C3.11 NCDT query0/file-info constructor, no native UI', 'results': results}, indent=2) + '\n')
for result in results:
    q = result['query_u32']
    print(result['file'], 'outer/status', (result['outer'], result['identity_status']),
          'duration/fps/flags/audio', [q[hex(i)] for i in (0x18,0x1c,0x20,0x28,0x2c,0x30,0x34,0x38)],
          'file_info', result['file_info_hex'])
