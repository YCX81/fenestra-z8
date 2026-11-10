import struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent)); import livectl
t, p = livectl.connect()
try:
    for n, a in [('flag! 0xa03bc6ba0', 0xa03bc6ba0), ('budget', 0xa03bc6bcc), ('G_LIMIT', 0xa03bc6bac), ('G_MODE', 0xa03bc6bc8),
                 ('QTYPE 0=RAW+JPG 1=RAW 3=JPG', 0xa03d482c8), ('QGRADE', 0xa03d482cc), ('RAWREC 1=LL 3=HE* 4=HE', 0xa03d482d8),
                 ('RELMODE', 0xa03d7c7a4), ('G_DRAINED', 0xa03bc5638)]:
        print(f'{n:30s} {struct.unpack("<i", p.debug.read(a, 4))[0]}')
finally:
    t.__exit__(None, None, None)
