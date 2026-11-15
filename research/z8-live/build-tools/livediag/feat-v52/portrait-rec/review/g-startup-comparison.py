#!/usr/bin/env python3
"""Read completed passive run artifacts; never connect to a live emulator."""
from pathlib import Path
import collections
import hashlib
import json
import struct

BASE = Path('/path/to/user/.local/share/nikon-z8-qemu/live-audio/regress/runs')
NAMES = ('v52g-hevc-close', 'v52g-final-rec')
FIELDS = ('requested', 'poll_count', 'stock_handoff_lock', 'capture.irq_seq',
          'capture.irq_timestamp', 'capture.phase', 'capture.reason',
          'capture.copied', 'capture.dropped', 'stream.width', 'stream.height',
          'stream.encoder.hw', 'stream.encoded', 'stream.allocation_requested',
          'transaction_count', 'transaction_error')
KINDS = {5, 6, 8, 9, 11, 12, 13, 14, 200, 201, 204, 205, 206,
         211, 212, 240, 241, 244, 245, 401, 402, 403}

def sha(data):
    return hashlib.sha256(data).hexdigest()

def run(name):
    p = BASE / name
    raw = (p / 'result.json').read_bytes()
    result = json.loads(raw)
    obs = (p / 'lv-observer.dat').read_bytes()
    header = struct.unpack_from('<12Q', obs)
    rec_size, capacity, head = header[2:5]
    assert rec_size == 72 and head <= capacity
    records = []
    counts = collections.Counter()
    last_live = None
    for i in range(head):
        v = struct.unpack_from('<8Q2I', obs, 96 + i * rec_size)
        seq, ns, pc, x0, x1, x2, x3, lr, cpu, kind = v
        counts[kind] += 1
        r = dict(seq=seq, host_ns=ns, pc=hex(pc), args=list(map(hex, (x0,x1,x2,x3))),
                 lr=hex(lr), cpu=cpu, kind=kind)
        if kind == 100:
            last_live = r
        if kind in KINDS or (kind == 202 and x0):
            records.append(r)
    checkpoints = []
    for c in result['live_checkpoints']:
        label = c['label']
        if not (label.startswith('step-047-') or any(label.startswith(f'step-{i:03d}-') for i in range(103, 116))):
            continue
        lv = c.get('lv_diagnostics', {})
        control = lv.get('engine1543-control-state', {})
        checkpoints.append(dict(label=label,
            runtime={k:c.get('runtime', {}).get(k) for k in FIELDS},
            irq_timestamp_us=lv.get('irq_timestamp_us'),
            last_irq_progress_checkpoint=lv.get('last_irq_progress_checkpoint'),
            raw=c.get('raw-pipeline-state'),
            control=[{k:b.get(k) for k in ('base','ready','active','held','starts','stops','holds')}
                     for b in control.get('blocks',[])],
            scanout=c.get('engine1548-scanout-state')))
    manifest=json.loads((p/'manifest.json').read_text())
    command=result['command']
    kernel=command[command.index('-kernel')+1]
    return dict(name=name, path=str(p), result_sha256=sha(raw), manifest=manifest,
        actual_kernel=kernel, actual_kernel_sha256=manifest[kernel],
        actual_machine=command[command.index('-M')+1],
        observer_sha256=sha(obs), stderr_sha256=sha((p/'stderr.log').read_bytes()),
        error=result.get('error'), checks=len(result['checks']),
        firmware_sha256=result.get('firmware_sha256'), qemu_sha256=result.get('qemu_sha256'),
        machine_properties=result.get('machine_properties'),
        runtime_profile=result.get('runtime_profile'),
        cpu_register_redirection=result.get('cpu_register_redirection'), ram_patches=result.get('ram_patches'),
        observer_header=list(header), counts=dict(counts), last_live=last_live,
        records=records, checkpoints=checkpoints,
        final_raw=result.get('raw-pipeline-state'), final_status=result.get('final_status'))

runs = [run(n) for n in NAMES]
print(json.dumps(dict(runs=runs,
    same_machine_properties=runs[0]['machine_properties']==runs[1]['machine_properties'],
    same_full_machine_argument=runs[0]['actual_machine']==runs[1]['actual_machine'],
    same_qemu=runs[0]['qemu_sha256']==runs[1]['qemu_sha256'],
    scope='Passive completed artifacts only; native error-flag producer and missing first restarted raw-frame cause remain unproved.'), indent=2))
