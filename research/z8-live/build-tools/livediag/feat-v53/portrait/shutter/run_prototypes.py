#!/usr/bin/env python3
"""Rebuild private host prototypes with sanitizers; never changes firmware."""
from pathlib import Path
import hashlib, json, subprocess
P = Path(__file__).resolve().parent
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
report = {'production_ready': False, 'native_sound_adapter': False, 'tests': []}
for name, sources in (
    ('pause', ['test_pause.c', 'work/audio/live_audio.c', 'work/audio/live_audio_z8.c']),
    ('queue', ['test_queue.c', 'queue_prototype.c']),
):
    cmd = ['clang', '-std=c11', '-g', '-O1', '-fsanitize=address,undefined',
           '-fno-omit-frame-pointer', '-pthread', '-Iwork', *sources, '-o', 'test_' + name]
    built = subprocess.run(cmd, cwd=P, capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    run = subprocess.run([str(P / ('test_' + name))], cwd=P, capture_output=True, text=True)
    (P / (name + '-tests.txt')).write_text(run.stdout + run.stderr)
    assert run.returncode == 0 and not run.stderr, (run.returncode, run.stderr)
    report['tests'].append({'name': name, 'passed': True, 'command': cmd,
        'source_sha256': {s: sha(P / s) for s in sources},
        'stdout': run.stdout, 'stderr': run.stderr})
report['passed'] = True
report['boundaries'] = [
    'Native queue, ADC/DAC and callbacks are host shims in these two tests.',
    'Separate actual-ARM tests cover native configuration/admission, not physical cadence.',
    'No native SFX hook, cancellation adapter or SoundTask integration is implemented.',
    'Late callback after a new GO still needs native ordering/generation evidence.',
]
(P / 'prototype-tests.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS private pause/queue actual source ASan+UBSan; production_ready=false')
