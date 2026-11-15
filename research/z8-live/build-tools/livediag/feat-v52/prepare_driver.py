"""Regenerate passive observer layouts from the exact AArch64 source headers."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent
BASE = ROOT.parent / 'feat-v50/playback/validated-v51d-recovery-driver'
p = argparse.ArgumentParser()
p.add_argument('source', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
assert not a.output.exists()
shutil.copytree(BASE, a.output)
fields = [k for k in json.loads((BASE / 'runtime-offsets.json').read_text()) if k != 'struct_bytes']
fields += ['capture.descriptor', 'stream.fr_desc', 'stream.encoder.hw', 'audio.active',
           'audio.descriptor', 'audio.cancelled', 'audio_z8.phase', 'audio_z8.closed',
           'audio_z8.stop_ack', 'audio_z8.stop_timeouts', 'audio_z8.blocks', 'audio_z8.dropped',
           'audio_phase', 'audio_error', 'audio_starts', 'audio_tracks', 'audio_skipped',
           'crumb_pad', 'stock_handoff_lock', 'stream.cfr_epoch', 'stream.cfr_index',
           'stream.cfr_source_us', 'stream.cfr_repeats', 'stream.cfr_have']
pbfields = ['audio_stage', 'audio_plays', 'audio_errors', 'audio_stop', 'audio_waits',
            'audio_skipped', 'audio_reason', 'audio_source_frames', 'audio_pending',
            'audio_gate', 'audio_request_gate', 'audio_stop_retries']

def layout(name, folder, typename, names):
    c = a.output / (name + '-layout.c')
    asm = c.with_suffix('.s')
    c.write_text('#include <stddef.h>\n#include "runtime.h"\nconst unsigned offsets[]={\n' +
                 f'sizeof({typename}),\n' + ',\n'.join(
                 f'offsetof({typename},{f}),sizeof((({typename}*)0)->{f})' for f in names) + '\n};\n')
    subprocess.run(['clang', '--target=aarch64-none-elf', '-ffreestanding', '-Os', '-S',
                    '-I', str(folder), '-I', str(folder / 'core-include'), str(c), '-o', str(asm)], check=True)
    values = [int(v) for v in re.findall(r'\.word\s+(\d+)', asm.read_text())]
    assert len(values) == 1 + len(names) * 2, (name, values)
    offsets = dict(zip(names, values[1::2])); offsets['struct_bytes'] = values[0]
    widths = dict(zip(names, values[2::2]))
    (a.output / (name + '-offsets.json')).write_text(json.dumps(offsets, indent=2) + '\n')
    (a.output / (name + '-widths.json')).write_text(json.dumps(widths, indent=2) + '\n')
    return offsets, widths

runtime, widths = layout('runtime', a.source / 'src-fix37', 'struct live_runtime', fields)
pb, pbwidths = layout('pb', a.source / 'pb-fix37', 'PbRuntime', pbfields)
driver = a.output / 'retry-driver/run_live.py'
s = driver.read_text()
anchor = 'OFFSETS=json.loads(OFFSET_PATH.read_text())'
assert s.count(anchor) == 1
s = s.replace(anchor, anchor + "\nFIELD_WIDTHS=json.loads((TASK/'runtime-widths.json').read_text())\nPB_OFFSETS=json.loads((TASK/'pb-offsets.json').read_text())\nPB_WIDTHS=json.loads((TASK/'pb-widths.json').read_text())")
anchor = "            runtime[name]=int.from_bytes(blob[offset:offset+width],'little')"
assert s.count(anchor) == 1
s = s.replace(anchor, "            width=FIELD_WIDTHS[name]\n" + anchor)
s = s.replace('Runtime ABI size disagrees with v51d source layout', 'Runtime ABI size disagrees with this exact AArch64 source layout')
anchor = "                s['pb']['error']=rd(pb+rd(meta+5*4,4),4)"
assert s.count(anchor) == 1
s = s.replace(anchor, anchor + "\n                pb_blob=g.read_bytes(pb,PB_OFFSETS['struct_bytes'])\n                for field,width in PB_WIDTHS.items():\n                    at=PB_OFFSETS[field];s['pb'][field]=int.from_bytes(pb_blob[at:at+width],'little')")
driver.write_text(s)
receipt = dict(source=str(a.source.resolve()), runtime_bytes=runtime['struct_bytes'],
               pb_bytes=pb['struct_bytes'], source_layout_target='aarch64-none-elf',
               changes='Passive field offsets/widths and PB audio observations only; no deadline/input/assertion changes',
               driver_sha256=hashlib.sha256(driver.read_bytes()).hexdigest())
(a.output / 'v52-layout-provenance.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps(receipt, indent=2))
