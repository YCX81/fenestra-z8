#!/usr/bin/env python3
"""Describe a built MAIN as declared byte ranges over the c770f340 core baseline,
in the changed-ranges.json form the QEMU capture harness verifies before it runs a
derived image (every differing byte is listed; restoring them gives the baseline).
usage: derive_manifest.py BASELINE.main BUILT.main SYMBOLS.json OUT_DIR
Writes OUT_DIR/<name of BUILT> (a copy) and OUT_DIR/changed-ranges.json."""
import hashlib, json, shutil, sys
from pathlib import Path

base_path, main_path, symbols_path, out = map(Path, sys.argv[1:5])
base, main = base_path.read_bytes(), main_path.read_bytes()
sha = lambda b: hashlib.sha256(b).hexdigest()
assert len(base) == len(main) == 81807709
assert sha(base) == 'c770f34014849b2c42f20b9258a32b8959253cfb296e48f83417572781e00864'
LOAD = 0x40160000
ranges, i, n = [], 0, len(main)
step = 1 << 16
while i < n:
    if base[i:i + step] == main[i:i + step]:
        i += step
        continue
    end = min(i + step, n)
    while i < end:
        if base[i] == main[i]:
            i += 1
            continue
        j = i
        gap = 0
        k = i
        while k < n and gap < 32:      # merge differences closer than 32 bytes
            if base[k] != main[k]:
                j = k + 1
                gap = 0
            else:
                gap += 1
            k += 1
        ranges.append(dict(address=hex(LOAD + i), size=j - i, expected=base[i:j].hex(), replacement=main[i:j].hex(),
                           purpose='v36/v37 layers over the core baseline'))
        i = j
out.mkdir(parents=True, exist_ok=True)
shutil.copy2(main_path, out / main_path.name)
symbols = json.loads(symbols_path.read_text())['symbols']
(out / 'changed-ranges.json').write_text(json.dumps(dict(schema='derived-over-core-baseline/v1', input_sha256=sha(base),
                                                         output_sha256=sha(main), flashable=False, symbols=symbols,
                                                         ranges=ranges)) + '\n')
print(json.dumps(dict(ranges=len(ranges), changed_bytes=sum(r['size'] for r in ranges), output_sha256=sha(main))))
