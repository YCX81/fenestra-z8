"""Execute original JPEG header interrupt with RAM-backed hardware registers."""
from pathlib import Path
import io
import json
import subprocess
import tempfile
from elftools.elf.elffile import ELFFile

HERE = Path(__file__).resolve().parent
LIVE = HERE.parents[2]
original = (LIVE / 'feat-mov/evidence/ncdt-replay.S').read_text()
prefix = original.split("    // Initialize the guest's table")[0]
tail = original[original.index('finish:\n'):]
# Four pairs distinguish an unsupported format from a request/header mismatch.
asm = prefix + '''
    mov w24, #0
format_case:
    ldr x0, =0x4e004000
    mov w1, #0
    mov w2, #0x4000
    ldr x16, =0x4127ab40
    blr x16
    ldr x19, =0x4e004000
    ldr x20, =0x4e005000
    ldr x21, =0x4e006000
    ldr x22, =0x4e007000
    mov w8, #0x189
    str w8, [x19]
    str x20, [x19, #8]
    str x21, [x19, #0x18]
    str x22, [x21]
    lsr w8, w24, #1
    add w8, w8, #1
    str w8, [x22]
    and w8, w24, #1
    add w8, w8, #0x39
    str w8, [x20]
    ldr w8, =0x0a001000
    str w8, [x20, #0x480]
    mov w8, #-1
    str w8, [x20, #0x484]
    mov w8, #160
    str w8, [x20, #0x1c]
    strh w8, [x22, #8]
    mov w8, #120
    str w8, [x20, #0x18]
    strh w8, [x22, #0xa]
    mov x0, x19
    ldr x16, =0x40c0c598
    blr x16
    mov w0, w24
    bl print_word
    ldr w0, [x21, #0x50]
    bl print_word
    ldr w0, [x22]
    bl print_word
    ldr w0, [x20, #0x400]
    bl print_word
    add w24, w24, #1
    cmp w24, #4
    b.ne format_case
    b finish
''' + tail
with tempfile.TemporaryDirectory(prefix='jpeg-gate-', dir=HERE) as td:
    tmp = Path(td)
    subprocess.run(['clang', '--target=aarch64-none-elf', '-x', 'assembler', '-c',
                    '-o', str(tmp / 'probe.o'), '-'], input=asm.encode(), check=True)
    elf = ELFFile(io.BytesIO((tmp / 'probe.o').read_bytes()))
    assert elf.get_section_by_name('.rela.text') is None
    (tmp / 'probe.bin').write_bytes(elf.get_section_by_name('.text').data())
    command = ['qemu-system-aarch64', '-M', 'virt', '-cpu', 'cortex-a57', '-accel', 'tcg',
               '-m', '1280M', '-display', 'none', '-nodefaults', '-nic', 'none',
               '-semihosting-config', 'enable=on,target=native',
               '-device', f'loader,file={LIVE / "feat-mov/bg34.main"},addr=0x40160000,force-raw=on',
               '-device', f'loader,file={tmp / "probe.bin"},addr=0x47000000,force-raw=on,cpu-num=0']
    output = subprocess.check_output(command, stderr=subprocess.STDOUT, timeout=20).decode()
words = iter(int(line, 16) for line in output.splitlines())
rows = []
for case in range(4):
    assert next(words) == case
    flags, requested_after, go = next(words), next(words), next(words)
    requested, actual = 1 + (case >> 1), 1 + (case & 1)
    assert flags == (0x2000 if requested == actual else 0x2800)
    assert bool(go & 0x4000) == (requested == actual)
    rows.append(dict(requested_format=requested, header_format=actual,
                     event_flags=hex(flags), request_format_after=requested_after,
                     image_start=bool(go & 0x4000)))
assert next(words, None) is None
result = dict(scope='Original firmware interrupt, synthetic header registers; no native UI',
              function='0x40c0c598', compare='0x40c0c6e4', mismatch='0x40c0c75c', results=rows)
(HERE / 'container-variants/jpeg-format-gate.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
