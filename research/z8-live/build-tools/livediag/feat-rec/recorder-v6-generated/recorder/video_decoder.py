#!/usr/bin/env python3
"""Decode lamp-v6 phone video using an explicit pixel ROI, or timestamped CSV.
Requires PyAV/numpy only for video. No device, USB, or camera access.
"""
import argparse
import bisect
import csv
import json
import math
from pathlib import Path

STAGES = 'IRSMD'
HALF = .050

def crc4(bits):
    crc = 0
    for bit in bits:
        feedback = (crc >> 3) ^ bit
        crc = ((crc << 1) & 15) ^ (3 if feedback else 0)
    return crc

def integer(bits):
    v = 0
    for b in bits:
        v = (v << 1) | b
    return v

def payload_length(header, short=False):
    if short:
        if integer(header[:3]) != 4:
            raise ValueError('reserved short stage')
        return 14
    stage, code = integer(header[:2]), integer(header[2:5])
    if stage < 2:
        if code > (3 if stage == 1 else 0):
            raise ValueError('reserved I/R code')
        return 9
    if stage == 2:
        if code == 7:
            raise ValueError('reserved S code')
        return 41 if code == 6 else 9
    if code not in (0, 1, 2, 6):
        raise ValueError('reserved M code')
    return 17 if code == 6 else 9

def parse_bits(bits, short=False):
    n = payload_length(bits[:5], short)
    if len(bits) != n:
        raise ValueError('truncated payload')
    stage, code = integer(bits[:2]), integer(bits[2:5])
    value = integer(bits[5:-4]) if code == 6 else (255 if stage == 3 and code == 2 else code)
    # Reject noncanonical escapes as well as reserved codes.
    canonical = not (code == 6 and ((stage == 2 and value <= 5) or
                                  (stage == 3 and value in (0, 1, 255))))
    if short:
        stage, code, value = 4, 0, integer(bits[3:-4])
        canonical = value >> 2 in (1, 2, 10, 11, 13, 15, 17, 18)
    expected, received = crc4(bits[:-4]), integer(bits[-4:])
    return dict(stage=STAGES[stage], stage_number=stage, close_stage=(value >> 2) if short else None, nr_mask=(value & 3) if short else None, value=value, value_hex=hex(value), code=code,
                crc_received=received, crc_expected=expected, crc_ok=received == expected,
                accepted=canonical and received == expected,
                error=None if canonical and received == expected else 'CRC mismatch' if canonical else 'noncanonical escape')

def decode_samples(samples, threshold=None, half=HALF, tolerance=.026, max_gap=.045):
    """Edge/run-length decoder; never search for a timing that makes CRC pass.
    samples = [(PTS seconds, ROI mean brightness), ...]. Rejected sync/payload
    candidates are retained. Long dark tails are allowed only after the last bit.
    """
    if not all(math.isfinite(v) for v in (half,tolerance,max_gap)) or half <= 0 or not 0 < tolerance < half or max_gap <= 0:
        raise ValueError('invalid timing parameters')
    if len(samples) < 2:
        raise ValueError('need at least two frames')
    times, brightness = map(list, zip(*samples))
    if not all(math.isfinite(t) and math.isfinite(v) for t, v in samples):
        raise ValueError('nonfinite sample')
    if any(b <= a for a, b in zip(times, times[1:])):
        raise ValueError('PTS must be strictly increasing')
    values = sorted(brightness)
    # Extrema allow a short frame within a long recording; explicit threshold
    # is preferable when unrelated ROI motion dominates the brightness range.
    lo, hi = values[0], values[-1]
    threshold = (lo+hi)/2 if threshold is None else threshold
    if not math.isfinite(threshold):
        raise ValueError('nonfinite threshold')
    levels = [int(v >= threshold) for v in brightness]
    meta = dict(threshold=threshold, minimum=lo, maximum=hi, half_ms=half*1000,
                max_frame_gap_s=max(b-a for a, b in zip(times, times[1:])))
    if hi-lo < 8:
        return [], levels, dict(meta, warning='insufficient ROI contrast')
    edges = [(times[i-1]+times[i])/2 for i in range(1, len(times)) if levels[i] != levels[i-1]]
    boundaries = [times[0], *edges, times[-1]]
    runs = [(levels[0] ^ (i & 1), boundaries[i], boundaries[i+1]) for i in range(len(boundaries)-1)]
    frames = []
    base_half, base_tolerance = half, tolerance
    for j in range(1, len(runs)-1):
        level, start, end = runs[j]
        short = abs((end-start)-.125) <= .018
        half = .025 if short else base_half
        tolerance = .012 if short else base_tolerance
        sync_low, sync_high, sync_tail = (.100, .125, .075) if short else (.200, .250, .150)
        if not level or runs[j-1][2]-runs[j-1][1] < sync_low-tolerance:
            continue
        row = dict(sync_rise_s=start, timestamp_s=start-sync_low,
                   payload_start_s=end+sync_tail, end_s=None, bits='', crc_ok=None, accepted=False)
        frames.append(row)
        if abs((end-start)-sync_high) > tolerance*1.5:
            row['error'] = 'sync high duration (ordinary long light is not a frame)'
            continue
        halves, bits, needed = [], [], None
        cursor = j+1
        try:
            while cursor < len(runs):
                lev, a, b = runs[cursor]
                duration = b-a
                if cursor == j+1:
                    duration -= sync_tail
                # Once length is known, a final low half can merge into idle.
                remaining = needed*2-len(halves) if needed is not None else None
                units = max(0, int(math.floor(duration/half+.5)))
                tail = remaining is not None and lev == 0 and remaining <= 2 and duration >= remaining*half-tolerance
                if tail:
                    units = remaining
                elif abs(duration-units*half) > tolerance:
                    raise ValueError('pulse timing')
                if cursor == j+1:
                    if units not in (0, 1):
                        raise ValueError('sync low duration')
                elif units not in (1, 2):
                    raise ValueError('illegal Manchester run')
                segment_start = a + (sync_tail if cursor == j+1 else 0)
                for u in range(units):
                    halves.append(lev)
                    if len(halves) % 2 == 0:
                        pair = halves[-2:]
                        if pair not in ([1, 0], [0, 1]):
                            raise ValueError('illegal Manchester pair')
                        bits.append(pair[0] == 0)
                        row['bits'] = ''.join(str(int(v)) for v in bits)
                        if len(bits) == 5:
                            needed = payload_length(bits, short)
                        if needed is not None and len(bits) == needed:
                            finish = segment_start+(u+1)*half if tail else b
                            # Unobserved samples are not invented to repair bits.
                            ia = max(0, bisect.bisect_left(times, start)-1)
                            iz = min(len(times)-1, bisect.bisect_right(times, finish))
                            if any(times[k+1]-times[k] > max_gap for k in range(ia, iz)):
                                raise ValueError('video sample gap')
                            row.update(parse_bits(bits, short), end_s=finish)
                            break
                if needed is not None and len(bits) == needed:
                    break
                cursor += 1
            else:
                raise ValueError('truncated frame')
        except ValueError as error:
            row.update(error=str(error), accepted=False)
    return frames, levels, meta

def video_samples(path, roi):
    """PTS/time_base from every decoded frame; encoded orientation, no FPS guess."""
    import av
    x, y, w, h = roi
    if min(x, y) < 0 or min(w, h) <= 0:
        raise ValueError('ROI must be x>=0 y>=0 width>0 height>0')
    samples = []
    with av.open(str(path)) as container:
        stream = container.streams.video[0]
        for frame in container.decode(stream):
            if frame.pts is None or frame.time_base is None:
                raise ValueError('video lacks frame PTS; no synthetic FPS timestamps')
            pixels = frame.to_ndarray(format='gray')
            if x+w > pixels.shape[1] or y+h > pixels.shape[0]:
                raise ValueError('ROI exceeds encoded frame bounds')
            samples.append((float(frame.pts*frame.time_base), float(pixels[y:y+h, x:x+w].mean())))
    return samples

def write_outputs(prefix, samples, frames, levels, meta):
    brightness_path = Path(str(prefix)+'.brightness.csv')
    frames_path = Path(str(prefix)+'.frames.json')
    with brightness_path.open('w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['frame', 'timestamp_s', 'brightness', 'level'])
        for i, ((t, v), level) in enumerate(zip(samples, levels)):
            writer.writerow([i, f'{t:.9f}', f'{v:.6f}', level])
    frames_path.write_text(json.dumps(dict(protocol='lamp-v6', **meta, frames=frames,
        limitation='CRC-valid frames are positive arrival evidence only; missing/corrupted frames do not prove nonarrival.'), indent=2)+'\n')
    return brightness_path, frames_path

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('input', type=Path)
    p.add_argument('--roi', type=int, nargs=4, metavar=('X', 'Y', 'WIDTH', 'HEIGHT'))
    p.add_argument('--sequence', action='store_true', help='CSV with timestamp_s,brightness columns')
    p.add_argument('--threshold', type=float)
    p.add_argument('--half-ms', type=float, default=50)
    p.add_argument('--out', type=Path, help='output prefix (default: input stem + .lamp)')
    a = p.parse_args()
    if a.sequence:
        with a.input.open(newline='') as f:
            samples = [(float(r['timestamp_s']), float(r['brightness'])) for r in csv.DictReader(f)]
    else:
        if a.roi is None:
            p.error('video requires --roi X Y WIDTH HEIGHT in encoded frame pixels')
        samples = video_samples(a.input, a.roi)
    frames, levels, meta = decode_samples(samples, a.threshold, half=a.half_ms/1000)
    prefix = a.out or a.input.with_suffix('.lamp')
    if a.input.resolve() in [Path(str(prefix)+suffix).resolve() for suffix in ('.brightness.csv','.frames.json')]:
        p.error('output must not overwrite input; choose a different --out prefix')
    paths = write_outputs(prefix, samples, frames, levels,
                          dict(meta, input=str(a.input), roi=a.roi, timestamp_basis='source PTS seconds'))
    print(json.dumps(dict(outputs=list(map(str, paths)), accepted=sum(r['accepted'] for r in frames),
                         candidates=len(frames)), indent=2))

if __name__ == '__main__':
    main()
