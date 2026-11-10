#!/usr/bin/env python3
"""Z8 card and debug access over Fenestra USB Debug (no card removal).

Uses the stock serialized file service through native calls, the same entries Live uses:
open 0x402dd4f8(path, flags, 0x20) (flags 0 = read, 0x101 = create exclusive),
read 0x402dd6c8(fd, buf, n, &done), write 0x402dd78c(fd, buf, n, &done, 0),
seek 0x402dd8e8(fd, off, whence, &pos) (nonzero = ok), close 0x402dd558,
remove 0x402dda4c(path), mkdir 0x402ddbdc(path), dir 0x402ddc6c/0x402ddd1c/0x402ddd6c.

  python z8card.py ls A:\\DCIM
  python z8card.py stat A:\\Z_8_0405.bin
  python z8card.py get A:\\LIVE\\100NCZ_8\\DSC_0001.MOV out.mov
  python z8card.py put local.bin A:\\TEST.BIN [--replace]
  python z8card.py rm A:\\TEST.BIN
  python z8card.py mkdir A:\\TMP
  python z8card.py fwpush NEW_Z_8_0405.bin --old OLD_Z_8_0405.bin [--card A:\\Z_8_0405.bin]
      Replace the firmware file on the card with NEW while sending only the 4 KiB blocks
      that differ from OLD (which must be the file now on the card; checked by sampling).
      The camera copies the rest card-to-card. The old file is kept as Z_8_PREV.bin.
  python z8card.py fwpush2 NEW_Z_8_0405.bin --old OLD_Z_8_0405.bin [--card A:\\Z_8_0405.bin] [--no-keep]
      Fast verified push: changed blocks only, camera builds the file card-to-card, hashes it with
      Fenestra's own SHA-256 (whole file, only 32 bytes over USB), then swaps by rename (old kept as Z_8_PREV.bin).
  python z8card.py sha [A:\\FILE [LOCAL]]   camera-side SHA-256 of a card file (optionally compare to LOCAL)
  python z8card.py shell "help"            native shell line(s)
  python z8card.py repl                    interactive: shell lines, plus :ls :get :put :peek
"""
import argparse
import hashlib
import os
import random
import struct
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import z8_audio_probe as zp  # noqa: E402

F_OPEN, F_READ, F_WRITE, F_CLOSE, F_SEEK = 0x402dd4f8, 0x402dd6c8, 0x402dd78c, 0x402dd558, 0x402dd8e8
F_REMOVE, F_MKDIR = 0x402dda4c, 0x402ddbdc
F_RENAME = 0x402dd9fc          # camera.card.rename(from, to) -> 0 ok (Fenestra z8-c311 profile)
# Fenestra core SHA-256 in the C3.11 LivePhoto builds (8166b8db..v10 6ac3526c); identity-checked before use.
SHA_UPDATE, SHA_FINAL = 0x401c6d70, 0x401c7030
SHA_UPDATE_SIG = bytes.fromhex('800200b4fd7bbca9f65702a9f50302aafd030091f44f03a9f30300aaf40301aaf85f01a9')
SHA_FINAL_SIG = bytes.fromhex('ff8303d1f44f0da9f30301aaf40300aae1031f2a')
RENAME_SIG = bytes.fromhex('f553bea9f37b01a915bb83d2155da0f2')
SHA_IV = (0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19)
HEAP_ALLOC, HEAP_FREE = 0x40d2c408, 0x40d2c448
BUF_BYTES = 0x100000          # 1 MiB camera-side transfer buffer
BLOCK = 4096
MAGIC = b'Z8CARD01'
CHUNK = 1008                  # Fenestra RTOS monitor read/write limit


def s32(v):
    v &= 0xffffffff
    return v - (1 << 32) if v & 0x80000000 else v


class Card:
    def __init__(self, p, log_dir):
        self.p, self.log_dir = p, log_dir
        self.base = self._scratch()
        self.path_at, self.word_at, self.buf = self.base + 0x10, self.base + 0x200, self.base + 0x1000

    def _scratch(self):
        # Always a fresh heap block for this session. An address remembered from an earlier
        # session is unsafe: DRAM survives a camera restart, so the magic can still be there
        # while the camera has reused that memory (11-07: corrupted a pushed firmware file).
        # 4 KiB of slack after the buffer: 11-07 the last 48 bytes of a 1 MiB chunk came out wrong
        # exactly at the end of an allocation sized to the buffer.
        base = zp.native_call(self.p, HEAP_ALLOC, (BUF_BYTES + 0x2000, 0))
        if not base:
            raise SystemExit('camera heap allocation (1 MiB) failed')
        self.p.debug.write(base, MAGIC)
        (self.log_dir / 'z8card-scratch.txt').write_text(f'{base:#x} (this session only, freed at exit)\n')
        return base

    def release(self):
        if self.base:
            try:
                zp.native_call(self.p, HEAP_FREE, (self.base,))
            finally:
                self.base = 0

    def intact(self):
        return bytes(self.p.debug.read(self.base, 8)) == MAGIC

    def call(self, address, *args, timeout_ms=10000):
        return zp.native_call(self.p, address, args, timeout_ms)

    def set_path(self, path, at=None):
        raw = path.encode('ascii') + b'\0'
        if len(raw) > 0x1f0:
            raise SystemExit('path too long')
        self.p.debug.write(at or self.path_at, raw)
        return at or self.path_at

    def mem_write(self, address, data):
        for off in range(0, len(data), CHUNK):
            self.p.debug.write(address + off, data[off:off + CHUNK])

    def mem_read(self, address, n):
        out = bytearray()
        while len(out) < n:
            out += self.p.debug.read(address + len(out), min(CHUNK, n - len(out)))
        return bytes(out)

    def u64(self, at):
        return struct.unpack('<Q', self.p.debug.read(at, 8))[0]

    # file primitives -------------------------------------------------------------
    def open(self, path, create=False):
        fd = s32(self.call(F_OPEN, self.set_path(path), 0x101 if create else 0, 0x20))
        if fd < 0 or (fd & 0xffffffff) == 0x7fff0000:
            raise SystemExit(f'open {path} ({"create" if create else "read"}) failed: {fd & 0xffffffff:#x}')
        return fd & 0xffffffff

    def close(self, fd):
        rc = self.call(F_CLOSE, fd) & 0xffffffff
        if rc:
            raise SystemExit(f'close failed {rc:#x}')

    def seek(self, fd, offset, whence=0):
        self.p.debug.write(self.word_at, bytes(8))
        if not self.call(F_SEEK, fd, offset, whence, self.word_at) & 0xffffffff:
            raise SystemExit(f'seek {offset} failed')
        return self.u64(self.word_at)

    def read_into_buf(self, fd, n):
        self.p.debug.write(self.word_at, bytes(4))
        rc = self.call(F_READ, fd, self.buf, n, self.word_at) & 0xffffffff
        done = self.p.u32(self.word_at)
        if rc or done > n:
            raise SystemExit(f'read failed rc {rc:#x} done {done}')
        return done

    def write_from_buf(self, fd, n):
        self.p.debug.write(self.word_at, bytes(4))
        rc = self.call(F_WRITE, fd, self.buf, n, self.word_at, 0) & 0xffffffff
        done = self.p.u32(self.word_at)
        if rc or done != n:
            raise SystemExit(f'write failed rc {rc:#x} done {done}/{n}')

    def size(self, path):
        fd = self.open(path)
        try:
            return self.seek(fd, 0, 2)
        finally:
            self.close(fd)

    def exists(self, path):
        fd = s32(self.call(F_OPEN, self.set_path(path), 0, 0x20))
        if fd < 0 or (fd & 0xffffffff) == 0x7fff0000:
            return False
        self.close(fd & 0xffffffff)
        return True

    def remove(self, path):
        rc = self.call(F_REMOVE, self.set_path(path)) & 0xffffffff
        if rc:
            raise SystemExit(f'remove {path} failed {rc:#x}')

    def mkdir(self, path):
        rc = self.call(F_MKDIR, self.set_path(path)) & 0xffffffff
        if rc:
            raise SystemExit(f'mkdir {path} failed {rc:#x}')

    def read_range(self, path, offset, n):
        fd = self.open(path)
        try:
            if self.seek(fd, offset) != offset:
                raise SystemExit('seek mismatch')
            got = self.read_into_buf(fd, n)
            return self.mem_read(self.buf, got)
        finally:
            self.close(fd)

    # commands ----------------------------------------------------------------------
    def get(self, path, out):
        fd = self.open(path)
        total, t0 = 0, time.monotonic()
        try:
            with open(out, 'xb') as f:
                while True:
                    done = self.read_into_buf(fd, BUF_BYTES)
                    if not done:
                        break
                    f.write(self.mem_read(self.buf, done))
                    total += done
                    print(f'  {total} B  {total / (time.monotonic() - t0) / 1024:.0f} KiB/s', flush=True)
        finally:
            self.close(fd)
        return total

    def put(self, local, path, replace=False):
        data = Path(local).read_bytes()
        if self.exists(path):
            if not replace:
                raise SystemExit(f'{path} exists (use --replace)')
            self.remove(path)
        fd = self.open(path, create=True)
        t0 = time.monotonic()
        try:
            for off in range(0, len(data), BUF_BYTES):
                part = data[off:off + BUF_BYTES]
                self.mem_write(self.buf, part)
                self.write_from_buf(fd, len(part))
                print(f'  {off + len(part)}/{len(data)} B  {(off + len(part)) / (time.monotonic() - t0) / 1024:.0f} KiB/s', flush=True)
        finally:
            self.close(fd)
        size = self.size(path)
        if size != len(data):
            raise SystemExit(f'size mismatch on card: {size} != {len(data)}')
        # spot-check the head and tail
        for off in {0, max(0, len(data) - 4096)}:
            if self.read_range(path, off, min(4096, len(data) - off)) != data[off:off + 4096]:
                raise SystemExit(f'readback differs at {off}')
        return len(data)

    def settle(self, seconds=3.0):
        # After long card writes the camera answers PTP 0x2019 (busy) for a while.
        time.sleep(seconds)

    def fwpush(self, new_path, old_path, card_path, keep, samples, resume=False):
        new, old = Path(new_path).read_bytes(), Path(old_path).read_bytes()
        if len(new) != len(old):
            raise SystemExit('fwpush needs NEW and OLD of equal size (same container layout)')
        tmp0 = card_path.rsplit('\\', 1)[0] + '\\Z8NEW.TMP'
        if resume and not self.exists(card_path) and self.exists(tmp0) and self.size(tmp0) == len(new):
            # Interrupted after the old file was removed: the verified copy is the only one left.
            diff = [o for o in range(0, len(new), BLOCK) if new[o:o + BLOCK] != old[o:o + BLOCK]]
            print('resume: card file missing, finishing from', tmp0)
            return self._finish(new, old, new_path, card_path, tmp0, diff, [], False)
        size = self.size(card_path)
        if size != len(old):
            raise SystemExit(f'{card_path} is {size} B, OLD is {len(old)} B: the card holds a different file')
        diff = [o for o in range(0, len(new), BLOCK) if new[o:o + BLOCK] != old[o:o + BLOCK]]
        print(f'{len(diff)} of {len(new) // BLOCK + 1} blocks differ ({len(diff) * BLOCK / 1024:.0f} KiB to send)')
        # The card file must be OLD: check every block we will NOT send that neighbours a
        # changed one, plus random samples, against OLD.
        rnd = random.Random(len(new))
        check = sorted(set(rnd.sample(range(0, len(old), BLOCK), min(samples, len(old) // BLOCK)) + diff[:8]))
        for o in check:
            if self.read_range(card_path, o, min(BLOCK, len(old) - o)) != old[o:o + BLOCK]:
                raise SystemExit(f'card file differs from OLD at {o:#x}: wrong --old, nothing changed')
        print(f'card file matches OLD at {len(check)} sampled blocks')
        tmp = card_path.rsplit('\\', 1)[0] + '\\Z8NEW.TMP'
        if resume and self.exists(tmp) and self.size(tmp) == len(new):
            print(f'resume: verifying existing {tmp}')
            return self._finish(new, old, new_path, card_path, tmp, diff, check, keep)
        if self.exists(tmp):
            self.remove(tmp)
        src = self.open(card_path)
        try:
            dst = self.open(tmp, create=True)
        except BaseException:
            self.close(src)   # do not leak the camera-side handle
            raise
        dset = set(diff)
        t0 = time.monotonic()
        try:
            for off in range(0, len(new), BUF_BYTES):
                n = self.read_into_buf(src, min(BUF_BYTES, len(new) - off))
                if n != min(BUF_BYTES, len(new) - off):
                    raise SystemExit(f'short read at {off}')
                for b in range(off, off + n, BLOCK):
                    if b in dset:
                        self.mem_write(self.buf + (b - off), new[b:min(b + BLOCK, len(new))])
                self.write_from_buf(dst, n)
                if not self.intact():
                    raise SystemExit('camera-side buffer header changed during the copy; stopping')
                print(f'  {off + n}/{len(new)} B  {time.monotonic() - t0:.0f} s', flush=True)
        finally:
            self.close(src)
            self.close(dst)
        self.settle()
        return self._finish(new, old, new_path, card_path, tmp, diff, check, keep)

    def _finish(self, new, old, new_path, card_path, tmp, diff, check, keep):
        t0 = time.monotonic()
        if self.size(tmp) != len(new):
            raise SystemExit('size mismatch on the new file; card file left unchanged')
        for o in sorted(set(diff + check)):
            if self.read_range(tmp, o, min(BLOCK, len(new) - o)) != new[o:o + BLOCK]:
                raise SystemExit(f'new file differs at {o:#x}; card file left unchanged')
        print(f'new file verified at {len(set(diff + check))} blocks')
        # Swap: keep the old file under another name by a card-local copy, then replace.
        prev = card_path.rsplit('\\', 1)[0] + '\\Z_8_PREV.bin'
        if keep and self.exists(card_path):
            if self.exists(prev):
                self.remove(prev)
            self.copy(card_path, prev, len(old))
        if self.exists(card_path):
            self.remove(card_path)
        self.copy(tmp, card_path, len(new))
        self.settle()
        if self.size(card_path) != len(new):
            raise SystemExit(f'final size mismatch; the verified copy is still at {tmp}')
        for o in sorted(set(diff))[:64]:
            if self.read_range(card_path, o, min(BLOCK, len(new) - o)) != new[o:o + BLOCK]:
                raise SystemExit(f'final file differs at {o:#x}; the verified copy is still at {tmp}')
        self.remove(tmp)
        print(f'{card_path} now = {Path(new_path).name} sha256 {hashlib.sha256(new).hexdigest()[:16]}...'
              f' ({time.monotonic() - t0:.0f} s)')

    def repair(self, new_path, card_path, samples=300):
        """Card file is meant to be NEW but may have corrupted blocks (stale-buffer bug): find them
        (first two blocks of every 1 MiB chunk + random samples), rebuild through a fresh buffer."""
        new = Path(new_path).read_bytes()
        if self.size(card_path) != len(new):
            raise SystemExit('card file size differs from NEW: use fwpush or a card reader')
        rnd = random.Random(len(new) ^ 0x5a5a)
        cand = sorted(set([o for k in range(0, len(new), BUF_BYTES) for o in (k, k + BLOCK, k + BUF_BYTES - BLOCK) if o < len(new)] +
                          rnd.sample(range(0, len(new), BLOCK), min(samples, len(new) // BLOCK)) +
                          [len(new) - len(new) % BLOCK]))
        bad = [o for o in cand if self.read_range(card_path, o, min(BLOCK, len(new) - o)) != new[o:o + BLOCK]]
        print(f'checked {len(cand)} blocks, {len(bad)} differ from NEW: {[hex(o) for o in bad[:12]]}')
        if not bad:
            print('nothing to repair'); return
        tmp = card_path.rsplit('\\', 1)[0] + '\\Z8NEW.TMP'
        if self.exists(tmp):
            self.remove(tmp)
        src = self.open(card_path)
        try:
            dst = self.open(tmp, create=True)
        except BaseException:
            self.close(src)   # do not leak the camera-side handle
            raise
        dset = set(bad); t0 = time.monotonic()
        try:
            for off in range(0, len(new), BUF_BYTES):
                n = self.read_into_buf(src, min(BUF_BYTES, len(new) - off))
                if n != min(BUF_BYTES, len(new) - off):
                    raise SystemExit(f'short read at {off}')
                for b in range(off, off + n, BLOCK):
                    if b in dset:
                        self.mem_write(self.buf + (b - off), new[b:min(b + BLOCK, len(new))])
                self.write_from_buf(dst, n)
                if not self.intact():
                    raise SystemExit('camera-side buffer header changed during the copy; stopping')
        finally:
            self.close(src); self.close(dst)
        self.settle()
        # Verify every candidate block (incl. the repaired ones) in the rebuilt file, then swap.
        miss = [o for o in cand if self.read_range(tmp, o, min(BLOCK, len(new) - o)) != new[o:o + BLOCK]]
        if miss:
            raise SystemExit(f'rebuilt file still differs at {[hex(o) for o in miss[:8]]}; card file left as it was')
        print(f'rebuilt file verified at {len(cand)} blocks ({time.monotonic() - t0:.0f} s)')
        self.remove(card_path)
        self.copy(tmp, card_path, len(new))
        self.settle()
        miss = [o for o in cand if self.read_range(card_path, o, min(BLOCK, len(new) - o)) != new[o:o + BLOCK]]
        if miss:
            raise SystemExit(f'final file differs at {[hex(o) for o in miss[:8]]}; verified copy kept at {tmp}')
        self.remove(tmp)
        print(f'{card_path} repaired and verified at {len(cand)} blocks')

    # fast path: camera-side hashing and rename --------------------------------------
    def _check_code(self):
        for addr, sig, name in ((SHA_UPDATE, SHA_UPDATE_SIG, 'sha256_update'), (SHA_FINAL, SHA_FINAL_SIG, 'sha256_final'),
                                (F_RENAME, RENAME_SIG, 'card.rename')):
            if self.mem_read(addr, len(sig)) != sig:
                raise SystemExit(f'{name} at {addr:#x} is not the expected code: unsupported firmware build')

    def sha256(self, path, size=None):
        """SHA-256 of a card file computed on the camera (Fenestra's own fx_sha256_update/final),
        1 MiB per native read; only 32 bytes come back over USB."""
        self._check_code()
        ctx, out = self.base + 0x800, self.base + 0x900
        self.mem_write(ctx, struct.pack('<8I', *SHA_IV) + bytes(80))
        fd = self.open(path); done = 0; t0 = time.monotonic()
        try:
            while True:
                n = self.read_into_buf(fd, BUF_BYTES)
                if not n:
                    break
                rc = self.call(SHA_UPDATE, ctx, self.buf, n) & 0xffffffff
                if rc:
                    raise SystemExit(f'sha256_update returned {rc:#x}')
                if not self.intact():
                    raise SystemExit('camera-side buffer header changed while hashing; stopping')
                done += n
                if n < BUF_BYTES:
                    break
        finally:
            self.close(fd)
        if size is not None and done != size:
            raise SystemExit(f'hashed {done} B, expected {size}')
        self.call(SHA_FINAL, ctx, out)
        digest = self.mem_read(out, 32)
        print(f'  camera sha256 {path}: {digest.hex()[:16]}... ({done} B, {time.monotonic() - t0:.0f} s)')
        return digest

    def rename(self, src, dst):
        self._check_code()
        a = self.set_path(src, self.base + 0x400); b = self.set_path(dst, self.base + 0x600)
        rc = self.call(F_RENAME, a, b) & 0xffffffff
        if rc or self.exists(src) or not self.exists(dst):
            raise SystemExit(f'rename {src} -> {dst} failed ({rc:#x})')

    def fwpush2(self, new_path, old_path, card_path, keep=True):
        """Fast verified update file push: send only changed 4 KiB blocks, the camera builds the new file
        card-to-card, hashes it itself (must equal sha256(NEW)), then swaps by rename."""
        new, old = Path(new_path).read_bytes(), Path(old_path).read_bytes()
        if len(new) != len(old):
            raise SystemExit('fwpush needs NEW and OLD of equal size (same container layout)')
        want = hashlib.sha256(new).digest(); t_all = time.monotonic()
        if self.size(card_path) != len(old):
            raise SystemExit(f'{card_path} is not the size of OLD: the card holds a different file')
        diff = [o for o in range(0, len(new), BLOCK) if new[o:o + BLOCK] != old[o:o + BLOCK]]
        print(f'{len(diff)} changed blocks ({len(diff) * BLOCK // 1024} KiB to send)')
        folder = card_path.rsplit('\\', 1)[0]
        tmp, prev = folder + '\\Z8NEW.TMP', folder + '\\Z_8_PREV.bin'
        if self.exists(tmp):
            self.remove(tmp)
        src = self.open(card_path)
        try:
            dst = self.open(tmp, create=True)
        except BaseException:
            self.close(src)   # do not leak the camera-side handle
            raise
        dset = set(diff); t0 = time.monotonic()
        try:
            for off in range(0, len(new), BUF_BYTES):
                n = self.read_into_buf(src, min(BUF_BYTES, len(new) - off))
                if n != min(BUF_BYTES, len(new) - off):
                    raise SystemExit(f'short read at {off}')
                for b in range(off, off + n, BLOCK):
                    if b in dset:
                        self.mem_write(self.buf + (b - off), new[b:min(b + BLOCK, len(new))])
                self.write_from_buf(dst, n)
                if not self.intact():
                    raise SystemExit('camera-side buffer header changed during the copy; stopping')
        finally:
            self.close(src); self.close(dst)
        print(f'  new file built on the card in {time.monotonic() - t0:.0f} s')
        self.settle()
        if self.sha256(tmp, len(new)) != want:
            raise SystemExit(f'{tmp} does not hash to NEW (wrong OLD on the card?); {card_path} left unchanged')
        if keep:
            if self.exists(prev):
                self.remove(prev)
            self.rename(card_path, prev)
        else:
            self.remove(card_path)
        self.rename(tmp, card_path)
        if self.size(card_path) != len(new):
            raise SystemExit(f'{card_path} has the wrong size after the rename')
        print(f'{card_path} = {Path(new_path).name}, sha256 {want.hex()[:16]}... verified on the camera '
              f'(total {time.monotonic() - t_all:.0f} s){"; old file kept as " + prev if keep else ""}')

    def copy(self, src_path, dst_path, size):
        src, dst = self.open(src_path), self.open(dst_path, create=True)
        try:
            for off in range(0, size, BUF_BYTES):
                n = self.read_into_buf(src, min(BUF_BYTES, size - off))
                self.write_from_buf(dst, n)
                if not self.intact():
                    raise SystemExit('camera-side buffer header changed during the copy; stopping')
        finally:
            self.close(src)
            self.close(dst)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['ls', 'stat', 'get', 'put', 'rm', 'mkdir', 'fwpush', 'fwpush2', 'sha', 'repair', 'shell', 'repl', 'speed'])
    ap.add_argument('args', nargs='*')
    ap.add_argument('--replace', action='store_true')
    ap.add_argument('--old')
    ap.add_argument('--card', default='A:\\Z_8_0405.bin')
    ap.add_argument('--no-keep', action='store_true', help='fwpush: do not keep the old file as Z_8_PREV.bin')
    ap.add_argument('--keep', action='store_true', help='fwpush2: keep the old file as Z_8_PREV.bin (off by default)')
    ap.add_argument('--samples', type=int, default=48)
    ap.add_argument('--resume', action='store_true', help='fwpush: reuse a complete Z8NEW.TMP left by an interrupted push')
    a = ap.parse_args()
    import livectl
    c = None
    t, p = livectl.connect()
    log_dir = HERE / 'logs'
    try:
        if a.cmd == 'shell':
            for line in a.args:
                print(f'$ {line}')
                print(p.shell(line, timeout_ms=10000) or '')
            return
        if a.cmd == 'repl':
            c = Card(p, log_dir)
            return repl(p, c)
        c = Card(p, log_dir)
        if a.cmd == 'ls':
            ns = argparse.Namespace(path=a.args[0] if a.args else 'A:\\', max_entries=500, raw_entries=0, log_dir=log_dir)
            zp.stage_ls(p, ns)
        elif a.cmd == 'stat':
            print(a.args[0], c.size(a.args[0]), 'B')
        elif a.cmd == 'get':
            print(c.get(a.args[0], a.args[1]), 'B')
        elif a.cmd == 'put':
            print(c.put(a.args[0], a.args[1], a.replace), 'B written and checked')
        elif a.cmd == 'rm':
            c.remove(a.args[0])
        elif a.cmd == 'mkdir':
            c.mkdir(a.args[0])
        elif a.cmd == 'fwpush2':
            if not a.old or not a.args:
                raise SystemExit('fwpush2 NEW.bin --old OLD.bin')
            c.fwpush2(a.args[0], a.old, a.card, a.keep)   # never leave Z_8_PREV.bin by default (updater reflashes it)
        elif a.cmd == 'sha':
            path = a.args[0] if a.args else a.card
            d = c.sha256(path)
            if len(a.args) > 1:
                ok = d == hashlib.sha256(Path(a.args[1]).read_bytes()).digest()
                print('matches' if ok else 'DIFFERS from', a.args[1])
        elif a.cmd == 'repair':
            c.repair(a.args[0], a.card)
        elif a.cmd == 'fwpush':
            if not a.old or not a.args:
                raise SystemExit('fwpush NEW.bin --old OLD.bin')
            c.fwpush(a.args[0], a.old, a.card, not a.no_keep, a.samples, a.resume)
        elif a.cmd == 'speed':
            n, t0 = 100, time.monotonic()
            for i in range(n):
                p.debug.read(c.buf + (i % 64) * CHUNK, CHUNK)
            dt = time.monotonic() - t0
            print(f'RTOS read: {n / dt:.1f} transactions/s = {n * CHUNK / dt / 1024:.1f} KiB/s')
            t0 = time.monotonic()
            for i in range(20):
                c.call(F_CLOSE, 0xffffffff)
            print(f'native call round trip: {(time.monotonic() - t0) / 20 * 1000:.0f} ms')
    finally:
        if c is not None:
            c.release()
        t.__exit__(None, None, None)


def repl(p, c):
    print('native shell; prefix : for tools (:ls PATH, :get PATH OUT, :put LOCAL PATH, :rm PATH, '
          ':peek ADDR N, :poke ADDR HEX, :q)')
    while True:
        try:
            line = input('z8> ').strip()
        except EOFError:
            break
        if not line:
            continue
        try:
            if line.startswith(':'):
                w = line[1:].split()
                if w[0] in ('q', 'quit'):
                    break
                if w[0] == 'ls':
                    zp.stage_ls(p, argparse.Namespace(path=w[1] if len(w) > 1 else 'A:\\', max_entries=500,
                                                      raw_entries=0, log_dir=c.log_dir))
                elif w[0] == 'get':
                    print(c.get(w[1], w[2]), 'B')
                elif w[0] == 'put':
                    print(c.put(w[1], w[2], replace=True), 'B')
                elif w[0] == 'rm':
                    c.remove(w[1])
                elif w[0] == 'peek':
                    d = c.mem_read(int(w[1], 0), int(w[2], 0))
                    for o in range(0, len(d), 16):
                        print(f'{int(w[1], 0) + o:#x}  {d[o:o + 16].hex(" ")}')
                elif w[0] == 'poke':
                    c.mem_write(int(w[1], 0), bytes.fromhex(w[2]))
                else:
                    print('unknown tool command')
            else:
                print(p.shell(line, timeout_ms=10000) or '')
        except SystemExit as e:
            print('error:', e)


if __name__ == '__main__':
    main()
