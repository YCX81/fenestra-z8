#!/usr/bin/env python3
"""仅由用户在 win-lab 执行：在内存中扩展参考驱动的输入序列，不改磁盘驱动。"""
import argparse
import hashlib
import os
from pathlib import Path
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--driver', type=Path, required=True)
p.add_argument('--actions', default='i,ok,left,right,right,left,ok,i,dial+,dial-')
p.add_argument('--prepare-only', action='store_true', help='Only compile the adapted driver; never execute it')
a, rest = p.parse_known_args()
ref = Path(__file__).with_name('run_imenu.reference.py').read_bytes()
src = a.driver.read_bytes()
if hashlib.sha256(src).digest() != hashlib.sha256(ref).digest():
    p.error('Lab driver differs from reviewed run_imenu.reference.py; review before running')
actions = a.actions.split(',')
if any(x not in ('i', 'ok', 'left', 'right', 'up', 'down', 'menu', 'dial+', 'dial-') for x in actions):
    p.error('Unsupported action')
s = src.decode()
assert "probe.MENU_CONTACTS['ok']=(0x2a,0x8000,0)" in s
s = s.replace("probe.MENU_CONTACTS['ok']=(0x2a,0x8000,0)", "probe.MENU_CONTACTS['ok']=(0x4a,0,1)  # real OK: MCU body 0x4A, sw1 bit0")
start = s.index("    elif A.plan=='imenu':\n")
end = s.index('    else:\n', start)
replacement = '''    elif A.plan=='imenu':
        for n, action in enumerate(C64_ACTIONS):
            label = f'c64-{n:02d}-{action}'
            if action.startswith('dial'):
                delta = 1 if action == 'dial+' else -1
                body = bytearray(probe.key_body()); body[0x0b] = delta & 255
                expected = bytearray(probe.expected_keys())
                struct.pack_into('<I', expected, 16, delta & 0xffffffff)
                rows.append(dict(label=label, body=bytes(body), expected=bytes(expected),
                                 gap=400000000, half=False, full=False))
                rows.append(dict(label=label+'-release', body=probe.key_body(),
                                 expected=probe.expected_keys(), gap=4000000000, half=False, full=False))
            else:
                key(label, action, 4)
        shutter('wait-end', gap=3)
'''
s = s[:start] + replacement + s[end:]
old = '0xa1166b000+(b*64+s-1)*0x400'
assert s.count(old) == 1
s = s.replace(old, '0xa1166b080+(b*64+s-1)*0x400')
# Log the actual C10 descriptor, avoiding reliance on a screenshot filename.
needle = "                try:info['slots']=slot_state(g)"
assert s.count(needle) == 1
s = s.replace(needle, "                info['c64_style_7e00']=int.from_bytes(g.read_bytes(0xa116d020c,4),'little')\n" + needle)
code = compile(s, str(a.driver.resolve()), 'exec')
if a.prepare_only:
    print('PASS: driver hash, action grammar, guarded replacements, Python compilation; QEMU NOT RUN')
else:
    if '--plan' not in rest or rest[rest.index('--plan')+1] != 'imenu':
        p.error('Use --plan imenu')
    sys.argv = [str(a.driver.resolve()), *rest]
    sys.path.insert(0, str(a.driver.resolve().parent))
    exec(code, {'__name__': '__main__', '__file__': str(a.driver.resolve()), 'C64_ACTIONS': actions})
