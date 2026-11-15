"""Read actual parser object state in isolated QEMU; no native-UI claim."""
from pathlib import Path
import io
import json
import struct
import subprocess
import tempfile
from elftools.elf.elffile import ELFFile

HERE = Path(__file__).resolve().parent
LIVE = HERE.parents[2]
WRITER = HERE.parents[1] / 'writer'
source = (LIVE / 'feat-mov/evidence/ncdt-replay.S').read_text()
anchor = '    cbnz w25, next_movie\n    ldr x0, =0x4e002000\n'
assert source.count(anchor) == 1
source = source.replace(anchor, '''    cbnz w25, next_movie
    ldr x20, =0x4e002000
    ldr x20, [x20]
    mov w21, #(0xb00 / 4)
dump_object:
    ldr w0, [x20], #4
    bl print_word
    subs w21, w21, #1
    b.ne dump_object
    b next_movie
    ldr x0, =0x4e002000
''')
paths = [WRITER / 'parser/HOST_4K.MOV', WRITER / 'parser/LIV_9013-META.MOV',
         WRITER / 'parser/native-metadata-last.MOV']
packet = bytearray(4)
for path in paths:
    blob = path.read_bytes()
    packet += struct.pack('<I', len(blob)) + blob + bytes(-len(blob) % 4)
packet += bytes(4)
with tempfile.TemporaryDirectory(prefix='parser-state-', dir=HERE) as td:
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
objects = {}
for path in paths:
    assert next(words) == 0
    objects[path.name] = b''.join(struct.pack('<I', next(words)) for _ in range(0xb00 // 4))
assert next(words, None) is None
results = {'scope': 'Actual C3.11 parser object bytes, synthetic file callbacks; not UI playback',
           'current_track_kind_at_a88': {name: struct.unpack_from('<I', blob, 0xa88)[0] for name, blob in objects.items()},
           'differences': {}}
first = objects[paths[0].name]
for name, blob in list(objects.items())[1:]:
    results['differences'][name] = [{'offset': hex(off), 'host': hex(struct.unpack_from('<I', first, off)[0]),
                                  'other': hex(struct.unpack_from('<I', blob, off)[0])}
                                 for off in range(0, len(blob), 4) if first[off:off+4] != blob[off:off+4]]
for name, blob in objects.items():
    (HERE / 'container-variants' / (name + '.parser.bin')).write_bytes(blob)
(HERE / 'container-variants/parser-state.json').write_text(json.dumps(results, indent=2) + '\n')
print('current kinds:', results['current_track_kind_at_a88'])
print('difference counts:', {name: len(rows) for name, rows in results['differences'].items()})
