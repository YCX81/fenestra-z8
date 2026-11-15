"""Replay actual firmware MOV/NCDT parsing for final C-writer audio modes."""
from pathlib import Path
import contextlib
import hashlib
import io
import json
import os
import sys

HERE = Path(__file__).resolve().parent
LIVE = HERE.parents[2]
WRITER = HERE.parents[1] / 'writer'
sys.path[:0] = [str(LIVE / 'feat-mov'), str(LIVE / 'feat-mov/evidence')]
import check_ncdt as c
import mov_fix as m

c.ROOT = WRITER / 'parser'
paths = [WRITER / 'nctg-tests/final' / (name + '.MOV') for name in
         ('host-sample', 'no-audio', 'audio-fallback', 'audio-partial-fallback')]
paths += [HERE / 'audio-paths' / (name + '.MOV') for name in
          ('audio', 'silent', 'audio-read-fallback')]
if len(sys.argv) > 1:
    paths = [Path(name) for name in sys.argv[1:]]
cases, movies, rows = [], [], []
for path in paths:
    data = path.read_bytes()
    roots = m.parse(data)
    children = m.direct_child(roots, b'moov').children
    udta = m.direct_child(children, b'udta').encode()
    kinds = [t.one(b'mdia').one(b'hdlr').data[8:12] for t in children if t.typ == b'trak']
    # The final timed metadata track is terminal; no later udta sees kind=4.
    assert children[-1].typ == b'trak' and kinds[-1] == b'meta'
    assert [x.typ for x in children][-3:] == [b'udta', b'meta', b'trak']
    expected = [0, 0, len(udta), 0, 1, 0]
    cases.append((str(path), udta, 1, 0,
                  [(160,120,0), (1280,720,1), (640,360,2), (1920,1080,3)]))
    movies.append((str(path), data, expected))
    rows.append({'path': str(path), 'sha256': hashlib.sha256(data).hexdigest(),
                 'track_order': [x.decode() for x in kinds], 'expected': expected})
stream = io.StringIO()
with contextlib.redirect_stdout(stream):
    c.replay(cases, movies)
print(stream.getvalue(), end='')
Path(os.environ.get('PARSER_REPORT', str(HERE / 'audio-paths/final-parser.json'))).write_text(json.dumps(
    {'status':'PASS', 'scope':'Actual C 3.11 parser with narrow meta-handler hook; no native UI',
     'firmware_sha256': hashlib.sha256((c.ROOT/'bg34.main').read_bytes()).hexdigest(),
     'results':rows, 'log':stream.getvalue()}, indent=2) + '\n')
