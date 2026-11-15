"""Require an actual native HEVC close, resource disposal and second successful open."""
from pathlib import Path
import argparse, hashlib, json, struct

p = argparse.ArgumentParser()
p.add_argument('run', type=Path)
p.add_argument('core_report', type=Path)
p.add_argument('production_sources', type=Path)
p.add_argument('diagnostic_sources', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
d = json.loads((a.run/'result.json').read_text())
core = json.loads(a.core_report.read_text())
assert core['passed'] and d.get('error') is None and d['owned_qemu_stopped']
assert core['main_sha256'] == d['live_a']['executed_main_sha256']
before, after = d['pb_recovery_before'], d['pb_recovery_after']
assert before['encoded'] > 0 and after['encoded'] > before['encoded']
closed = [x for x in d['live_checkpoints'] if x['label'].endswith('-pb-view')
          and x.get('w5_close_diag', {}).get('hw') == 0
          and x.get('w5_vdi') == '0x0' and x.get('w5_worker') == '0x0'
          and x.get('w5_client_map') == '0xff']
assert closed, 'No fully closed/disposed checkpoint after real Live encoding'
raw = (a.run/'lv-observer.dat').read_bytes()
h = struct.unpack_from('<12Q', raw)
assert h[0] == 0x4c564f4253455256 and h[2] == 72 and h[5] == h[8] == 0
events = []
for i in range(h[4]):
    r = struct.unpack_from('<8Q2I', raw, 96 + (i % h[3]) * 72)
    if r[9] in (401, 402, 403):
        value = r[6] if r[9] == 401 else r[3]
        assert value == 0, (r[9], value)
        events.append(dict(sequence=r[0], kind=r[9], result=value))
for kind in (401, 402, 403):
    assert sum(e['kind'] == kind for e in events) >= 2, (kind, events)
prod = json.loads(a.production_sources.read_text())
diag = json.loads(a.diagnostic_sources.read_text())
assert set(prod) == set(diag)
changed = [k for k in prod if prod[k] != diag[k]]
assert changed == ['src-fix37/runtime.c'], changed
report = dict(passed=True, diagnostic_main_sha256=core['main_sha256'],
              codec_source_sha256=prod['src-fix37/codec/live_w5enc.c'],
              diagnostic_source_changes=changed, closed_checkpoints=closed,
              successful_open_events=events, before=before, after=after,
              observer_register_errors=h[8], observer_overwritten=h[5],
              result_sha256=hashlib.sha256((a.run/'result.json').read_bytes()).hexdigest(),
              scope='Diagnostic ordinary REC closes the same production codec: real EOS/EXIT/dispose/reopen. Not native-player takeover or physical camera acceptance.',
              physical_camera_verified=False)
a.output.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({k: v for k, v in report.items() if k != 'closed_checkpoints'}, indent=2))
