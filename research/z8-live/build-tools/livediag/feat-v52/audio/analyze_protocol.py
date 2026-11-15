#!/usr/bin/env python3
"""Read-only audit of the private PB_AUDIO_TEST whole-camera lane.

This flag supplies zero source frames. A pass establishes service protocol and
Live recovery only, never speaker DMA consumption or audible hardware output.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('run', type=Path)
ap.add_argument('--report', type=Path, required=True)
a = ap.parse_args()
rows = [json.loads(s) for s in (a.run / 'live-checkpoints.jsonl').read_text().splitlines()]
raw = (a.run / 'lv-observer.dat').read_bytes()
h = struct.unpack_from('<12Q', raw)
assert h[:4] == (0x4c564f4253455256, 1, 72, 65536), h[:4]
events = []
for seq in range(max(0, h[4] - h[3]), h[4]):
    e = struct.unpack_from('<8Q2I', raw, 96 + seq % h[3] * h[2])
    assert e[0] == seq
    if e[9] in (300, 301, 302):
        events.append(dict(seq=e[0], host_ns=e[1], pc=hex(e[2]), x0=hex(e[3]),
                           x1=hex(e[4]), x2=hex(e[5]), x3=hex(e[6]),
                           lr=hex(e[7]), cpu=e[8], kind=e[9]))
callbacks = [e['x0'] for e in events if e['kind'] == 301]
protocol = ['0x43180014', '0x4318001a', '0x43180015', '0x43180016']
compact = []
for s in rows:
    if any(x in s['label'] for x in ('photo-live', 'post-complete', 'pb-', 'final')):
        r = s.get('runtime', {})
        compact.append(dict(label=s['label'], driver=s.get('native_audio_driver'),
                            service=s.get('native_audio_service'), pb=s.get('pb'),
                            live={k: r.get(k) for k in ('requested', 'audio.active',
                                  'audio_z8.phase', 'capture.irq_seq', 'stream.encoded',
                                  'stream.error', 'stream.job')}))
final = next((s for s in rows if s['label'] == 'final'), None)
before = next((s for s in rows if s['label'].endswith('-pb-return-half')), None)
after = next((s for s in rows if s['label'].endswith('-pb-recovered')), final)
pb = final.get('pb', {}) if final else {}
recovered = bool(before and after and all(after['runtime'][k] > before['runtime'][k]
                                        for k in ('capture.irq_seq', 'stream.encoded')))
service_passed = bool(final and callbacks == protocol and pb.get('audio_plays') == 1
              and pb.get('audio_errors') == 0 and pb.get('audio_stage') == 0
              and pb.get('audio_source_frames') == 0 and pb.get('frames', 0) > 0
              and pb.get('playing') == 0 and pb.get('error') == 0
              and pb.get('audio_gate', 2) & 2 == 0
              and final.get('native_audio_driver') == 1
              and final.get('native_audio_service') == 0 and h[8] == 0)
passed = service_passed and recovered
result = json.loads((a.run / 'result.json').read_text()) if (a.run / 'result.json').exists() else {}
out = dict(schema='v52-private-pb-audio-protocol/v1', passed=passed,
           service_protocol_passed=service_passed, run_error=result.get('error'),
           scope='zero-frame service protocol and Live recovery; private PB_AUDIO_TEST',
           acoustic_verified=False, tx_dma_consumption_verified=False,
           final_present=bool(final), callbacks=callbacks, expected_callbacks=protocol,
           observer_register_errors=h[8], observer_overwritten=h[5], events=events,
           checkpoints=compact, live_recovered=recovered,
           observer_sha256=hashlib.sha256(raw).hexdigest())
a.report.write_text(json.dumps(out, indent=2) + '\n')
print(json.dumps({k: out[k] for k in ('passed', 'service_protocol_passed', 'final_present', 'callbacks', 'live_recovered')}))
