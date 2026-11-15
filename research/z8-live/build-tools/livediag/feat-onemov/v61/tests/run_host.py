#!/usr/bin/env python3
"""Run actual V61 playback control flow against in-memory Nikon API stubs."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess

HERE = Path(__file__).resolve().parent
ap = argparse.ArgumentParser()
ap.add_argument('--source', type=Path, default=HERE.parent / 'work/pb-fix37')
args = ap.parse_args()
source = args.source.resolve()
runtime = source / 'runtime.c'
runtime_bytes = runtime.read_bytes()
text = runtime_bytes.decode()
header_bytes = (source / 'runtime.h').read_bytes()
sync_bytes = (source / 'sync.c').read_bytes()
notify = text[text.index('void pb_system_notify('):text.index('uint32_t pb_key_notify(')]
global_epoch_update = notify.index('atomic_fetch_add_explicit(&r->media_epoch,1,memory_order_acq_rel)')
card_epoch_update = notify.index('atomic_fetch_add_explicit(&r->card_epoch[i],1,memory_order_acq_rel)')
assert global_epoch_update < card_epoch_update, 'Global pairing invalidation must precede per-card epoch publication'
ranges = [
    ('static uint64_t pb_close_gate(', 'void pb_set_key('),
    ('static int lock(', 'static int current('),
    ('static void hide(', 'static int playback_owner('),
    ('static int pair_lock(', 'uint32_t pb_ui_poll_wrap('),
    ('void pb_ui_leave(', 'void pb_ui_state_notify('),
    ('static uint32_t be32(', '/* ---- preview sound'),
    ('static void worker(', 'void pb_boot('),
    ('void pb_system_notify(', 'uint64_t pb_key_decode_wrap('),
    ('static void enqueue(', '/* Versioned read-only diagnostic offsets'),
]
sha = lambda b: hashlib.sha256(b).hexdigest()
parts, provenance = [], []
for start, end in ranges:
    a = text.index(start)
    b = text.index(end, a)
    fragment = text[a:b]
    line = text[:a].count('\n') + 1
    parts.append(f'#line {line} "{runtime}"\n{fragment}')
    provenance.append(dict(start=start, first_line=line, sha256=sha(fragment.encode())))
template = (HERE / 'harness.c.in').read_text()
assert template.count('/* ACTUAL_FIRMWARE_FUNCTIONS */') == 1
generated = HERE / 'harness.generated.c'
generated.write_text(template.replace('/* ACTUAL_FIRMWARE_FUNCTIONS */', '\n'.join(parts)))
exe = HERE / 'host-regression'
cmd = [os.environ.get('CC', 'clang'), '-std=c11', '-O0', '-g', '-Wall', '-Wextra',
       '-Wno-unused-parameter', '-Wno-unused-function',
       '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
       '-I', str(source), '-I', str(source / 'core-include'),
       str(generated), str(source / 'sync.c'), '-o', str(exe)]
subprocess.run(cmd, check=True)
result = subprocess.run([str(exe)], capture_output=True, text=True)
print(result.stdout, end='')
if result.stderr:
    print(result.stderr, end='')
report = dict(status='PASS' if result.returncode == 0 else 'FAIL',
              source=str(runtime), runtime_sha256=sha(runtime_bytes),
              header_sha256=sha(header_bytes),
              sync_sha256=sha(sync_bytes),
              source_stable_during_run=(runtime.read_bytes() == runtime_bytes and
                                       (source / 'runtime.h').read_bytes() == header_bytes and
                                       (source / 'sync.c').read_bytes() == sync_bytes),
              extracted_ranges=provenance, compile_command=cmd,
              passed_cases=sum(line.startswith('PASS ') for line in result.stdout.splitlines()),
              static_checks={'media_notify_global_epoch_update_precedes_card_epoch_update': True},
              returncode=result.returncode, stdout=result.stdout, stderr=result.stderr,
              hardware_access=False,
              limitations='Host Nikon API/file/DCF memory/scheduler/GUI stubs. Actual extracted functions are not rewritten. Does not establish native ref layout, card-selection behavior, decoder success or hardware timing.')
(HERE / 'results.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
raise SystemExit(result.returncode)
