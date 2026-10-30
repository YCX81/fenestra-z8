"""Map Z9 C5.32 MAIN addresses to Z8 C3.11 MAIN by relocation-normalized code matching.

Code addresses: k-gram votes over a window around the address (PC-relative and
ADRP-paired immediates masked), then a similarity score on the winning delta.
Data addresses: find Z9 code that materializes the address (ADRP+ADD/LDR/STR,
MOVZ/MOVK chains), map those sites to Z8 and decode what the Z8 code builds.
Run: python -I addrmap.py ADDR... | --file list.txt   (hex, one per line)
"""
import hashlib, struct, sys
from pathlib import Path
import numpy as np

BASE = 0x40160000
NIKON = Path(__file__).resolve().parents[3]
Z9 = NIKON / 'z8-mcu-audit-20261019/.local/z9modules/eg1990_053200.bi'
Z8 = NIKON / 'z8-integration-20261014/.local/firmware/z8-c311-main.bin'
SHA = {Z9: '6380ded18395ce11cb327c766721274c85127d2cf1a1e887ee60e3d156810bdd',
       Z8: '2b43a31c9de838f84b12416de4348a7004e65be07f853ec56ec2b10f3a8e3853'}
K, WIN = 6, 48


def words(path):
    data = path.read_bytes()
    assert hashlib.sha256(data).hexdigest() == SHA[path], path
    n = len(data) // 4
    return data, np.frombuffer(data[:n * 4], dtype='<u4').copy()


def normalize(w):
    v = w.copy()
    top6, top8 = w >> 26, w >> 24
    v[(top6 == 0x05) | (top6 == 0x25)] &= 0xfc000000                       # b / bl
    m = (top8 == 0x54); v[m] &= 0xff00001f                                  # b.cond
    m = (w & 0x7e000000) == 0x34000000; v[m] &= 0xff00001f                  # cbz/cbnz
    m = (w & 0x7e000000) == 0x36000000; v[m] &= 0xfff8001f                  # tbz/tbnz
    adrp = (w & 0x1f000000) == 0x10000000; v[adrp] &= 0x9f00001f            # adr/adrp
    m = (w & 0x3b000000) == 0x18000000; v[m] &= 0xff00001f                  # ldr literal
    # imm12 of add / ldr / str whose base came from an adrp within the previous 3 words
    rn = (w >> 5) & 31
    addi = (w & 0x7f800000) == 0x11000000
    ldst = (w & 0x3b000000) == 0x39000000
    cand = addi | ldst
    for back in (1, 2, 3):
        prev = np.zeros_like(w); prev[back:] = w[:-back]
        hit = cand & ((prev & 0x9f000000) == 0x90000000) & ((prev & 31) == rn)
        v[hit] &= 0xffc003ff
    # movz/movk immediates (64-bit globals are built this way)
    m =((w & 0x7f800000) == 0x52800000) | ((w & 0x7f800000) == 0x72800000)
    big = m & ((w >> 21 & 3) >= 1)            # only hw>=1 halves: keep small constants exact
    v[big] &= 0xffe0001f
    return v


def kgram_hash(v):
    h = np.zeros(len(v) - K + 1, dtype=np.uint64)
    for i in range(K):
        h = h * np.uint64(0x9E3779B97F4A7C15) + v[i:len(v) - K + 1 + i].astype(np.uint64)
    return h


class Mapper:
    def __init__(self):
        self.d9, self.w9 = words(Z9)
        self.d8, self.w8 = words(Z8)
        self.n9, self.n8 = normalize(self.w9), normalize(self.w8)
        self.h9 = kgram_hash(self.n9)
        h8 = kgram_hash(self.n8)
        self.order = np.argsort(h8, kind='stable'); self.sorted8 = h8[self.order]

    def lookup(self, h):
        lo = np.searchsorted(self.sorted8, h, 'left'); hi = np.searchsorted(self.sorted8, h, 'right')
        return self.order[lo:hi] if hi - lo <= 8 else ()

    def code(self, va):
        i = (va - BASE) // 4
        votes = {}
        for j in range(max(0, i - WIN), min(len(self.h9), i + WIN)):
            for p in self.lookup(self.h9[j]):
                d = int(p) - j
                votes[d] = votes.get(d, 0) + 1
        if not votes:
            return None
        best = sorted(votes.items(), key=lambda kv: -kv[1])[:2]
        d = best[0][0]; j8 = i + d
        lo, hi = max(0, i - 16), i + 16
        sim = float(np.mean(self.n9[lo:hi] == self.n8[lo + d:hi + d]))
        exact = bool(self.w9[i] == self.w8[j8])
        second = best[1][1] if len(best) > 1 else 0
        return dict(z8=BASE + 4 * j8, votes=best[0][1], second=second, sim=round(sim, 3), exact_word=exact)

    # ---- data references ----
    def _refs(self, w):
        """Yield (index, target) for adrp+add/ldr/str pairs within 3 words."""
        n = len(w)
        idx = np.nonzero((w & 0x9f000000) == 0x90000000)[0]
        out = {}
        for i in idx:
            a = int(w[i]); rd = a & 31
            imm = ((a >> 29) & 3) | (((a >> 5) & 0x7ffff) << 2)
            if imm & (1 << 20): imm -= 1 << 21
            page = ((BASE + 4 * int(i)) & ~0xfff) + (imm << 12)
            for k in (1, 2, 3):
                if i + k >= n: break
                b = int(w[i + k])
                if (b >> 5) & 31 != rd: continue
                if (b & 0x7f800000) == 0x11000000:
                    t = page + ((b >> 10) & 0xfff) * (4096 if (b >> 22) & 1 else 1)
                elif (b & 0x3b000000) == 0x39000000:
                    t = page + ((b >> 10) & 0xfff) * (1 << (b >> 30))
                else:
                    continue
                out.setdefault(t & 0xffffffffffff, []).append(int(i))
                break
        return out

    def _movchains(self, w):
        """Yield (index, value) for movz x + movk x chains building >32-bit constants."""
        out = {}
        idx = np.nonzero((w & 0xff800000) == 0xd2800000)[0]               # movz x
        for i in idx:
            a = int(w[i]); rd = a & 31; val = ((a >> 5) & 0xffff) << (16 * ((a >> 21) & 3)); k = 1
            while i + k < len(w) and k < 6:
                b = int(w[i + k])
                if (b & 0xff800000) == 0xf2800000 and (b & 31) == rd:
                    sh = 16 * ((b >> 21) & 3); val = (val & ~(0xffff << sh)) | (((b >> 5) & 0xffff) << sh)
                k += 1
            if val >> 32:
                out.setdefault(val, []).append(int(i))
        return out

    def data(self, va):
        if not hasattr(self, 'r9'):
            self.r9, self.r8 = self._refs(self.w9), None
            self.m9 = self._movchains(self.w9)
        sites = [(i, 'adrp') for i in self.r9.get(va, [])] + [(i, 'mov') for i in self.m9.get(va, [])]
        results = {}
        for i, kind in sites[:24]:
            m = self.code(BASE + 4 * i)
            if not m or m['sim'] < 0.6: continue
            j = (m['z8'] - BASE) // 4
            t = self._decode_at(j, kind)
            if t is not None:
                results[t] = results.get(t, 0) + 1
        return dict(sites=len(sites), candidates=sorted(results.items(), key=lambda kv: -kv[1])[:3])

    def _decode_at(self, j, kind):
        w = self.w8
        if kind == 'adrp':
            a = int(w[j])
            if (a & 0x9f000000) != 0x90000000: return None
            rd = a & 31; imm = ((a >> 29) & 3) | (((a >> 5) & 0x7ffff) << 2)
            if imm & (1 << 20): imm -= 1 << 21
            page = ((BASE + 4 * j) & ~0xfff) + (imm << 12)
            for k in (1, 2, 3):
                b = int(w[j + k])
                if (b >> 5) & 31 != rd: continue
                if (b & 0x7f800000) == 0x11000000:
                    return page + ((b >> 10) & 0xfff) * (4096 if (b >> 22) & 1 else 1)
                if (b & 0x3b000000) == 0x39000000:
                    return page + ((b >> 10) & 0xfff) * (1 << (b >> 30))
            return None
        a = int(w[j])
        if (a & 0xff800000) != 0xd2800000: return None
        rd = a & 31; val = ((a >> 5) & 0xffff) << (16 * ((a >> 21) & 3))
        for k in range(1, 6):
            b = int(w[j + k])
            if (b & 0xff800000) == 0xf2800000 and (b & 31) == rd:
                sh = 16 * ((b >> 21) & 3); val = (val & ~(0xffff << sh)) | (((b >> 5) & 0xffff) << sh)
        return val


def is_code(va):
    return 0x40160000 <= va < 0x42000000


if __name__ == '__main__':
    args = sys.argv[1:]
    if args and args[0] == '--file':
        args = [l.split()[0] for l in Path(args[1]).read_text().splitlines() if l.strip() and not l.startswith('#')]
    m = Mapper()
    for a in args:
        va = int(a, 16)
        r = m.code(va) if is_code(va) else m.data(va)
        if r and 'z8' in r: r['z8'] = hex(r['z8'])
        if r and 'candidates' in r: r['candidates'] = [(hex(t), c) for t, c in r['candidates']]
        print(hex(va), r, flush=True)
