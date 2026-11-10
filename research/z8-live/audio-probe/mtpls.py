#!/usr/bin/env python3
"""Read-only listing of what the Z8's stock MTP/PTP service exposes (Windows WPD).
  python mtpls.py [--depth 3] [--get NAME --out DIR]
--get copies one object (matched by file name) through the stock WPD/MTP path and reports speed."""
import argparse
import ctypes as c
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import z8_audio_probe as zp  # noqa: E402

OBJ = '{EF6B490D-5CD8-437A-AFFC-DA8B60EE4A3C}'
RES = '{E81E79BE-34F0-41BF-B53F-F1A06AE87842}'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--depth', type=int, default=3)
    ap.add_argument('--get')
    ap.add_argument('--out', default=str(HERE / 'mtp-pulled'))
    a = ap.parse_args()
    log = zp.Log(HERE / 'logs', 'mtpls')
    t = zp.open_transport(zp.find_host(None), log)
    t.__enter__()
    try:
        content = t.device.Content()
        props = content.Properties()
        keys = t.client.CreateObject(t.types.PortableDeviceKeyCollection, interface=t.api.IPortableDeviceKeyCollection)
        K = {n: t.key(pid, OBJ) for n, pid in (('name', 4), ('file', 12), ('size', 11), ('ctype', 7))}
        for k in K.values():
            keys.Add(c.byref(k))

        def info(oid):
            v = props.GetValues(oid, keys)
            out = {}
            for n, k in K.items():
                try:
                    if n == 'size':
                        out[n] = v.GetUnsignedLargeIntegerValue(c.byref(k))
                    elif n == 'ctype':
                        out[n] = str(v.GetGuidValue(c.byref(k)))
                    else:
                        out[n] = v.GetStringValue(c.byref(k))
                except Exception:
                    pass
            return out

        def children(oid):
            # comtypes maps Next(n) to (first id, fetched); fetch one at a time.
            e = content.EnumObjects(0, oid, None)
            ids = []
            while True:
                r = e.Next(1)
                if not r or r[1] == 0 or not r[0]:
                    break
                ids.append(r[0])
            return ids

        found = {}

        def walk(oid, depth, pad):
            for ch in children(oid):
                i = info(ch)
                name = i.get('file') or i.get('name') or ch
                folder = i.get('ctype', '').upper().startswith('{27E2E392')
                print(f"{pad}{name}  size={i.get('size', '')}  id={ch}{'  <folder>' if folder else ''}")
                found[name.upper()] = (ch, i)
                if depth > 0 and (folder or not i.get('size')):
                    walk(ch, depth - 1, pad + '  ')

        walk('DEVICE', a.depth, '')
        if a.get:
            hit = found.get(a.get.upper())
            if not hit:
                raise SystemExit(f'{a.get} not exposed over MTP (walk deeper with --depth?)')
            oid, i = hit
            res = content.Transfer()
            rk = t.key(0, RES)
            r = res.GetStream(oid, c.byref(rk), 0, 0)  # STGM_READ; returns (stream, optimal size)
            print('GetStream ->', [type(x).__name__ for x in (r if isinstance(r, (tuple, list)) else [r])])
            items = r if isinstance(r, (tuple, list)) else [r]
            stream = next(x for x in items if hasattr(x, 'RemoteRead') or hasattr(x, 'Read'))
            chunk = 262144
            out = Path(a.out); out.mkdir(parents=True, exist_ok=True)
            dst = out / a.get
            t0 = time.monotonic(); n = 0
            with dst.open('wb') as f:
                while True:
                    data, got = stream.RemoteRead(chunk)  # comtypes: (buffer, bytes read)
                    if not got:
                        break
                    f.write(bytes(data[:got])); n += got
            dt = time.monotonic() - t0
            print(f'copied {n} B in {dt:.1f} s = {n / dt / 1e6:.1f} MB/s -> {dst}')
    finally:
        t.__exit__(None, None, None)


if __name__ == '__main__':
    main()
