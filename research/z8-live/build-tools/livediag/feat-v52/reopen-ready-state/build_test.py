#!/usr/bin/env python3
from pathlib import Path
import subprocess, json
D = Path(__file__).resolve().parent
S = D / 'work/src-fix37'
flags = ['--target=aarch64-none-elf', '-Os', '-ffreestanding', '-fno-builtin',
         '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
         '-ffunction-sections', '-fdata-sections', '-Wall', '-Wextra', '-Werror']
(D / 'entry.c').write_text('#include "' + str(S / 'codec/live_w5enc.c') + '"\n'
    'int test_ready(struct live_w5 *w) { return w5_params_ready(w); }\n'
    'int test_open(struct live_w5 *w) { return open_stream(w); }\n')
out = {}
for codec in (0, 1):
    obj = D / f'codec{codec}.o'
    subprocess.run(['clang-21', *flags, f'-DLIVE_W5_HEVC={codec}', '-I', str(S),
        '-I', str(S / 'ui/core/include'), '-I', str(S / 'codec/freestanding'),
        '-c', str(D / 'entry.c'), '-o', str(obj)], check=True)
    defs = {'memcpy':0x401c6cb4, 'memset':0x401c6d00,
            'live_hevc_param':0x40201000, 'live_h264_idr_init':0x40201004}
    for name in ('ready', 'open'):
        stem = D / f'{name}-codec{codec}'
        subprocess.run(['ld.lld-21', '--gc-sections', '-e', 'test_' + name,
            '-Ttext=0x40100000', *[f'--defsym={k}={v:#x}' for k,v in defs.items()],
            '-o', str(stem) + '.elf', str(obj)], check=True)
        subprocess.run(['llvm-objcopy-21', '-O', 'binary', str(stem)+'.elf',str(stem)+'.bin'],check=True)
        (D / f'{name}-codec{codec}.nm').write_text(subprocess.check_output(
            ['llvm-nm-21', '-n', str(stem)+'.elf'], text=True))
    text = subprocess.check_output(['llvm-size-21','-A',str(obj)],text=True)
    out[str(codec)] = {l.split()[0]:int(l.split()[1]) for l in text.splitlines() if l.startswith('.')}
(D / 'build-size.json').write_text(json.dumps(out,indent=2)+'\n')
print('Built both codec variants with actual source')
