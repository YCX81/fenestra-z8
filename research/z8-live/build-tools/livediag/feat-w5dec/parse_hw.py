"""Run the C3.11 mode-0 MOV parser + HEVC parameter-set extractor (real firmware
code in qemu virt, harness from feat-mov/evidence) on the hardware Live MOV."""
import sys
from pathlib import Path
HERE = Path(__file__).resolve().parent
ROOT = HERE.parent / 'feat-mov'
sys.path[:0] = [str(ROOT), str(ROOT / 'evidence')]
import check_ncdt, mov_fix
orig = (HERE / 'hw-DSC_0028.MOV').read_bytes()
cam = (HERE / 'hw-DSC_0028-camera.MOV').read_bytes()
config = mov_fix.direct_child(mov_fix.parse(cam), b'moov').one(b'hvcC')
lengths = [0, 0]; pos = 23
for tag in (32, 33, 34):
    assert config.data[pos] & 63 == tag
    size = int.from_bytes(config.data[pos+3:pos+5], 'big'); lengths += [tag, size]; pos += 5 + size
print('hvcC', config.data[:23].hex(), lengths)
check_ncdt.replay([], [('hw Apple original', orig, [6]), ('hw camera format', cam, lengths)], hevc_headers=True)
