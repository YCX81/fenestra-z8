"""Isolated playback diagnostics; never edit the production writer or source MOV."""
from pathlib import Path
import hashlib
import json
import sys

HERE = Path(__file__).resolve().parent
FEAT = HERE.parents[1]
sys.path.insert(0, str(FEAT.parent / 'feat-mov'))
import mov_fix as m

source = (FEAT / 'writer/parser/HOST_4K.MOV').read_bytes()
reference = (FEAT / 'writer/parser/LIV_9013-META.MOV').read_bytes()
outdir = HERE / 'container-variants'
outdir.mkdir(exist_ok=True)

def sha(data):
    return hashlib.sha256(data).hexdigest()

def tracks(blob):
    moov = m.direct_child(m.parse(blob), b'moov')
    return {m.handler(t): t for t in moov.all(b'trak')}

original_tracks = tracks(source)
reference_tracks = tracks(reference)
report = {'source_sha256': sha(source), 'reference_sha256': sha(reference),
          'video_trak_identical': original_tracks[b'vide'].encode() == reference_tracks[b'vide'].encode(),
          'audio_trak_identical': original_tracks[b'soun'].encode() == reference_tracks[b'soun'].encode(),
          'variants': []}

def emit(name, roots, expected_size=None):
    blob = b''.join(x.encode() for x in roots)
    if expected_size is not None:
        assert len(blob) == expected_size
    current_tracks = tracks(blob)
    for kind, original in original_tracks.items():
        assert current_tracks[kind].encode() == original.encode()
    original_mdat = m.direct_child(m.parse(source), b'mdat')
    variant_mdat = m.direct_child(m.parse(blob), b'mdat')
    assert original_mdat.start == variant_mdat.start
    assert original_mdat.encode() == variant_mdat.encode()
    assert m.direct_child(m.parse(source), b'free').encode() == m.direct_child(m.parse(blob), b'free').encode()
    path = outdir / name
    path.write_bytes(blob)
    report['variants'].append({'file': str(path), 'bytes': len(blob), 'sha256': sha(blob),
                               'tracks': [m.handler(t).decode() for t in m.direct_child(m.parse(blob), b'moov').all(b'trak')]})

# Only swap two whole trak boxes; box sizes, mdat, media offsets and all fields stay exact.
roots = m.parse(source)
moov = m.direct_child(roots, b'moov')
indices = [i for i, child in enumerate(moov.children) if child.typ == b'trak']
assert [m.handler(moov.children[i]) for i in indices] == [b'vide', b'meta', b'soun']
moov.children[indices[1]], moov.children[indices[2]] = moov.children[indices[2]], moov.children[indices[1]]
emit('HOST_AVM.MOV', roots, len(source))

# Match reference root child order, without changing any media or NCDT byte.
roots = m.parse(source)
moov = m.direct_child(roots, b'moov')
by_kind = {m.handler(t): t for t in moov.all(b'trak')}
mvhd = m.direct_child(moov.children, b'mvhd')
meta = m.direct_child(moov.children, b'meta')
udta = m.direct_child(moov.children, b'udta')
moov.children = [mvhd, by_kind[b'vide'], meta, by_kind[b'soun'], udta, by_kind[b'meta']]
emit('HOST_AVUM.MOV', roots, len(source))

# Reference additionally has a repeated root Apple meta. Keep same source metadata,
# duplicate only that box. moov occurs after media, so sample offsets do not move.
moov.children.insert(-1, meta)
emit('HOST_AVUM2.MOV', roots)
(outdir / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
