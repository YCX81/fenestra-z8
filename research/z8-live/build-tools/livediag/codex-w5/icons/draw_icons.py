"""Original Live icon studies. Standard library only; writes beside this file."""
from pathlib import Path
import struct
import zlib
import hashlib

ROOT = Path(__file__).resolve().parent
BLACK = (0, 0, 0, 255)
WHITE = (255, 255, 255, 255)


class Canvas:
    def __init__(self, w, h, scale=1):
        self.w, self.h, self.s = w * scale, h * scale, scale
        self.data = bytearray(bytes(BLACK) * self.w * self.h)

    def rect(self, x, y, w, h):
        x, y, w, h = [int(v * self.s) for v in (x, y, w, h)]
        for yy in range(y, y + h):
            start = (yy * self.w + x) * 4
            self.data[start:start + w * 4] = bytes(WHITE) * w

    def frame(self, x, y, w, h, t=3):
        self.rect(x, y, w, t)
        self.rect(x, y+h-t, w, t)
        self.rect(x, y+t, t, h-2*t)
        self.rect(x+w-t, y+t, t, h-2*t)

    def poly(self, pts):
        pts = [(x*self.s, y*self.s) for x, y in pts]
        for y in range(int(min(p[1] for p in pts)), int(max(p[1] for p in pts))):
            hits = []
            for (ax, ay), (bx, by) in zip(pts, pts[1:]+pts[:1]):
                if min(ay, by) <= y+.5 < max(ay, by):
                    hits.append(ax + (y+.5-ay)*(bx-ax)/(by-ay))
            hits.sort()
            for a, b in zip(hits[::2], hits[1::2]):
                for x in range(int(a), int(b)+1):
                    if a <= x+.5 < b:
                        i = (y*self.w+x)*4
                        self.data[i:i+4] = bytes(WHITE)

    def save(self, name):
        def chunk(kind, data):
            return struct.pack('!I', len(data))+kind+data+struct.pack('!I', zlib.crc32(kind+data))
        rows = b''.join(b'\0'+self.data[y*self.w*4:(y+1)*self.w*4] for y in range(self.h))
        png = b'\x89PNG\r\n\x1a\n'
        png += chunk(b'IHDR', struct.pack('!2I5B', self.w, self.h, 8, 6, 0, 0, 0))
        png += chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b'')
        (ROOT/name).write_bytes(png)


def motion(c):
    # Two detached, open chevrons. Exactly three pixels thick.
    for x in (81, 93):
        c.poly([(x, 14), (x+3, 14), (x+11, 22), (x+3, 30),
                (x, 30), (x+8, 22)])


def busy(c):
    # Static write-to-storage cue; no implied progress percentage.
    c.rect(90, 9, 3, 13)
    c.poly([(84, 19), (99, 19), (92, 27)])
    c.rect(81, 28, 3, 7)
    c.rect(99, 28, 3, 7)
    c.rect(84, 32, 15, 3)


def icon(option, writing=False, scale=1):
    c = Canvas(112, 44, scale)
    if option == 1:
        # Still frame containing a rectilinear time pulse.
        c.frame(12, 7, 58, 30)
        for r in [(22, 21, 12, 3), (31, 14, 3, 10),
                  (34, 14, 10, 3), (41, 17, 3, 13),
                  (44, 27, 9, 3), (50, 21, 3, 6), (53, 21, 7, 3)]:
            c.rect(*r)
    elif option == 2:
        # Two exposed frame corners, then one complete photograph.
        for r in [(13, 9, 3, 22), (16, 9, 10, 3),
                  (21, 13, 3, 22), (24, 13, 10, 3)]:
            c.rect(*r)
        c.frame(29, 17, 42, 22)
        c.poly([(40, 33), (47, 23), (54, 33)])
        c.rect(58, 23, 4, 4)
    else:
        # A chosen instant flanked by shorter before/after cells.
        c.frame(10, 15, 12, 14)
        c.frame(27, 7, 28, 30)
        c.frame(60, 15, 12, 14)
        c.poly([(41, 15), (48, 22), (41, 29), (34, 22)])
    (busy if writing else motion)(c)
    return c


FONT = {
 '1':['010','110','010','010','111'], '2':['110','001','010','100','111'],
 '3':['110','001','010','001','110'],
 'N':['101','111','111','111','101'], 'O':['111','101','101','101','111'],
 'R':['110','101','110','101','101'], 'M':['101','111','111','101','101'],
 'A':['010','101','111','101','101'], 'L':['100','100','100','100','111'],
 'B':['110','101','110','101','110'], 'U':['101','101','101','101','111'],
 'S':['111','100','111','001','111'], 'Y':['101','101','010','010','010'],
 ' ':['000']*5,
}


def label(c, text, x, y, size=3):
    for ch in text:
        for yy, row in enumerate(FONT[ch]):
            for xx, v in enumerate(row):
                if v == '1': c.rect(x+xx*size, y+yy*size, size, size)
        x += 4*size


def paste4(dst, src, x, y):
    # Explicit nearest-neighbor replication of the delivered 112x44 bitmap.
    for yy in range(src.h):
        row = b''.join(src.data[(yy*src.w+xx)*4:(yy*src.w+xx+1)*4]*4 for xx in range(src.w))
        for dy in range(4):
            start = ((y+yy*4+dy)*dst.w+x)*4
            dst.data[start:start+len(row)] = row


def main():
    preview = Canvas(1024, 704)
    label(preview, 'NORMAL', 96, 20)
    label(preview, 'BUSY', 560, 20)
    results = []
    for option in (1, 2, 3):
        for writing in (False, True):
            c = icon(option, writing)
            assert len(c.data) == 112*44*4
            assert set(tuple(c.data[i:i+4]) for i in range(0,len(c.data),4)) == {BLACK, WHITE}
            name = f'option-{option}-'+('busy-' if writing else '')+'112x44.png'
            c.save(name)
            results.append(name)
            paste4(preview, c, 560 if writing else 96, 64+(option-1)*216)
        label(preview, str(option), 32, 140+(option-1)*216, 5)
        icon(option, scale=16).save(f'option-{option}-large.png')
        results.append(f'option-{option}-large.png')
    preview.save('preview.png')
    results.append('preview.png')
    for name in results:
        data = (ROOT/name).read_bytes()
        width, height = struct.unpack('!II', data[16:24])
        print(f'{name}: {width}x{height}, RGBA, sha256={hashlib.sha256(data).hexdigest()}')


if __name__ == '__main__':
    main()
