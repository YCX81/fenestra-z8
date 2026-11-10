#!/usr/bin/env python3
"""Z8 photo-mode microphone probe over Fenestra USB Debug (FXP2 / RTOS monitor).

Drives the stock factory `audiodbg` shell command (AudioApplicationServiceSoundTest.c,
C3.11 MAIN sha 2b43a31c...) through Fenestra's native-shell job, and watches a few
audio-service globals with plain memory reads. Nothing is written to the camera
except through `audiodbg` itself.

Stages (run in order, stop at the first surprise):
  check    read-only handshake, registers, globals; shell `echo` and `help` (must list
           audiodbg); then toggle audiodbg's debug-only depth flag (code 21) and read it
           back (skip with --read-only)
  watch    read-only: poll the audio globals for N seconds
  record   `audiodbg 1 1 <sub>` (start) ... `audiodbg 1 0 <sub>` (stop), then
           `audiodbg 20 <n>` writes A:\\TEST<n>.WAV; stop is always attempted once started
  pull     read the raw sound-test buffer over USB (--seconds of it) to see the
           real DMA sample packing; checks every page is mapped first
  analyze  host-side WAV statistics (no camera)

Static facts used (stock C3.11, see memory z8-audiodbg-soundtest):
  audio service state u32       0xa02d4cb48  (getter 0x40c83908; start refuses 3 and 5)
  sound-test buffer pointer u64 0xa038fef78  (0x15f9000 B, allocated on first start)
  depth flag set by code 21     0xa038ff694  (copied to 0xa038ff690 at start; 0=16-bit WAV)
  WAV file number u16           0xa02920560  (code 20 argument, 1..9999)
"""
import argparse
import json
import os
import struct
import sys
import time
import wave
from pathlib import Path

STATE = 0xa02d4cb48
BUFFER_PTR = 0xa038fef78
DEPTH_ARG = 0xa038ff694
DEPTH_LATCHED = 0xa038ff690
FILE_NO = 0xa02920560
# Discovery windows: every changed u32 is reported, to find capture progress counters.
WINDOWS = {'soundtest': (0xa038fef70, 0x740), 'audiosvc': (0xa02d4c5c0, 0x5a0)}
BUSY_STATES = (3, 5)
RTOS_OP = 7
UNKNOWN_COMMAND = ('not found', 'unknown', 'no such', 'invalid command', 'undefined')


def find_host(explicit):
    here = Path(__file__).resolve().parent
    for p in [explicit, os.environ.get('FENESTRA_HOST'), here.parent / 'fenestra-z8' / 'host',
              Path.home() / 'Nikon' / 'fenestra-z8' / 'host']:
        if p and (Path(p) / 'ptp_rtos.py').is_file():
            return Path(p)
    sys.exit('Fenestra host tools not found: pass --fenestra-host <fenestra>/host or set FENESTRA_HOST')


class Log:
    def __init__(self, directory, stage):
        directory.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime('%Y%m%d-%H%M%S') + f'.{int(time.time() * 1000) % 1000:03d}-{os.getpid()}'
        self.path = directory / f'{stamp}-{stage}.jsonl'
        self.stream = self.path.open('x')
        self.t0 = time.monotonic()

    def __call__(self, event, **data):
        row = dict(t=round(time.monotonic() - self.t0, 3), event=event, **data)
        self.stream.write(json.dumps(row, ensure_ascii=False) + '\n')
        self.stream.flush()
        return row


def open_transport(host_dir, log):
    """Native Fenestra transport, but identity is checked on FXP2 (C 3.11), not on the
    PTP DeviceInfo version, which the repacked update container changes (04.05.p0)."""
    sys.path.insert(0, str(host_dir))
    import ptp_contract

    def identify(model, version, profile='auto'):
        if model not in ('Z 8', 'Z8'):
            raise ValueError(f'not a Z8: model={model!r}')
        log('device-info', model=model, version=version)
        return 'z8-c311'
    ptp_contract.identify = identify
    if sys.platform == 'win32':
        import ptp_wpd
        ptp_wpd.identify = identify
        return ptp_wpd.WpdPtp(profile='auto')
    import ptp_usb
    ptp_usb.identify = identify
    if sys.platform == 'darwin':
        import usb.core
        original = usb.core.Device.is_kernel_driver_active

        def no_kernel_driver(self, interface):
            # macOS has no detachable kernel PTP driver; ptpcamerad is a user process.
            try:
                return original(self, interface)
            except (NotImplementedError, usb.core.USBError):
                return False
        usb.core.Device.is_kernel_driver_active = no_kernel_driver
    return ptp_usb.UsbPtp(profile='auto')


class Probe:
    def __init__(self, client, log, shell_timeout_ms):
        from ptp_rtos import RtosDebug
        self.client, self.log, self.shell_timeout_ms = client, log, shell_timeout_ms
        self.debug = RtosDebug(client)
        dbg = self.debug
        raw_read, raw_translate = dbg.read, dbg.translate
        def retried(fn):
            def call(*a):
                from ptp_client import PtpError
                for attempt in range(60):
                    try:
                        return fn(*a)
                    except PtpError as exc:
                        if exc.code != 0x2019 or attempt == 59:
                            raise
                        self.client.pending = None   # reads/translates are safe to resend
                        time.sleep(0.5)
            return call
        dbg.read, dbg.translate = retried(raw_read), retried(raw_translate)
        def read_range(address, count):
            data = bytearray()
            while len(data) < count:
                data += dbg.read(address + len(data), min(1008, count - len(data)))
            return bytes(data)
        self.debug.read_range = read_range

    def handshake(self):
        report = self.client.hello()
        self.log('hello', **{k: v for k, v in report.items() if k != 'identity'})
        if report.get('firmware') != 'C 3.11':
            raise SystemExit(f'Fenestra reports {report.get("firmware")}, expected C 3.11; addresses do not apply')
        if not report['capabilities'] & (1 << (RTOS_OP - 1)):
            raise SystemExit('this Fenestra build does not advertise the RTOS monitor (op 7)')
        model = report.get('model') or report.get('identity', {}).get('model', '')
        if model and 'Z8' not in model.replace(' ', ''):
            raise SystemExit(f'Fenestra identity model {model!r} is not a Z8')
        return report

    def u32(self, address):
        return struct.unpack('<I', self.debug.read(address, 4))[0]

    def u64(self, address):
        return struct.unpack('<Q', self.debug.read(address, 8))[0]

    def globals(self):
        return dict(state=self.u32(STATE), buffer=f'0x{self.u64(BUFFER_PTR):x}',
                    depth_arg=self.u32(DEPTH_ARG), depth_latched=self.u32(DEPTH_LATCHED),
                    file_no=struct.unpack('<H', self.debug.read(FILE_NO, 2))[0])

    def snapshot(self):
        out = {}
        for name, (base, size) in WINDOWS.items():
            data = bytearray()
            while len(data) < size:
                data += self.debug.read(base + len(data), min(1008, size - len(data)))
            out[name] = bytes(data)
        return out

    def diff(self, old, new, limit=16):
        changes = []
        for name, (base, size) in WINDOWS.items():
            a, b = old[name], new[name]
            for off in range(0, size, 4):
                x, y = a[off:off + 4], b[off:off + 4]
                if x != y:
                    changes.append(f'{base + off:#x}:{x[::-1].hex()}->{y[::-1].hex()}')
        return changes[:limit], len(changes)

    def shell(self, line, timeout_ms=None):
        timeout_ms = timeout_ms or self.shell_timeout_ms
        self.log('shell-submit', line=line, timeout_ms=timeout_ms)
        job = self.debug.submit_command(line, timeout_ms)
        try:
            result = self.debug.wait_job(job, timeout=timeout_ms / 1000 + 2)
        except TimeoutError as exc:
            # Never resubmit: the native command may still be running.
            self.log('shell-timeout', line=line, job=job, error=str(exc))
            raise
        text = result.pop('output').decode('utf-8', errors='replace')
        self.log('shell-done', line=line, job=job, state=result['state'], output=text,
                 restart_required=result['restart_required'])
        if result['phase'] == 3:
            self.debug.acknowledge(job)
        if result['state'] != 'done':
            raise RuntimeError(f'`{line}` ended {result["state"]}; output: {text!r}')
        # audiodbg returns silently for invalid codes; only shell-level errors print.
        if any(k in text.lower() for k in UNKNOWN_COMMAND):
            raise RuntimeError(f'native shell did not accept `{line}`: {text!r}')
        return text


def say(*parts):
    print(*parts, flush=True)


def stage_check(p, args):
    regs = p.debug.registers()
    p.log('registers', **regs)
    say('registers:', json.dumps(regs))
    g = p.globals()
    p.log('globals', **g)
    say('audio globals:', json.dumps(g))
    out = p.shell('echo fenestra-audio-probe')
    if 'fenestra-audio-probe' not in out:
        raise SystemExit(f'native shell echo did not come back (got {out!r}); stop here')
    say('native shell: ok')
    listing = p.shell('help')
    if 'audiodbg' not in listing:
        raise SystemExit('`help` does not list audiodbg (output in the log); stop here')
    say('help lists audiodbg')
    if args.read_only:
        say('--read-only: skipping the audiodbg registration test')
        return
    if g['state'] in BUSY_STATES:
        raise SystemExit(f'audio state {g["state"]} is busy (movie/playback?); switch to photo LV and retry')
    original = g['depth_arg']
    try:
        for value in (1, 0):
            text = p.shell(f'audiodbg 21 {value}')
            got = p.u32(DEPTH_ARG)
            p.log('depth-readback', wrote=value, read=got, output=text)
            if got != value:
                raise SystemExit(f'audiodbg 21 {value} did not reach {DEPTH_ARG:#x} (read {got}); '
                                 'command missing or address map differs; stop here')
    finally:
        if original not in (0, 1):
            p.shell(f'audiodbg 21 {original}')
    say('audiodbg registered and depth flag at', hex(DEPTH_ARG), 'verified (left at 16-bit)')
    say('check passed: next run `record` in photo live view')


def poll(p, seconds, label, prompts=()):
    base = p.snapshot()
    end = time.monotonic() + seconds
    pending = sorted(prompts)
    while True:
        now = time.monotonic()
        elapsed = seconds - (end - now)
        while pending and elapsed >= pending[0][0]:
            say(f'  >>> {pending.pop(0)[1]}')
        g = p.globals()
        snap = p.snapshot()
        changes, total = p.diff(base, snap)
        p.log(label, elapsed=round(elapsed, 2), changed=total, sample=changes, **g)
        say(f'  {label} +{elapsed:4.1f}s state={g["state"]} buf={g["buffer"]} changed={total} '
            + ' '.join(changes[:4]))
        base = snap
        if now >= end:
            return g
        time.sleep(args_interval)


def stage_watch(p, args):
    say(f'watching {args.seconds}s (read-only)')
    poll(p, args.seconds, 'watch')


def stage_record(p, args):
    if not 1 <= args.file_no <= 9:
        raise SystemExit('--file-no 1..9 (single digit avoids decimal/hex ambiguity in the native parser)')
    if not 0 <= args.sub <= 3:
        raise SystemExit('--sub 0..3 (the dispatcher rejects larger values)')
    if not args.yes:
        answer = input('Camera in PHOTO mode, live view on, card in slot A with space for 23 MB, '
                       'not recording/playing back? Type yes: ')
        if answer.strip().lower() != 'yes':
            raise SystemExit('aborted')
    before = p.globals()
    p.log('before', **before)
    say('before:', json.dumps(before))
    if before['state'] in BUSY_STATES:
        raise SystemExit(f'audio state {before["state"]} is busy; not starting')
    p.shell(f'audiodbg 21 {0 if args.depth == 16 else 1}')
    prompts = []
    if args.shutter_at is not None:
        prompts.append((args.shutter_at, 'press the shutter fully NOW (one frame), keep the camera still'))
    started = False
    try:
        say(f'start: audiodbg 1 1 {args.sub}')
        started = True  # mark before the job: a lost reply may still have started capture
        p.shell(f'audiodbg 1 1 {args.sub}')
        if args.unmute:
            for reg in (0x15, 0x16):
                say(f'  codec R{reg} <- {args.adc_volume:#x}: rc={native_call(p, CODEC_WRITE, (reg, args.adc_volume))}')
        poll(p, args.seconds, 'recording', prompts)
    finally:
        if started:
            say(f'stop: audiodbg 1 0 {args.sub}')
            try:
                p.shell(f'audiodbg 1 0 {args.sub}')
            except Exception as exc:
                p.log('stop-failed', error=repr(exc))
                say('!!! stop failed:', exc, '- turn USB Debug off; power-cycle the camera if audio stays on')
                raise
    after = p.globals()
    p.log('after-stop', **after)
    say('after stop:', json.dumps(after))
    poll(p, 1.0, 'settle')
    if args.no_write:
        say('--no-write: WAV not written')
        return
    say(f'write: audiodbg 20 {args.file_no} (A:\\TEST{args.file_no:04d}.WAV, ~23 MB)')
    p.shell(f'audiodbg 20 {args.file_no}', timeout_ms=args.write_timeout_ms)
    say(f'done. Copy TEST{args.file_no:04d}.WAV from the slot-1 card root, then:')
    say(f'  python3 {Path(__file__).name} analyze TEST{args.file_no:04d}.WAV')


def stage_pull(p, args):
    """Read the raw sound-test buffer back over USB (no card write), to see the real
    sample packing. Only after `record`; the pointer stays valid after stop."""
    if args.address is not None:  # e.g. the 5-block RX DMA ring
        base, size = args.address, args.count
    else:
        base = p.u64(BUFFER_PTR)
        if not base:
            raise SystemExit('sound-test buffer pointer is 0: run `record` first')
        size = min(int(args.seconds * 48000 * 8), 0x15f9000)  # up to 8 B/frame (s32 stereo)
    for page in range(base, base + size, 0x1000):
        if p.debug.translate(page)['fault']:
            raise SystemExit(f'{page:#x} is not mapped; not reading (a fault would stop the camera)')
    out = args.log_dir / f'buffer-{base:x}-{size}.raw'
    say(f'pulling {size} B from {base:#x} -> {out}')
    t0 = time.monotonic()
    with out.open('xb') as stream:
        done = 0
        while done < size:
            data = p.debug.read(base + done, min(1008, size - done))
            stream.write(data)
            done += len(data)
            if done % (256 * 1008) < 1008:
                say(f'  {done}/{size} B, {done / (time.monotonic() - t0) / 1024:.0f} KiB/s')
    p.log('pull', base=f'0x{base:x}', bytes=size, output=str(out), seconds=round(time.monotonic() - t0, 1))
    raw = out.read_bytes()
    for name, fmt, width in (('s16 stereo', '<h', 2), ('s32 stereo', '<i', 4)):
        n = min(len(raw) // width, 96000)
        values = [struct.unpack_from(fmt, raw, i * width)[0] for i in range(n)]
        low = sum(1 for v in values if v & 0xff == 0) / max(n, 1)
        say(f'  as {name}: nonzero {sum(1 for v in values if v)}/{n}, '
            f'max |v| {max(map(abs, values)) if values else 0}, low byte zero {low:.0%}')
    say('  (s32 with ~100% low-byte zero = left-aligned 24-bit; first words in hex:)',
        raw[:32].hex(' ', 4))


CODEC_WRITE = 0x40c7f824  # (reg, value): raw WM8962 write used by audiocntdbg's modify helper
RING = 0xa170fe400


def native_call(p, address, call_args, timeout_ms=5000):
    p.log('call-submit', address=f'0x{address:x}', args=[hex(a) for a in call_args])
    from ptp_client import BackendError
    try:
        job = p.debug.submit_call(address, call_args, timeout_ms)
    except BackendError:
        # a finished job from an interrupted run still occupies the monitor: acknowledge it
        # (a job that is still running is left alone) and submit once more
        p.client.pending = None
        st = p.debug.job_status()
        if st['phase'] not in (3, 4, 5):
            raise
        try:
            p.debug.acknowledge(st['job'])
        except BackendError:
            p.client.pending = None
        p.log('stale-job-acked', job=st['job'], phase=st['phase'])
        job = p.debug.submit_call(address, call_args, timeout_ms)
    result = p.debug.wait_job(job, timeout=timeout_ms / 1000 + 2)
    result.pop('output')
    p.log('call-done', job=job, state=result['state'], result=result['result'])
    if result['phase'] == 3:
        # the job already ran: only the acknowledge is retried on a busy camera (never the call)
        from ptp_client import PtpError, BackendError
        for attempt in range(40):
            try:
                p.debug.acknowledge(job)
                break
            except BackendError:
                if not attempt:
                    raise
                break   # an earlier attempt landed; its reply was the one lost
            except PtpError as exc:
                if exc.code != 0x2019 or attempt == 39:
                    raise
                p.client.pending = None
                time.sleep(0.25)
    if result['state'] != 'done':
        raise RuntimeError(f'call {address:#x} ended {result["state"]}')
    return result['result']


def stage_adc_test(p, args):
    """Photo mode: start the sound test, un-mute the WM8962 ADC digital volume
    (R21/R22 = 0x1C0: 0 dB + VU, the movie-standby value), then read the RX ring."""
    if not args.yes and input('Camera in PHOTO live view? Type yes: ').strip().lower() != 'yes':
        raise SystemExit('aborted')
    if p.u32(STATE) in BUSY_STATES:
        raise SystemExit('audio state busy (movie mode?); switch to photo first')
    p.shell('audiodbg 21 0')
    started = False
    try:
        started = True
        p.shell(f'audiodbg 1 1 {args.sub}')
        say('sound test started, state', p.u32(STATE))
        for reg in (0x15, 0x16):
            rc = native_call(p, CODEC_WRITE, (reg, args.adc_volume))
            say(f'  codec R{reg} <- {args.adc_volume:#x}: rc={rc}')
        time.sleep(args.seconds)
        raw = bytearray()
        while len(raw) < 32768:
            raw += p.debug.read(RING + len(raw), 1008)
        values = struct.unpack('<%dh' % (len(raw) // 2), bytes(raw[:len(raw) // 2 * 2]))
        nonzero = sum(1 for v in values if v)
        p.log('adc-test-ring', nonzero=nonzero, total=len(values), head=raw[:64].hex())
        say(f'ring first 32 KiB as s16: nonzero {nonzero}/{len(values)}, max |v| {max(map(abs, values))}')
        say('first words:', raw[:32].hex(' ', 4))
    finally:
        if started:
            p.shell(f'audiodbg 1 0 {args.sub}')
            say('stopped, state', p.u32(STATE))


HEAP_ALLOC = 0x40d2c408   # (bytes, 0) -> pointer; the allocator Live v4 uses
POST = 0x40c80988         # audio-driver message post, bit0 = queued
ST_CALLBACK = 0x40e3acbc  # stock SoundTest block callback(src, bytes)
ST_COUNTER = 0xa038ff3d0  # its block counter (copies into the 23 MB buffer while < 0x78)
RING_DESCRIPTOR = 0xa031f8bb8
# Payload images from live_audio_z8.c (read back from the camera after audiodbg 1 1 0).
MSG_IMAGES = [
    (0x42040104, bytes([0, 0, 0, 0, 5, 0, 0, 0, 0, 0, 0, 0, 5]).ljust(0x20, b'\0')),
    (0x4204010b, bytes([0, 1, 0, 1, 1, 0, 0, 0])),
    (0x42040101, bytes([0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0])),
    (0x42040102, bytes([0] * 12 + [1, 0, 0, 0])),
    (0x42040109, None),  # ring + callback, built at run time
    (0x42040106, bytes([0, 0, 0, 0, 1, 1, 1, 0])),
]
STOP_IMAGE = (0x42040108, bytes([1, 0, 0, 0, 0, 0, 0, 0]))


def stage_msg_test(p, args):
    """Replay the live_audio_z8 start/stop message sequence from the host (own
    message buffers, no 23 MB SoundTest buffer), with the stock SoundTest callback
    standing in for live_audio_z8_block so callbacks are visible as its counter."""
    if not args.yes and input('Camera in PHOTO live view? Type yes: ').strip().lower() != 'yes':
        raise SystemExit('aborted')
    if p.u32(STATE) != 1:
        raise SystemExit(f'audio state {p.u32(STATE)} is not idle (1); switch to photo first')
    ring_base = p.u64(RING_DESCRIPTOR)
    ring_count = p.u32(RING_DESCRIPTOR + 0x10)
    if not ring_base or not 2 <= ring_count <= 8:
        raise SystemExit(f'unexpected ring descriptor {ring_base:#x}/{ring_count}')
    base = native_call(p, HEAP_ALLOC, (0x240, 0))
    if not base:
        raise SystemExit('heap allocation failed')
    say(f'scratch {base:#x} (0x240 B, not freed), ring {ring_base:#x} x{ring_count}')
    ring = struct.pack('<IIIIB7xQQQ', ring_count, 0x17700, 0, 0x5dc0, 1, ring_base, args.callback, 0)
    payload_at, blob, msgs = base + 0x190, bytearray(), []
    for i, (msg_id, image) in enumerate(MSG_IMAGES + [STOP_IMAGE]):
        image = ring if image is None else image
        msgs.append(struct.pack('<3QQIIQQ', 0, 0, 0, 0, msg_id, 0, payload_at + len(blob), 0))
        blob += image + b'\0' * (-len(image) % 8)
    image = b''.join(msgs).ljust(0x190, b'\0') + bytes(blob)
    for off in range(0, len(image), 1008):
        p.debug.write(base + off, image[off:off + 1008])
    if bytes(p.debug.read(base, min(len(image), 1008))) != image[:1008]:
        raise SystemExit('scratch readback differs')
    p.log('msg-test-image', base=f'0x{base:x}', hex=image.hex())
    counter0 = p.u32(ST_COUNTER)
    posted = 0
    try:
        for i, (msg_id, _) in enumerate(MSG_IMAGES):
            ok = native_call(p, POST, (base + i * 0x38,))
            say(f'  post {msg_id:#x}: {ok}')
            posted += 1
            if not ok & 1:
                raise RuntimeError(f'post {msg_id:#x} refused')
        deadline = time.monotonic() + 3
        while p.u32(ST_COUNTER) == counter0 and time.monotonic() < deadline:
            time.sleep(0.1)
        counter1 = p.u32(ST_COUNTER)
        say(f'state {p.u32(STATE)}, callback counter {counter0} -> {counter1}')
        if counter1 == counter0:
            raise RuntimeError('no callback within 3 s')
        for reg in (0x15, 0x16):
            say(f'  codec R{reg} <- {args.adc_volume:#x}: rc={native_call(p, CODEC_WRITE, (reg, args.adc_volume))}')
        time.sleep(args.seconds)
        counter2 = p.u32(ST_COUNTER)
        say(f'callback counter after {args.seconds}s: {counter2} ({counter2 - counter1} blocks)')
        for name, addr in (('ring', ring_base),
                           ('callback copy', p.u64(BUFFER_PTR) + 0x17700 * max(counter2 - 1, 0))):
            raw = bytearray()
            while len(raw) < 32768:
                raw += p.debug.read(addr + len(raw), 1008)
            v = struct.unpack('<%dh' % (len(raw) // 2), bytes(raw))
            nz = sum(1 for x in v if x)
            p.log('msg-test-data', where=name, nonzero=nz, peak=max(map(abs, v)))
            say(f'  {name} {addr:#x}: nonzero {nz}/{len(v)}, peak {max(map(abs, v))}')
    finally:
        if posted:
            ok = native_call(p, POST, (base + 6 * 0x38,))
            deadline = time.monotonic() + 3
            while p.u32(STATE) != 1 and time.monotonic() < deadline:
                time.sleep(0.1)
            for reg in (0x15, 0x16):
                native_call(p, CODEC_WRITE, (reg, 0x100))
            say(f'stop posted ({ok}), state {p.u32(STATE)}, ADC volume back to mute')


#  Wave5 driver RAM (w5-init static map). RAM only: Wave5 MMIO (0x16010000) and
#  the power/reset blocks are never read, since a clock-gated block can hang the bus.
W5_WINDOWS = {
    'vdi_state': (0xa02d3d928, 0x100), 'hevc_service': (0xa02d30060, 0xc0),
    'driver_request': (0xa02d32720, 0x40), 'encoder_instances': (0xa02d308f0, 5 * 0xf0),
    'movie_resources': (0xa046135a8, 0x140), 'movrs_object': (0xa031b5f38, 0x100),
    'movrs_buffers': (0xa031b5ec8, 0x70), 'adapter_state': (0xa02e98e00, 0x80),
    'board_handles': (0xa02e98488, 0x18), 'microcode_staging': (0xa02ede828, 0x10),
    'power_event': (0xa02c98644, 0x10), 'unknown_497d028': (0xa0497d028, 0x40),
    # service open 0x40c32fec: +0x10 VDI up, +0x18/+0x24 stream slots {used, coreA, coreB},
    # +0x30/+0x34 core A/B busy, +0x38 saved bring-up cfg (+0x10 size, +0x18 arena)
    'svc_slots': (0xa02d30100, 0x80), 'client_map': (0xa02e98480, 8),
}


W5_VDI_BASE = 0xa02d3d928 + 0x58   # holds 0x16010000 once the Wave5 driver is up


def w5_marks(p):
    """Cheap per-tick view: VDI register base, audio state, first word of instance 0."""
    return (p.u64(W5_VDI_BASE), p.u32(STATE), p.u64(0xa02d308f0))


def stage_snap(p, args):
    """Read-only dump of the Wave5 driver RAM windows to logs/w5-<label>.json.
    With --arm: poll until a recording brings Wave5 up, snap <label>-rec (and -rec2
    3 s later), then wait for it to go down again (or --stop-timeout) and snap <label>-after."""
    if not args.label:
        raise SystemExit('snap needs --label (e.g. photo / movie)')
    if not args.arm:
        return snap_once(p, args.log_dir, args.label)
    for page in (W5_VDI_BASE & ~0xfff, STATE & ~0xfff, 0xa02d308f0 & ~0xfff):
        if p.debug.translate(page)['fault']:
            raise SystemExit(f'{page:#x} not mapped')
    base = w5_marks(p)
    say(f'armed: vdi+0x58 {base[0]:#x}, audio {base[1]}, inst0 {base[2]:#x}; start recording now')
    trace, t0, last = [], time.monotonic(), base
    def tick():
        nonlocal last
        m = w5_marks(p)
        if m != last:
            trace.append((round(time.monotonic() - t0, 2), *[f'{v:#x}' for v in m]))
            say(f'{trace[-1][0]:7.2f}s vdi {m[0]:#x} audio {m[1]} inst0 {m[2]:#x}')
            last = m
        time.sleep(args.interval)
        return m
    while True:   # start = driver base set, or the audio service leaves its standby state
        m = tick()
        if m[0] or m[1] != base[1]:
            break
        if time.monotonic() - t0 > 600:
            raise SystemExit('Wave5 never came up within 10 min')
    snap_once(p, args.log_dir, f'{args.label}-rec')
    time.sleep(3)
    snap_once(p, args.log_dir, f'{args.label}-rec2')
    rec, up = tick(), time.monotonic()
    while True:   # stop = driver base cleared or audio service left its recording state
        m = tick()
        if not m[0] or m[1] != rec[1]:
            break
        if time.monotonic() - up > args.stop_timeout:
            say(f'no stop seen in {args.stop_timeout:.0f} s, snapping anyway')
            break
    # keep watching briefly so a staged teardown settles before the final read
    end = time.monotonic() + 3
    while time.monotonic() < end:
        tick()
    snap_once(p, args.log_dir, f'{args.label}-after')
    (args.log_dir / f'w5-{args.label}-trace.json').write_text(json.dumps(trace, indent=1))
    p.log('snap-trace', trace=trace)


def snap_once(p, log_dir, label):
    out = {'audio_state': p.u32(STATE)}
    for name, (base, size) in W5_WINDOWS.items():
        for page in range(base & ~0xfff, base + size, 0x1000):
            if p.debug.translate(page)['fault']:
                raise SystemExit(f'{name}: {page:#x} not mapped')
        data = bytearray()
        while len(data) < size:
            data += p.debug.read(base + len(data), min(1008, size - len(data)))
        out[name] = {'address': f'0x{base:x}', 'hex': data.hex()}
    path = log_dir / f'w5-{label}.json'
    path.write_text(json.dumps(out, indent=1))
    p.log('snap', label=label, path=str(path))
    say(f'wrote {path} ({len(W5_WINDOWS)} windows, audio state {out["audio_state"]})')


HEAP_TABLE, HEAP_FREE = 0xa03203dc8, 0x40d2c448   # 2 x {handle, size @+8, base @+0x10}; free(ptr)
POOL_ALLOC, POOL_FREE, POOL_GATE = 0x4104d2dc, 0x4104d600, 0xa03f31a68  # Live v4 pool ABI


def stage_heap_test(p, args):
    """How large a block each allocator hands out right now. Every block is freed
    immediately; sizes double until the first failure."""
    caps = []
    for i in range(2):
        handle, size, base = struct.unpack_from('<QQQ', p.debug.read(HEAP_TABLE + i * 0x20, 0x18))
        caps.append((size & 0xffffffff) // 4)
        say(f'heap {i}: handle {handle:#x} base {base:#x} size {size & 0xffffffff:#x} '
            f'({(size & 0xffffffff) / 2**20:.1f} MB)')
    sizes = [1 << n for n in range(20, 30)]   # 1 MB .. 512 MB
    for i in range(2):   # the RTOS heaps are only a few MB: stay at a quarter so other tasks never starve
        for n in [1 << k for k in range(16, 30) if 1 << k <= caps[i]]:
            ptr = native_call(p, HEAP_ALLOC, (n, i))
            p.log('heap-alloc', heap=i, bytes=n, ptr=f'0x{ptr:x}')
            if not ptr:
                say(f'heap {i}: {n >> 10} KB failed')
                break
            native_call(p, HEAP_FREE, (ptr,))
            say(f'heap {i}: {n >> 10} KB ok at {ptr:#x}, freed')
    gate = p.u32(POOL_GATE)
    if gate:
        say(f'pool admission gate {gate:#x} is closed; pool test skipped')
        return
    for n in sizes:
        d = native_call(p, POOL_ALLOC, (0x4c4155, 0x60, n))
        if not d:
            say(f'pool: {n >> 20} MB failed')
            break
        r, cls = struct.unpack_from('<QI', p.debug.read(d + 8, 12))   # class at +0x10 (0x24 expected)
        cpu, actual = struct.unpack('<QI', p.debug.read(r, 12)) if r else (0, 0)
        native_call(p, POOL_FREE, (d,))
        p.log('pool-alloc', bytes=n, desc=f'0x{d:x}', cls=cls, cpu=f'0x{cpu:x}', actual=actual)
        say(f'pool: {n >> 20} MB -> class {cls:#x}, {actual / 2**20:.1f} MB at {cpu:#x}, freed')


W5_MEMCALC = 0x40c341ec   # (in 0x1c, out 0x30) -> 0 ok / 4 invalid; pure arithmetic (0x40c3924c)
W5_PLAN_FIELDS = ('code', 'temp', 'work', 'fb', 'fb_count', 'fb2', 'fb2_count', 'stride',
                  'bitstream', 'mv', 'fbc_y', 'fbc_c')


def w5_plan(p, base, codec, width, height, kbps, gop, stream, depth):
    p.debug.write(base + 0x100, struct.pack('<7I', codec, width, height, kbps, gop, stream, depth))
    p.debug.write(base + 0x200, bytes(0x30))
    rc = native_call(p, W5_MEMCALC, (base + 0x100, base + 0x200))
    o = dict(zip(W5_PLAN_FIELDS, struct.unpack('<12I', p.debug.read(base + 0x200, 0x30))))
    a = lambda v: (v + 0xfff) & ~0xfff
    # per-type sizes as 0x402f3d4c derives them (type 1 also scales with a mode-table count)
    o['arena'] = o['code'] + o['temp'] + o['work'] + o['fb_count'] * o['mv'] + 0x1000
    o['refs'] = (a(o['fb']) + a(o['fbc_y']) + a(o['fbc_c'])) * o['fb_count']
    o['src_frame'] = (o['fb2'] + 0x1fff) & ~0xfff
    return rc, o


def stage_w5_plan(p, args):
    """Ask the stock VPU memory calculator what an encoder instance needs (no hardware access)."""
    base = scratch(p, args)
    configs = [(1, 3840, 2160, 120000, 0, 0, 8), (1, 1920, 1080, 30000, 0, 0, 8),
               (1, 640, 432, 8000, 0, 0, 8), (0, 640, 432, 8000, 0, 0, 8)]
    for gop in range(4):
        configs.append((1, 640, 432, 8000, gop, 0, 8))
    for cfg in configs:
        rc, o = w5_plan(p, base, *cfg)
        p.log('w5-plan', config=cfg, rc=rc, out=o)
        mb = lambda v: f'{v / 2**20:.2f}'
        say(f'{"HEVC" if cfg[0] else "AVC "} {cfg[1]}x{cfg[2]} {cfg[3]} kbps gop{cfg[4]} -> rc {rc}: '
            f'arena {mb(o["arena"])} MB, refs {o["fb_count"]}x -> {mb(o["refs"])} MB, '
            f'src frame {mb(o["src_frame"])} MB, bitstream {mb(o["bitstream"])} MB | '
            + ' '.join(f'{k}={v:#x}' for k, v in o.items() if k in W5_PLAN_FIELDS))


def stage_dump(p, args):
    """Raw RAM range -> --output (page-checked first; RAM only, never MMIO)."""
    if args.address is None or not args.output:
        raise SystemExit('dump needs --address, --count and --output')
    for page in range(args.address & ~0xfff, args.address + args.count, 0x1000):
        if busy_retry(p, p.debug.translate, page)['fault']:
            raise SystemExit(f'{page:#x} not mapped')
    data, t0 = bytearray(), time.monotonic()
    while len(data) < args.count:
        data += busy_retry(p, p.debug.read, args.address + len(data), min(1008, args.count - len(data)))
    args.output.write_bytes(data)
    say(f'wrote {args.output}: {len(data)} B in {time.monotonic() - t0:.1f} s')


W5_BRINGUP, W5_EXIT, W5_CLIENT = 0x402f2d58, 0x402f3084, 4   # client 4: no stock caller uses it
W5_VDI = 0xa02d3d928


def w5_state(p):
    vdi = p.debug.read(W5_VDI, 0x100)
    return dict(base=struct.unpack_from('<Q', vdi, 0x58)[0], arena=struct.unpack_from('<Q', vdi, 0x28)[0],
                handles=struct.unpack('<3Q', p.debug.read(0xa02e98488, 0x18)),
                inst0=struct.unpack('<Q', p.debug.read(0xa02d308f0, 8))[0],
                ucode=struct.unpack('<Q', p.debug.read(0xa02ede828 + 8, 8))[0])


def stage_w5_up(p, args):
    """Photo-mode Wave5 power-up and shutdown as client 4 on a pool-backed 16 MiB arena.
    RAM reads only; the stock bring-up/exit do all register work."""
    st = w5_state(p)
    say(f'before: {st}')
    if st['base'] or st['inst0']:
        raise SystemExit('Wave5 already in use (movie/playback?); not touching it')
    if p.u32(POOL_GATE):
        raise SystemExit('pool admission gate closed')
    d = native_call(p, POOL_ALLOC, (0x4c4155, 0x60, 0x2000000))
    if not d:
        raise SystemExit('pool allocation failed')
    try:
        r = struct.unpack('<Q', p.debug.read(d + 8, 8))[0]
        cpu, actual = struct.unpack('<QI', p.debug.read(r, 12))
        arena = (cpu + 0xfff) & ~0xfff
        say(f'pool {actual / 2**20:.1f} MiB at {cpu:#x}; arena {arena:#x}')
        if (arena & 0xfffffff800000fff) != 0x800000000 or arena + 0x1000000 > cpu + actual:
            raise SystemExit('arena outside the VPU window')
        t0 = time.monotonic()
        rc = native_call(p, W5_BRINGUP, (W5_CLIENT, args.kbps, arena, 0x1000000), timeout_ms=20000)
        up = w5_state(p)
        say(f'bring-up rc {rc:#x} in {time.monotonic() - t0:.2f} s: {up}')
        p.log('w5-up', rc=rc, state={k: (hex(v) if isinstance(v, int) else [hex(x) for x in v]) for k, v in up.items()})
        snap_once(p, args.log_dir, 'w5up-on')
        if args.no_exit:
            d = 0   # keep the arena: the VPU owns it until the next power cycle
            say(f'left powered (no exit); arena pool block stays allocated')
            return
        t0 = time.monotonic()
        rc = native_call(p, W5_EXIT, (W5_CLIENT,), timeout_ms=20000)
        down = w5_state(p)
        say(f'exit rc {rc:#x} in {time.monotonic() - t0:.2f} s: {down}')
        snap_once(p, args.log_dir, 'w5up-off')
    finally:
        if d:
            native_call(p, POOL_FREE, (d,))
            say('pool block freed')


def stage_pool_cycle(p, args):
    """Isolation test: one photo-pool allocation of --count bytes, freed at once."""
    if p.u32(POOL_GATE):
        raise SystemExit('pool admission gate closed')
    d = native_call(p, POOL_ALLOC, (0x4c4155, 0x60, args.count))
    if not d:
        raise SystemExit('pool allocation failed')
    r = struct.unpack('<Q', p.debug.read(d + 8, 8))[0]
    cpu, actual = struct.unpack('<QI', p.debug.read(r, 12))
    cls = p.u32(d + 0x10)
    native_call(p, POOL_FREE, (d,))
    say(f'pool: desc {d:#x} class {cls:#x}, {actual / 2**20:.1f} MiB at {cpu:#x}, freed')


def stage_perm(p, args):
    """AT S1E1R vs S1E1W for one address (no access performed)."""
    if args.address is None:
        raise SystemExit('perm needs --address')
    r, w = p.debug.translate(args.address), p.debug.translate(args.address, True)
    say(f'{args.address:#x}: read {r}  write {w}')


W5_CONFIGURE = 0x40c333d0   # (cfg 0x20: codec,w,h32,kbps,gop,mode,depthY,depthC; out 0x38) -> 0 ok / 4 bad


def stage_w5_cfg(p, args):
    """Encoder memory requirements from the stock configure step (pure computation)."""
    base = scratch(p, args)
    for cfg in [(0, 1024, 704, 8000, 1, 0, 8, 8), (0, 1920, 1088, 28000, 1, 0, 8, 8),
                (1, 1024, 704, 8000, 1, 0, 8, 8), (0, 1024, 704, 8000, 0, 0, 8, 8)]:
        p.debug.write(base + 0x100, struct.pack('<8I', *cfg))
        p.debug.write(base + 0x200, bytes(0x38))
        rc = native_call(p, W5_CONFIGURE, (base + 0x100, base + 0x200))
        out = struct.unpack('<14I', p.debug.read(base + 0x200, 0x38))
        p.log('w5-cfg', cfg=cfg, rc=rc, out=[hex(v) for v in out])
        say(f'{cfg} rc {rc}: ' + ' '.join(f'{v:#x}' for v in out))


W5_OPEN, W5_REGISTER, W5_PARAMS_BUILD, W5_SET_PARAMS = 0x40c33834, 0x40c33480, 0x402f6c10, 0x40c338ac
W5_HEADERS, W5_RUN = 0x40c33d98, 0x40c34164
W5_NOOP_CB = 0x40c028ac   # `mov w0,#0; ret`
W5_CLIENT_MAP = 0xa02e98480   # bring-up stores the driver-assigned stream per client here (0xff = none)


def w5_stream(p):
    s = p.debug.read(W5_CLIENT_MAP + W5_CLIENT, 1)[0]
    if s == 0xff:
        raise SystemExit('client 4 has no stream (Wave5 not brought up by us)')
    return s


def w5_table(arena, regions):
    """0xf0-byte buffer registration (layout checked by 0x40c33480). All addresses share hi word 8."""
    t = bytearray(0xf0)
    hi = arena >> 32
    struct.pack_into('<IIQ', t, 0x00, hi, 0x1000000, arena)
    for off, key in ((0x10, 'frame'), (0x20, 'sub1'), (0x30, 'sub2')):
        struct.pack_into('<IIQ', t, off, regions[key][1], 0, regions[key][0])
    struct.pack_into('<II', t, 0x40, 2, regions['bs0'][1])
    struct.pack_into('<QQ', t, 0x48, regions['bs0'][0], regions['bs1'][0])
    struct.pack_into('<IIQ', t, 0x68, hi, regions['aux'][1], regions['aux'][0])
    struct.pack_into('<III', t, 0x78, hi, 2, regions['ref0'][1])
    struct.pack_into('<QQ', t, 0x88, regions['ref0'][0], regions['ref1'][0])
    struct.pack_into('<IIQ', t, 0xc8, regions['ext0'][1], 0, regions['ext0'][0])
    struct.pack_into('<IIQ', t, 0xd8, regions['ext1'][1], 0, regions['ext1'][0])
    struct.pack_into('<I', t, 0xe8, hi)
    # sizes sit at +4 of each (size, pad, base) triple; fix the three single regions
    for off, key in ((0x10, 'frame'), (0x20, 'sub1'), (0x30, 'sub2'), (0xc8, 'ext0'), (0xd8, 'ext1')):
        struct.pack_into('<I', t, off, regions[key][1]); struct.pack_into('<I', t, off + 4, 0)
    return bytes(t)


def stage_w5_open(p, args):
    """Open an AVC 1024x680 encoder on stream 1 with the stock service calls of 0x402f4560
    and fetch its SPS/PPS. Needs Wave5 already up (`w5-up --no-exit`); never calls exit."""
    st = w5_state(p)
    if st['base'] != 0x16010000:
        raise SystemExit(f'Wave5 not up: {st}')
    arena = st['arena']
    base = scratch(p, args)
    stream = w5_stream(p)
    d = args.address or native_call(p, POOL_ALLOC, (0x4c4155, 0x60, 0x3000000))
    if not d:
        raise SystemExit('pool allocation failed')
    r = struct.unpack('<Q', p.debug.read(d + 8, 8))[0]
    cpu, actual = struct.unpack('<QI', p.debug.read(r, 12))
    say(f'regions block: desc {d:#x}, {actual / 2**20:.1f} MiB at {cpu:#x}')
    at = (cpu + 0xfff) & ~0xfff
    regions = {}
    for key, size in (('frame', 0x1400000), ('sub1', 0x80000), ('sub2', 0x500000), ('bs0', 0x200000),
                      ('bs1', 0x200000), ('aux', 0x400000), ('ref0', 0x200000), ('ref1', 0x200000),
                      ('ext0', 0x200000), ('ext1', 0x200000), ('user', 0x80000)):
        regions[key] = (at, size)
        at += size
    assert at <= cpu + actual and all(v[0] >> 32 == arena >> 32 for v in regions.values())
    (args.log_dir / 'w5-open-regions.json').write_text(json.dumps(
        {'desc': hex(d), 'cpu': hex(cpu), 'arena': hex(arena), **{k: [hex(a), hex(n)] for k, (a, n) in regions.items()}}, indent=1))
    rc = native_call(p, W5_OPEN, (stream, W5_NOOP_CB))
    say(f'open stream {stream}: rc {rc}')
    if rc:
        return
    cfg = struct.pack('<8I', 0, 1024, 704, args.kbps, 1, 0, 8, 8)
    p.debug.write(base + 0x100, cfg)
    rc = native_call(p, W5_CONFIGURE, (base + 0x100, base + 0x200))
    say(f'configure: rc {rc}')
    p.debug.write(base + 0x300, w5_table(arena, regions))
    rc = native_call(p, W5_REGISTER, (stream, base + 0x300))
    say(f'register buffers: rc {rc}')
    if rc:
        return
    mode = struct.pack('<6H5I', 0, 0, 8, 1024, 680, 0, 30000, 1001, 15, 1, args.kbps * 1000) + struct.pack('<HH', 1, 0)
    p.debug.write(base + 0x400, mode)
    p.debug.write(base + 0x500, bytes(0x220))
    native_call(p, W5_PARAMS_BUILD, (base + 0x400, 0, regions['user'][0], base + 0x500))
    rc = native_call(p, W5_SET_PARAMS, (stream, base + 0x500, 0))
    say(f'set params: rc {rc}')
    if rc:
        return
    p.debug.write(base + 0x800, bytes(0x40))
    rc = native_call(p, W5_HEADERS, (stream, base + 0x800, base + 0x808, base + 0x810))
    hp, hn = struct.unpack('<QI', p.debug.read(base + 0x800, 12))
    lens = struct.unpack('<4I', p.debug.read(base + 0x810, 16))
    say(f'headers: rc {rc}, at {hp:#x}, n {hn:#x}, lens {lens}')
    if hp and 0 < sum(lens) < 0x400:
        hdr = p.debug.read(hp, sum(l + 4 for l in lens if l) or 64)
        say(f'header bytes: {hdr.hex()}')
        (args.log_dir / 'w5-open-headers.bin').write_bytes(hdr)


def stage_w5_hdr(p, args):
    """Retry the header fetch on the open stream (lock busy -> rc 1)."""
    base = scratch(p, args)
    stream = w5_stream(p)
    for attempt in range(10):
        p.debug.write(base + 0x800, bytes(0x40))
        rc = native_call(p, W5_HEADERS, (stream, base + 0x800, base + 0x808, base + 0x810))
        hp, hn = struct.unpack('<QI', p.debug.read(base + 0x800, 12))
        lens = struct.unpack('<4I', p.debug.read(base + 0x810, 16))
        say(f'try {attempt}: rc {rc}, at {hp:#x}, n {hn:#x}, lens {lens}')
        if rc == 0:
            if hp and 0 < sum(lens) < 0x400:
                hdr = p.debug.read(hp, min(1008, sum(l + 4 for l in lens if l)))
                say(f'header bytes: {hdr.hex()}')
                (args.log_dir / 'w5-open-headers.bin').write_bytes(hdr)
            return
        time.sleep(0.5)


W5_SUBMIT, W5_TRIGGER, MEMCPY = 0x40c33e34, 0x40c33fb0, 0x4127a900
B1_BASE, B1_STRIDE, B1_UV = 0xa1f5f9800, 0x1c2000, 0x12c000   # photo-LV 1024x680 NV12 ring (6 slots)


def stage_w5_enc(p, args):
    """Encode 4 live-view frames on the open stream: copy B1 slots into the VPU window,
    run, submit one batch of 4, trigger each (first as IDR), then scan bs0/bs1 for NALs."""
    stream = w5_stream(p)
    reg = json.loads((args.log_dir / 'w5-open-regions.json').read_text())
    user_base, user_size = (int(v, 16) for v in reg['user'])
    frames = (user_base + user_size + 0xfff) & ~0xfff
    fsize, csize = 1024 * 704 * 3 // 2, 1024 * 704
    entries = b''
    for i in range(4):
        dst = frames + i * fsize
        src = B1_BASE + i * B1_STRIDE
        native_call(p, MEMCPY, (dst, src, 1024 * 680))
        native_call(p, MEMCPY, (dst + csize, src + B1_UV, 1024 * 340))
        entries += struct.pack('<QQ', dst, dst + csize)
    say(f'4 frames copied to {frames:#x} (Y/C at +0/+{csize:#x})')
    base = scratch(p, args)
    p.debug.write(base + 0x900, struct.pack('<4I', 1, 1, 15, 15))
    rc = native_call(p, W5_RUN, (stream, base + 0x900))
    say(f'run: rc {rc}')
    p.debug.write(base + 0xa00, entries)
    rc = native_call(p, W5_SUBMIT, (stream, 0, base + 0xa00))
    say(f'submit 4: rc {rc}')
    for i in range(4):
        rc = native_call(p, W5_TRIGGER, (stream, 0, i, 0, 1 if i == 0 else 0))
        say(f'trigger {i}: rc {rc}, worker state {p.u32(0xa02d3274c)}, result {p.u32(0xa02d32770) & 0xffffffff:#x}')
        time.sleep(0.2)
    time.sleep(1)
    for key in ('bs0', 'bs1'):
        a, n = (int(v, 16) for v in reg[key])
        head = bytes(p.debug.read(a, 1008))
        say(f'{key} @{a:#x}: {head[:64].hex()}')
        if any(head):
            data = bytearray()
            while len(data) < args.count:
                data += busy_retry(p, p.debug.read, a + len(data), min(1008, args.count - len(data)))
            (args.log_dir / f'w5-enc-{key}.bin').write_bytes(data)
            nals = [(i, data[i + 4] & 0x1f) for i in range(len(data) - 5) if data[i:i + 4] == b'\0\0\0\1']
            say(f'  {len(nals)} start codes: {nals[:20]}')


W5_INFO = 0xa02d38b98   # per stream 9 x 0x480 frame-info ring: +0 u64 addr, +8 size, +0x2c type (0 I), +0x60 90 kHz pts


def stage_w5_more(p, args):
    """Second batch on a running stream: trigger flags from --line (e.g. 1,0,0,0); report
    each new frame's info entry and first NAL header."""
    stream = w5_stream(p)
    reg = json.loads((args.log_dir / 'w5-open-regions.json').read_text())
    user_base, user_size = (int(v, 16) for v in reg['user'])
    frames = (user_base + user_size + 0xfff) & ~0xfff
    fsize, csize = 1024 * 704 * 3 // 2, 1024 * 704
    flags = [int(x) for x in (args.line or ['0,0,0,0'])[0].split(',')]
    base = scratch(p, args)
    entries = b''.join(struct.pack('<QQ', frames + i * fsize, frames + i * fsize + csize) for i in range(4))
    p.debug.write(base + 0xa00, entries)
    rc = native_call(p, W5_SUBMIT, (stream, 0, base + 0xa00))
    say(f'submit: rc {rc}')
    for i, f in enumerate(flags):
        rc = native_call(p, W5_TRIGGER, (stream, 0, i, 0, f))
        say(f'trigger {i} flag {f}: rc {rc}')
        time.sleep(0.15)
    time.sleep(0.5)
    for e in range(9):
        a = W5_INFO + stream * 0x2400 + e * 0x480
        addr, size = struct.unpack('<QI', p.debug.read(a, 12))
        typ, idx, pts = p.u32(a + 0x2c), p.u32(a + 0x40), p.u32(a + 0x60)
        nal = p.debug.read(addr, 8).hex() if addr >> 32 == 8 else ''
        say(f'info[{e}] idx {idx} type {typ} pts {pts} size {size} @{addr:#x} {nal}')


def stage_w5_reparam(p, args):
    """Re-issue set-params on the running stream with word overrides from --line (off=val,...)."""
    stream = w5_stream(p)
    base = scratch(p, args)
    for item in (args.line or [''])[0].split(','):
        if item:
            off, val = (int(x, 0) for x in item.split('='))
            p.debug.write(base + 0x500 + off, struct.pack('<I', val))
    rc = native_call(p, W5_SET_PARAMS, (stream, base + 0x500, 0))
    say(f'set params again: rc {rc}, worker state {p.u32(0xa02d3274c)}, result {p.u32(0xa02d32770):#x}')


def stage_w5_soak(p, args):
    """--count frames on the running stream (4 fixed slots, I every 15): record each frame's
    bitstream address/size/type to find the bitstream-buffer wrap behaviour."""
    stream = w5_stream(p)
    reg = json.loads((args.log_dir / 'w5-open-regions.json').read_text())
    user_base, user_size = (int(v, 16) for v in reg['user'])
    frames = (user_base + user_size + 0xfff) & ~0xfff
    fsize, csize = 1024 * 704 * 3 // 2, 1024 * 704
    bs0, bsn = (int(v, 16) for v in reg['bs0'])
    bs1, _ = (int(v, 16) for v in reg['bs1'])
    base = scratch(p, args)
    p.debug.write(base + 0xa00, b''.join(struct.pack('<QQ', frames + i * fsize, frames + i * fsize + csize) for i in range(4)))
    # next hardware index: highest idx seen in the ring + 1
    ring = [W5_INFO + stream * 0x2400 + e * 0x480 for e in range(9)]
    idx = max(p.u32(a + 0x60) for a in ring) // 3000 + 1   # +0x60: 90 kHz stamp = picture * 3000
    rows, t0 = [], time.monotonic()
    for k in range(args.count):
        if k % 4 == 0 and native_call(p, W5_SUBMIT, (stream, 0, base + 0xa00)):
            raise SystemExit(f'submit failed at {k}')
        if native_call(p, W5_TRIGGER, (stream, 0, k % 4, 0, 1 if k % 15 == 0 else 0)):
            raise SystemExit(f'trigger failed at {k}')
        a = ring[idx % 9]
        for _ in range(100):
            addr, size = struct.unpack('<QI', p.debug.read(a, 12))
            if p.u32(a + 0x60) == idx * 3000 and size:
                break
            time.sleep(0.01)
        else:
            raise SystemExit(f'frame {k} (hw {idx}) never completed')
        typ = p.u32(a + 0x2c)
        where = 'bs0' if bs0 <= addr < bs0 + bsn else 'bs1' if bs1 <= addr < bs1 + bsn else 'OUT'
        buf_end = (bs0 if where == 'bs0' else bs1) + bsn
        if addr + size > buf_end:   # crosses a buffer boundary: keep the whole info entry
            (args.log_dir / f'w5-cross-{k}.bin').write_bytes(p.debug.read_range(a, 0x480))
        end_ok = addr + size <= (bs0 if where == 'bs0' else bs1) + bsn
        rows.append((k, idx, typ, size, hex(addr), where, end_ok))
        idx += 1
    (args.log_dir / 'w5-soak.json').write_text(json.dumps(rows))
    switches = [r for i, r in enumerate(rows) if i and r[5] != rows[i - 1][5]]
    backs = [r for i, r in enumerate(rows) if i and r[5] == rows[i - 1][5] and int(r[4], 16) < int(rows[i - 1][4], 16)]
    say(f'{len(rows)} frames in {time.monotonic() - t0:.0f} s, I {sum(r[2] == 0 for r in rows)}, '
        f'bytes {sum(r[3] for r in rows)}, outside {sum(r[5] == "OUT" for r in rows)}, straddle {sum(not r[6] for r in rows)}')
    say(f'buffer switches: {[(r[0], r[5], r[4]) for r in switches][:12]}')
    say(f'wrap-backs within a buffer: {[(r[0], r[5], r[4]) for r in backs][:12]}')


def stage_w5_clip(p, args):
    """--count live-view frames through the production sequence (fresh B1 copy per frame,
    batch of 4, I every 15, stamp-matched completion, ring-wrap reassembly); writes
    logs/w5-clip.aus = repeated {u32 size, u64 pts_us, bytes}."""
    stream = w5_stream(p)
    reg = json.loads((args.log_dir / 'w5-open-regions.json').read_text())
    user_base, user_size = (int(v, 16) for v in reg['user'])
    frames = (user_base + user_size + 0xfff) & ~0xfff
    fsize, csize = 1024 * 704 * 3 // 2, 1024 * 704
    bs0 = int(reg['bs0'][0], 16); end = bs0 + 0x400000
    base = scratch(p, args)
    p.debug.write(base + 0xa00, b''.join(struct.pack('<QQ', frames + i * fsize, frames + i * fsize + csize) for i in range(4)))
    ring = [W5_INFO + stream * 0x2400 + e * 0x480 for e in range(9)]
    idx = max(p.u32(a + 0x60) for a in ring) // 3000 + 1
    out, t0, wraps = bytearray(), time.monotonic(), 0
    pending_wrap = None
    for k in range(args.count):
        slot, src = k % 4, B1_BASE + (k % 6) * B1_STRIDE
        if slot == 0 and native_call(p, W5_SUBMIT, (stream, 0, base + 0xa00)):
            raise SystemExit(f'submit failed at {k}')
        dst = frames + slot * fsize
        native_call(p, MEMCPY, (dst, src, 1024 * 680))
        native_call(p, MEMCPY, (dst + csize, src + B1_UV, 1024 * 340))
        if native_call(p, W5_TRIGGER, (stream, 0, slot, 0, 1 if k % 15 == 0 else 0)):
            raise SystemExit(f'trigger failed at {k}')
        a = ring[idx % 9]
        for _ in range(100):
            addr, size = struct.unpack('<QI', p.debug.read(a, 12))
            if p.u32(a + 0x60) == idx * 3000 and size:
                break
            time.sleep(0.01)
        else:
            raise SystemExit(f'frame {k} never completed')
        if p.u32(a + 0x0c):   # split picture: {+0x10,+0x18} then {+0x20,+0x28}
            wraps += 1
            a1, n1 = struct.unpack('<QI', p.debug.read(a + 0x10, 12))
            a2, n2 = struct.unpack('<QI', p.debug.read(a + 0x20, 12))
            data = p.debug.read_range(a1, n1) + p.debug.read_range(a2, n2)
            assert n1 + n2 == size
        else:
            data = p.debug.read_range(addr, size)
        out += struct.pack('<IQ', size, k * 33367) + data
        idx += 1
    (args.log_dir / 'w5-clip.aus').write_bytes(out)
    say(f'{args.count} AUs, {len(out)} B, {wraps} wrapped, {time.monotonic() - t0:.0f} s')


def stage_pool_free(p, args):
    """Release a pool descriptor left behind by an interrupted stage (address from its log)."""
    if args.address is None:
        raise SystemExit('pool-free needs --address <descriptor>')
    r = struct.unpack('<Q', p.debug.read(args.address + 8, 8))[0]
    cpu, actual = struct.unpack('<QI', p.debug.read(r, 12)) if r else (0, 0)
    say(f'descriptor {args.address:#x}: {actual / 2**20:.1f} MiB at {cpu:#x}')
    if not (0x800000000 <= cpu < 0x900000000 and actual % 0xaec000 == 0):
        raise SystemExit('does not look like a live pool descriptor; not freeing')
    native_call(p, POOL_FREE, (args.address,))
    say('freed')


def busy_retry(p, fn, *a, tries=60):
    """Read-only monitor calls only: after a burst the camera answers SUBMIT with
    0x2019 for a while. Release the retained job and resend (re-running a read is harmless)."""
    from ptp_client import PtpError
    for k in range(tries):
        try:
            return fn(*a)
        except PtpError as exc:
            if exc.code != 0x2019 or k == tries - 1:
                raise
            p.log('busy', call=fn.__name__, attempt=k)
            time.sleep(1)
            if p.client.pending is not None:
                try:
                    p.client.release(p.client.pending[0])
                except Exception as rel:
                    p.log('release-failed', error=repr(rel))
                p.client.pending = None


def stage_sample(p, args):
    """Read 64 B every --step bytes over [--address, --address+--count) and save a
    fingerprint per sample to logs/dram-<label>.json; diff against --against if given.
    Pages are translated first; unmapped samples are recorded as None, never read."""
    if not args.label or args.address is None:
        raise SystemExit('sample needs --label and --address')
    out, t0 = {}, time.monotonic()
    for at in range(args.address, args.address + args.count, args.step):
        if busy_retry(p, p.debug.translate, at & ~0xfff)['fault']:
            out[f'{at:#x}'] = None
            continue
        data = busy_retry(p, p.debug.read, at, 64)
        out[f'{at:#x}'] = data.hex() if any(data) else '0'
    path = args.log_dir / f'dram-{args.label}.json'
    path.write_text(json.dumps(out, indent=0))
    say(f'wrote {path}: {len(out)} samples in {time.monotonic() - t0:.1f} s, '
        f'{sum(v is None for v in out.values())} unmapped, {sum(v == "0" for v in out.values())} zero')
    if args.against:
        old = json.loads((args.log_dir / f'dram-{args.against}.json').read_text())
        changed = [a for a in out if a in old and out[a] != old[a]]
        say(f'{len(changed)} of {len(out)} samples changed since {args.against}')
        runs, start, prev = [], None, None
        for a in map(lambda k: int(k, 16), out):
            hit = f'{a:#x}' in changed
            if hit and start is None:
                start = a
            if not hit and start is not None:
                runs.append((start, a)); start = None
        if start is not None:
            runs.append((start, args.address + args.count))
        for lo, hi in runs:
            say(f'  changed {lo:#x}-{hi:#x} ({(hi - lo) / 2**20:.0f} MB)')


# Standard Nikon remote-control operations (no data phase): the camera refuses the REC
# button while a PTP session is open, but records to the card on StartMovieRecInCard.
DEVICE_READY, LV_START, LV_END, REC_START, REC_END = 0x90c8, 0x9201, 0x9202, 0x920a, 0x920b


def wpd_op(t, id, opcode, params):
    import ctypes as c
    v = t.command(id)
    v.SetUnsignedIntegerValue(c.byref(t.key(1001)), opcode)
    coll = t.client.CreateObject(t.types.PortableDevicePropVariantCollection,
                                 interface=t.api.IPortableDevicePropVariantCollection)
    tmp = t.values()
    for n in params:
        tmp.SetUnsignedIntegerValue(c.byref(t.key(1001)), n)
        variant = tmp.GetValue(c.byref(t.key(1001)))
        coll.Add(c.byref(variant))
    v.SetIPortableDevicePropVariantCollectionValue(c.byref(t.key(1002)), coll)
    return v


def nikon_op(p, opcode, *params):
    """Send one Nikon operation without a data phase; return the PTP response code (no raise).
    Fenestra's WPD transport only permits 0x95e0..3, so this builds the WPD
    EXECUTE_COMMAND_WITHOUT_DATA_PHASE (12) request from its helpers directly."""
    t = p.client.transport
    if hasattr(t, 'nikon_command'):
        return t.nikon_command(opcode, params)
    import ctypes as c
    code = t.send(wpd_op(t, 12, opcode, params)).GetUnsignedIntegerValue(c.byref(t.key(1003)))
    p.log('nikon-op', opcode=f'0x{opcode:04x}', params=[hex(n) for n in params], response=f'0x{code:04x}')
    return code


def nikon_read(p, opcode, *params, maximum=1024):
    """Operation with a data-in phase (WPD 13/15/17); returns (response code, data)."""
    t = p.client.transport
    if hasattr(t, 'nikon_read'):
        return t.nikon_read(opcode, params)
    import ctypes as c
    result = t.send(wpd_op(t, 13, opcode, params))
    context = result.GetStringValue(c.byref(t.key(1006)))
    data = b''
    try:
        total = min(result.GetUnsignedLargeIntegerValue(c.byref(t.key(1007))), maximum)
        while len(data) < total:
            n = min(1024, total - len(data))
            v = t.context_command(15, context)
            v.SetUnsignedIntegerValue(c.byref(t.key(1008)), n)
            v.SetBufferValue(c.byref(t.key(1012)), (c.c_ubyte * n)(), n)
            part = t.send(v)
            got = part.GetUnsignedIntegerValue(c.byref(t.key(1009)))
            pointer, count = part.GetBufferValue(c.byref(t.key(1012)))
            try:
                data += c.string_at(pointer, count)
            finally:
                t.free(pointer)
            if not got:
                break
    finally:
        code = t.send(t.context_command(17, context)).GetUnsignedIntegerValue(c.byref(t.key(1003)))
    p.log('nikon-read', opcode=f'0x{opcode:04x}', params=[hex(n) for n in params],
          response=f'0x{code:04x}', data=data.hex())
    return code, data


MOVIE_PROHIBIT = {14: 'not in application mode', 13: 'set-liveview selector enabled',
                  12: 'enlarged liveview', 11: 'card protected', 10: 'already recording',
                  9: 'buffer not yet written', 3: 'card full', 2: 'card not formatted',
                  1: 'card error', 0: 'no card'}


def movie_prohibit(p):
    code, data = nikon_read(p, 0x1015, 0xd0a4)   # GetDevicePropValue MovRecProhibitCondition
    if code != 0x2001 or len(data) < 4:
        say(f'MovRecProhibitCondition read: 0x{code:04x} {data.hex()}')
        return None
    value = struct.unpack_from('<I', data)[0]
    why = [m for b, m in MOVIE_PROHIBIT.items() if value >> b & 1]
    say(f'MovRecProhibitCondition 0x{value:08x} {why}')
    return value


def nikon_write(p, opcode, params, payload):
    """Operation with a data-out phase (WPD 14/16/17); returns the response code."""
    import ctypes as c
    t = p.client.transport
    start = wpd_op(t, 14, opcode, params)
    start.SetUnsignedLargeIntegerValue(c.byref(t.key(1007)), len(payload))
    result = t.send(start)
    context = result.GetStringValue(c.byref(t.key(1006)))
    try:
        v = t.context_command(16, context)
        v.SetUnsignedIntegerValue(c.byref(t.key(1010)), len(payload))
        v.SetBufferValue(c.byref(t.key(1012)), (c.c_ubyte * len(payload)).from_buffer_copy(payload), len(payload))
        t.send(v)
    finally:
        code = t.send(t.context_command(17, context)).GetUnsignedIntegerValue(c.byref(t.key(1003)))
    p.log('nikon-write', opcode=f'0x{opcode:04x}', params=[hex(n) for n in params],
          data=payload.hex(), response=f'0x{code:04x}')
    return code


def set_u8_prop(p, prop, value):
    """SetDevicePropValue (0x1016) for a UINT8 property; returns (response, value read back)."""
    code = nikon_write(p, 0x1016, (prop,), bytes([value]))
    wait_ready(p)
    c, d = nikon_read(p, 0x1015, prop)
    return code, (d[0] if c == 0x2001 and d else None)


LV_SELECTOR = 0xd1a6   # live-view photo/movie selector (0 photo, 1 movie); read-only outside app mode


PTP_SIZES = {1: 1, 2: 1, 3: 2, 4: 2, 5: 4, 6: 4, 7: 8, 8: 8}


def parse_prop_desc(d):
    """PTP DevicePropDesc -> dict (strings and arrays skipped)."""
    code, dtype, getset = struct.unpack_from('<HHB', d, 0)
    n = PTP_SIZES.get(dtype)
    if not n:
        return dict(code=code, dtype=dtype, getset=getset)
    fmt = {1: '<b', 2: '<B', 3: '<h', 4: '<H', 5: '<i', 6: '<I', 7: '<q', 8: '<Q'}[dtype]
    at = 5
    default = struct.unpack_from(fmt, d, at)[0]; at += n
    current = struct.unpack_from(fmt, d, at)[0]; at += n
    form = d[at] if at < len(d) else 0; at += 1
    out = dict(code=code, dtype=dtype, getset=getset, default=default, current=current, form=form)
    if form == 2:
        count = struct.unpack_from('<H', d, at)[0]; at += 2
        out['enum'] = [struct.unpack_from(fmt, d, at + i * n)[0] for i in range(min(count, 64))]
    elif form == 1:
        out['range'] = [struct.unpack_from(fmt, d, at + i * n)[0] for i in range(3)]
    return out


def stage_props(p, args):
    """Read-only: DeviceInfo property list, then the descriptor of every 0xD0xx-0xD2xx property
    that is a small writable enum (candidates for the live-view photo/movie selector)."""
    code, info = nikon_read(p, 0x1001, maximum=0x4000)
    at = 8
    at += 1 + 2 * info[at]               # vendor extension description
    at += 2                               # functional mode
    props = []
    for i in range(3):                    # operations, events, properties
        count = struct.unpack_from('<I', info, at)[0]; at += 4
        items = struct.unpack_from(f'<{count}H', info, at); at += 2 * count
        if i == 2:
            props = items
    c, vend = nikon_read(p, 0x90ca, maximum=0x4000)   # Nikon GetVendorPropCodes
    if c == 0x2001 and len(vend) >= 4:
        count = struct.unpack_from('<I', vend, 0)[0]
        props = tuple(props) + struct.unpack_from(f'<{min(count, (len(vend) - 4) // 2)}H', vend, 4)
    if args.line and args.line[0] == 'scan':   # vendor list unsupported: try every code
        props = tuple(range(0xd000, 0xd300))
    say(f'DeviceInfo 0x{code:04x}, vendor 0x{c:04x}: {len(props)} properties')
    rows = []
    for prop in props:
        if args.address and prop != args.address:
            continue
        if not args.address and not 0xd000 <= prop < 0xd300:
            continue
        c, d = nikon_read(p, 0x1014, prop)
        if c != 0x2001 or len(d) < 5:
            continue
        desc = parse_prop_desc(d)
        rows.append({k: (hex(v) if k == 'code' else v) for k, v in desc.items()})
        if args.address or (desc.get('getset') and desc.get('enum') and len(desc['enum']) <= 4):
            say(f"{prop:#06x} rw={desc['getset']} type={desc['dtype']} cur={desc.get('current')} "
                f"def={desc.get('default')} enum={desc.get('enum')} range={desc.get('range')}")
    (args.log_dir / 'props.json').write_text(json.dumps(rows, indent=1))
    say(f'{len(rows)} descriptors written to logs/props.json')


SETTING_GET = 0x408564ec          # (id) -> value of a camera setting item
SEL_ITEM, SEL_REMOTE_OK = 0x06000003, 0x3200   # LV selector item; 0x3200 == 1 lets PTP set it
SEL_PTP_SET, SEL_MAP, SEL_TO_INTERNAL = 0x4083cc58, 0x440f3fb8, 0x40811c40   # D1A6 set handler


def selector_state(p):
    return dict(item=native_call(p, SETTING_GET, (SEL_ITEM,)) & 0xffffffff,
                remote_ok=native_call(p, SETTING_GET, (SEL_REMOTE_OK,)) & 0xffffffff,
                prohibit=movie_prohibit(p))


def selector_native_set(p, args, value):
    """Run the stock 0xD1A6 SetDevicePropValue handler directly (skips only its 0x3200 gate):
    map PTP value -> internal, notify 0x04070021, set item 0x06000003, post prop-changed."""
    base = scratch(p, args)
    p.debug.write(base + 0xf00, struct.pack('<I', value))
    rc = native_call(p, SEL_PTP_SET, (base + 0xf00,), timeout_ms=10000) & 0xffffffff
    wait_ready(p)
    return rc


def stage_selector(p, args):
    """Read the LV selector state; with --line set=0|1 switch it through the stock handler."""
    st = selector_state(p)
    maps = {v: native_call(p, SEL_TO_INTERNAL, (SEL_MAP, 2, v)) & 0xffffffff for v in (0, 1)}
    say(f'selector item {st["item"]:#x}, 0x3200 {st["remote_ok"]}, prohibit {st["prohibit"]:#x}, '
        f'PTP->internal {maps}')
    if args.line and args.line[0].startswith('set='):
        v = int(args.line[0][4:])
        rc = selector_native_set(p, args, v)
        time.sleep(1)
        st = selector_state(p)
        say(f'set {v}: rc {rc:#x}; now item {st["item"]:#x}, prohibit {st["prohibit"]:#x}')


def stage_op(p, args):
    """Raw Nikon operation without data: --line 0x9435,0"""
    vals = [int(x, 0) for x in args.line[0].split(',')]
    say(f'op {vals[0]:#06x}{vals[1:]}: 0x{nikon_op(p, vals[0], *vals[1:]):04x}, ready 0x{wait_ready(p):04x}')
    say(f'prohibit {movie_prohibit(p):#x}')


def stage_call(p, args):
    """Native call: --address fn --line a0,a1,... (use only for known side-effect-free or vetted calls)."""
    vals = [int(x, 0) for x in args.line[0].split(',')] if args.line and args.line[0] else []
    say(f'{args.address:#x}{tuple(hex(v) for v in vals)} -> {native_call(p, args.address, tuple(vals)) & 0xffffffffffffffff:#x}')


W5_SVC = 0xa02d30110   # +0 VDI up, +8/+0x14 stream slots {used, coreA, coreB}, +0x20/+0x24 core A/B busy


def w5_summary(p):
    svc = struct.unpack('<10I', p.debug.read(W5_SVC, 0x28))
    cmap = bytes(p.debug.read(W5_CLIENT_MAP, 5))
    return dict(vdi=hex(p.u64(W5_VDI_BASE)), arena=hex(p.u64(W5_VDI + 0x28)), svc_up=svc[0],
                slot0=svc[2:5], slot1=svc[5:8], core_busy=svc[8:10], client_map=cmap.hex(),
                photo_movie=p.u32(0xa03bd062c), audio=p.u32(STATE), recorder=p.u32(0xa03bc7388),
                movie_state=p.u32(0xa03bc7994))


def stage_w5_state(p, args):
    """Read-only: Wave5 ownership + mode words (no native calls)."""
    say(json.dumps(w5_summary(p)))


def stage_w5_release(p, args):
    """Hand-run the payload's preempt: stock exit 0x402f3084(4) when client 4 holds Wave5
    alone. Refused in photo mode (exit hangs photo LV) unless --force (playback test)."""
    st = w5_summary(p)
    say(f'before: {json.dumps(st)}')
    stream = st['client_map'][8:10]
    if stream == 'ff' or int(stream, 16) > 1:
        raise SystemExit('client 4 holds no stream')
    mine, other = (st['slot0'], st['slot1']) if stream == '00' else (st['slot1'], st['slot0'])
    if not mine[0] or st['vdi'] == '0x0':
        raise SystemExit('client 4 map entry is stale (slot closed or VDI down)')
    if other[0]:
        raise SystemExit('the other stream slot is in use: exit would reset Wave5 under it')
    if st['photo_movie'] == 0 and not args.force:
        raise SystemExit('photo mode: exit hangs photo LV; switch to movie/playback first (or --force)')
    t0 = time.monotonic()
    rc = native_call(p, W5_EXIT, (W5_CLIENT,), timeout_ms=20000)
    say(f'exit rc {rc:#x} in {time.monotonic() - t0:.2f} s')
    say(f'after: {json.dumps(w5_summary(p))}')


def wait_ready(p, timeout=5.0):
    end = time.monotonic() + timeout
    while (code := nikon_op(p, DEVICE_READY)) == 0x2019 and time.monotonic() < end:
        time.sleep(0.1)
    return code


def stage_remote_rec(p, args):
    """Start a movie recording over PTP, snap the Wave5 driver RAM during and after it."""
    label = args.label or 'remote'
    snap_once(p, args.log_dir, f'{label}-before')
    app_mode = False
    if (prohibit := movie_prohibit(p)) and prohibit >> 14 & 1:
        code = nikon_op(p, 0x9435, 1)   # ChangeApplicationMode
        app_mode = code == 0x2001
        say(f'ChangeApplicationMode(1): 0x{code:04x}, ready 0x{wait_ready(p):04x}')
    selector_set = False
    if (prohibit := movie_prohibit(p)) and prohibit >> 13 & 1:
        code, back = set_u8_prop(p, LV_SELECTOR, 1)
        selector_set = code == 0x2001
        say(f'live-view selector -> movie: 0x{code:04x}, reads {back}')
        if not selector_set:   # 0x200F: gate 0x3200 closed; run the stock set handler directly
            rc = selector_native_set(p, args, 1)
            time.sleep(1)
            st = selector_state(p)
            selector_set = 'native'
            say(f'native selector -> movie: rc {rc:#x}, item {st["item"]:#x}, prohibit {st["prohibit"]:#x}')
            time.sleep(3)   # let the mode switch settle before live view
    say(f'live view start: 0x{nikon_op(p, LV_START):04x}, ready 0x{wait_ready(p):04x}')
    time.sleep(3)
    c, d = nikon_read(p, 0x1015, 0xd1a2)   # LiveViewStatus
    say(f'LiveViewStatus 0x{c:04x} {d.hex()}')
    prohibit = movie_prohibit(p)
    inhibit_cleared = False
    if selector_set == 'native' and not prohibit:
        reg = native_call(p, 0x40f23244, (0x40000100,)) & 0xffffffff
        if reg & 0x10:   # executor 0x40fa1d68 refuses (0xA004) while this bit is set
            native_call(p, 0x40ec479c, (0x10,))
            inhibit_cleared = True
            say(f'reg 0x40000100 {reg:#x} -> {native_call(p, 0x40f23244, (0x40000100,)) & 0xffffffff:#x}')
    code = nikon_op(p, REC_START) if not prohibit else 0
    say(f'StartMovieRecInCard: 0x{code:04x}')
    if code == 0xa004:   # 0x920a handler: precheck 0x4084144c, executor needs state 0xa03bc7388 == 2
        say(f'  precheck 0x4084144c -> {native_call(p, 0x4084144c, ()) & 0xffffffff:#x}, '
            f'recorder state {p.u32(0xa03bc7388)}, busy {p.u32(0xa03bc73b8)}, audio {p.u32(STATE)}, '
            f'reg 0x40000100 = {native_call(p, 0x40f23244, (0x40000100,)) & 0xffffffff:#x} '
            f'@ {native_call(p, 0x40f23270, (0x40000100,)):#x}')
    trace, t0, last = [], time.monotonic(), None
    def tick():
        nonlocal last
        m = w5_marks(p)
        if m != last:
            trace.append((round(time.monotonic() - t0, 2), *[f'{v:#x}' for v in m]))
            say(f'{trace[-1][0]:7.2f}s vdi {m[0]:#x} audio {m[1]} inst0 {m[2]:#x}')
            last = m
        time.sleep(args.interval)
    try:
        if code != 0x2001:
            return
        while time.monotonic() - t0 < 5 and not (last and last[0]):
            tick()
        snap_once(p, args.log_dir, f'{label}-rec')
        time.sleep(3)
        snap_once(p, args.log_dir, f'{label}-rec2')
        end = time.monotonic() + args.seconds
        while time.monotonic() < end:
            tick()
    finally:
        if code == 0x2001:
            say(f'EndMovieRec: 0x{nikon_op(p, REC_END):04x}, ready 0x{wait_ready(p, 15):04x}')
            end = time.monotonic() + 4
            while time.monotonic() < end:
                tick()
            snap_once(p, args.log_dir, f'{label}-after')
        if inhibit_cleared:
            native_call(p, 0x40ec46ec, (0x10,))
            say(f'reg 0x40000100 restored: {native_call(p, 0x40f23244, (0x40000100,)) & 0xffffffff:#x}')
        say(f'live view end: 0x{nikon_op(p, LV_END):04x}')
        if selector_set == 'native':
            rc = selector_native_set(p, args, 0)
            say(f'native selector -> photo: rc {rc:#x}, item {selector_state(p)["item"]:#x}')
        elif selector_set:
            code, back = set_u8_prop(p, LV_SELECTOR, 0)
            say(f'live-view selector -> photo: 0x{code:04x}, reads {back}')
        if app_mode:
            say(f'ChangeApplicationMode(0): 0x{nikon_op(p, 0x9435, 0):04x}')
        (args.log_dir / f'w5-{label}-trace.json').write_text(json.dumps(trace, indent=1))
        p.log('remote-rec-trace', trace=trace)


# Serialized stock file service (same entries Fenestra/Live v4 use on the camera).
F_OPEN, F_READ, F_CLOSE = 0x402dd4f8, 0x402dd6c8, 0x402dd558
D_OPEN, D_READ, D_CLOSE = 0x402ddc6c, 0x402ddd1c, 0x402ddd6c
SCRATCH_BYTES, SCRATCH_MAGIC = 0x11000, b'FXPROBE1'


def scratch(p, args):
    """One reusable camera heap block (address kept on the host, magic-checked)."""
    note = args.log_dir / 'scratch.txt'
    try:
        base = int(note.read_text().strip(), 16)
        if bytes(p.debug.read(base, 8)) == SCRATCH_MAGIC:
            return base
    except (OSError, ValueError):
        pass
    base = native_call(p, HEAP_ALLOC, (SCRATCH_BYTES, 0))
    if not base:
        raise SystemExit('heap allocation failed')
    p.debug.write(base, SCRATCH_MAGIC)
    note.write_text(f'{base:#x}\n')
    return base


def put_path(p, at, path):
    raw = path.encode('ascii') + b'\0'
    if len(raw) > 0xf0:
        raise SystemExit('path too long')
    p.debug.write(at, raw)


def s32(v):
    v &= 0xffffffff
    return v - (1 << 32) if v & 0x80000000 else v


def entry_names(raw):
    """Dir entry (0x402ddd1c, seen on the camera): 8.3 ASCII name at +0, NUL-terminated
    UTF-16LE long name at +0xe. No size or attribute bytes were present."""
    short = raw[:0xe].split(b'\0', 1)[0].decode('ascii', 'replace')
    long_ = raw[0xe:]
    end = next((i for i in range(0, len(long_) - 1, 2) if long_[i:i + 2] == b'\0\0'), len(long_))
    long_ = long_[:end].decode('utf-16-le', 'replace')
    return [long_ or short] + ([short] if long_ and short and short != long_ else [])


def stage_ls(p, args):
    base = scratch(p, args)
    path, fd_at, entry = base + 0x10, base + 0x100, base + 0x200
    put_path(p, path, args.path)
    p.debug.write(fd_at, b'\xff' * 4)
    rc = s32(native_call(p, D_OPEN, (path, fd_at)))
    fd = s32(p.u32(fd_at))
    if rc or fd < 0:
        raise SystemExit(f'open_dir {args.path}: rc {rc:#x}, fd {fd}')
    try:
        for i in range(args.max_entries):
            p.debug.write(entry, bytes(0x200))
            rc = s32(native_call(p, D_READ, (fd & 0xffffffff, entry)))
            raw = bytes(p.debug.read(entry, 0x200))
            if rc:
                say(f'  end (rc {rc & 0xffffffff:#x}) after {i} entries')
                break
            p.log('dir-entry', index=i, hex=raw.rstrip(b'\0').hex())
            if i < args.raw_entries:
                say(f'  raw[{i}] {raw[:0x80].hex(" ", 4)}')
            say(f'  {i:3d}: {" | ".join(entry_names(raw)) or "(no name found)"}')
    finally:
        native_call(p, D_CLOSE, (fd & 0xffffffff,))


def stage_get(p, args):
    if not args.output:
        raise SystemExit('get needs --output (local file on this host)')
    base = scratch(p, args)
    path, done_at, buf = base + 0x10, base + 0x100, base + 0x1000
    put_path(p, path, args.path)
    fd = s32(native_call(p, F_OPEN, (path, 0, 0)))
    if fd < 0:
        raise SystemExit(f'open {args.path}: {fd:#x}')
    total, t0 = 0, time.monotonic()
    try:
        with open(args.output, 'xb') as out:
            while True:
                p.debug.write(done_at, bytes(4))
                rc = native_call(p, F_READ, (fd & 0xffffffff, buf, 0x10000, done_at)) & 0xffffffff
                done = p.u32(done_at)
                if rc or done > 0x10000:
                    raise SystemExit(f'read error rc {rc:#x} done {done} at {total}')
                if not done:
                    break
                data = bytearray()
                while len(data) < done:
                    data += p.debug.read(buf + len(data), min(1008, done - len(data)))
                out.write(data)
                total += done
                say(f'  {total} B, {total / (time.monotonic() - t0) / 1024:.0f} KiB/s')
                if args.limit and total >= args.limit:
                    break
    finally:
        native_call(p, F_CLOSE, (fd & 0xffffffff,))
    p.log('get', path=args.path, bytes=total, output=str(args.output))
    say(f'{args.path} -> {args.output} ({total} B)')


def stage_shell(p, args):
    """Run native shell lines in order (e.g. `audiocntdbg 1` = WM8962 register dump to
    A:\\CODECnn.TXT). Stops at the first failure; nothing is retried."""
    if not args.line:
        raise SystemExit('shell needs at least one --line')
    for line in args.line:
        say(f'$ {line}')
        out = p.shell(line, timeout_ms=args.write_timeout_ms)
        if out:
            say(out)
        say(f'  state={p.u32(STATE)}')


def stage_peek(p, args):
    """Read-only hex dump of one RAM range (refuses unmapped pages)."""
    if args.address is None or not 1 <= args.count <= 0x10000:
        raise SystemExit('peek needs --address and --count 1..0x10000')
    for page in range(args.address & ~0xfff, args.address + args.count, 0x1000):
        if p.debug.translate(page)['fault']:
            raise SystemExit(f'{page:#x} is not mapped')
    data = bytearray()
    while len(data) < args.count:
        data += p.debug.read(args.address + len(data), min(1008, args.count - len(data)))
    p.log('peek', address=f'0x{args.address:x}', hex=data.hex())
    for off in range(0, len(data), 16):
        row = data[off:off + 16]
        words = ' '.join(f'{struct.unpack_from("<Q", row.ljust(16, b"\0"), i)[0]:016x}' for i in (0, 8))
        say(f'{args.address + off:#x}  {words}')


def stage_analyze(args):
    path = Path(args.wav)
    with wave.open(str(path), 'rb') as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    say(f'{path.name}: {ch} ch, {8 * width}-bit, {rate} Hz, {n} frames = {n / rate:.2f} s')
    if width not in (2, 3):
        raise SystemExit('unexpected sample width')

    def sample(i):
        b = raw[i * width:(i + 1) * width]
        return int.from_bytes(b, 'little', signed=True)
    full = (1 << (8 * width - 1))
    block = rate // 2
    last_nonzero = -1
    rows = []
    for start in range(0, n, block):
        frames = range(start, min(n, start + block))
        peak = [0] * ch
        energy = [0.0] * ch
        nonzero = False
        for f in frames:
            for c in range(ch):
                v = sample(f * ch + c)
                if v:
                    nonzero = True
                peak[c] = max(peak[c], abs(v))
                energy[c] += v * v
        if nonzero:
            last_nonzero = start
        rms = [((e / len(frames)) ** 0.5) / full for e in energy]
        rows.append((start / rate, [p / full for p in peak], rms))
        if start / rate >= args.max_seconds:
            break
    for t, peak, rms in rows:
        db = ' '.join(f'{(20 * __import__("math").log10(r) if r else -999):6.1f}' for r in rms)
        say(f'  {t:6.1f}s peak={" ".join(f"{x:.3f}" for x in peak)} rms_dBFS={db}')
    if last_nonzero < 0:
        say('RESULT: all zero in the scanned range -> no microphone data reached the buffer')
    else:
        say(f'RESULT: nonzero audio up to ~{last_nonzero / rate + 0.5:.1f}s; listen to confirm it is the mic')


args_interval = 0.25


def main():
    global args_interval
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('stage', choices=['check', 'watch', 'record', 'pull', 'peek', 'shell', 'adc-test',
                                      'msg-test', 'snap', 'remote-rec', 'sample', 'heap-test', 'w5-plan', 'w5-up', 'pool-free', 'pool-cycle', 'perm', 'w5-cfg', 'w5-open', 'w5-hdr', 'w5-enc', 'w5-more', 'w5-reparam', 'w5-soak', 'w5-clip', 'dump', 'ls', 'get', 'props', 'selector', 'op', 'call', 'w5-state', 'w5-release', 'analyze'])
    ap.add_argument('--path', default='A:\\', help='ls/get: camera path, e.g. A:\\ or A:\\CODEC00.TXT')
    ap.add_argument('--max-entries', type=int, default=200)
    ap.add_argument('--raw-entries', type=int, default=2, help='ls: print raw bytes of the first N entries')
    ap.add_argument('--limit', type=int, default=0, help='get: stop after this many bytes (0 = whole file)')
    ap.add_argument('--output', type=Path, help='get: new local file')
    ap.add_argument('--label', help='snap/sample: output label')
    ap.add_argument('--no-exit', action='store_true', help='w5-up: leave Wave5 powered (no exit, arena kept)')
    ap.add_argument('--kbps', type=int, default=8000, help='w5-up: bitrate passed to bring-up (kbps)')
    ap.add_argument('--step', type=lambda v: int(v, 0), default=0x400000, help='sample: stride')
    ap.add_argument('--against', help='sample: earlier label to diff with')
    ap.add_argument('--arm', action='store_true', help='snap: wait for a recording, snap during and after it')
    ap.add_argument('--stop-timeout', type=float, default=180, help='snap --arm: give up waiting for the stop')
    ap.add_argument('--callback', type=lambda v: int(v, 0), default=0x40e3acbc,
                    help='msg-test: block callback address (default: stock SoundTest callback)')
    ap.add_argument('--adc-volume', type=lambda v: int(v, 0), default=0x1c0, help='adc-test: R21/R22 value')
    ap.add_argument('--line', action='append', help='shell: native command line (repeatable, run in order)')
    ap.add_argument('--address', type=lambda v: int(v, 0), help='peek: start address')
    ap.add_argument('--count', type=lambda v: int(v, 0), default=0x100, help='peek: bytes')
    ap.add_argument('wav', nargs='?', help='analyze: WAV file copied from the card')
    ap.add_argument('--fenestra-host', help='path to <fenestra>/host (default: FENESTRA_HOST or ~/Nikon/fenestra-z8/host)')
    ap.add_argument('--log-dir', type=Path, default=Path(__file__).resolve().parent / 'logs')
    ap.add_argument('--force', action='store_true', help='w5-release: allow in photo mode (playback test)')
    ap.add_argument('--read-only', action='store_true', help='check: skip the audiodbg 21 toggle')
    ap.add_argument('--seconds', type=float, default=3.0)
    ap.add_argument('--interval', type=float, default=0.25)
    ap.add_argument('--sub', type=int, default=0, help='record: second audiodbg 1 argument (meaning unknown, 0..3)')
    ap.add_argument('--depth', type=int, choices=(16, 24), default=16)
    ap.add_argument('--file-no', type=int, default=7)
    ap.add_argument('--shutter-at', type=float, help='record: prompt a full shutter press at this second')
    ap.add_argument('--no-write', action='store_true', help='record: skip audiodbg 20')
    ap.add_argument('--unmute', action='store_true', help='record: write WM8962 R21/R22 = --adc-volume after start')
    ap.add_argument('--yes', action='store_true', help='record: skip the confirmation prompt')
    ap.add_argument('--shell-timeout-ms', type=int, default=5000)
    ap.add_argument('--write-timeout-ms', type=int, default=30000)
    ap.add_argument('--max-seconds', type=float, default=130, help='analyze: stop after this many seconds')
    ap.add_argument('--fake', action='store_true', help=argparse.SUPPRESS)
    args = ap.parse_args()
    args_interval = args.interval
    if args.stage == 'analyze':
        if not args.wav:
            ap.error('analyze needs a WAV path')
        return stage_analyze(args)
    log = Log(args.log_dir, args.stage)
    say('log:', log.path)
    if args.fake:
        sys.path.insert(0, str(find_host(args.fenestra_host)))
        from fake_camera import FakeCamera
        transport = FakeCamera()
    else:
        transport = open_transport(find_host(args.fenestra_host), log)
    from ptp_client import Client
    with transport:
        client = Client(transport)
        probe = Probe(client, log, args.shell_timeout_ms)
        probe.handshake()
        try:
            {'check': stage_check, 'watch': stage_watch, 'record': stage_record,
             'pull': stage_pull, 'peek': stage_peek, 'shell': stage_shell,
             'adc-test': stage_adc_test, 'msg-test': stage_msg_test,
             'snap': stage_snap, 'remote-rec': stage_remote_rec, 'sample': stage_sample, 'heap-test': stage_heap_test, 'w5-plan': stage_w5_plan, 'w5-up': stage_w5_up, 'pool-free': stage_pool_free, 'pool-cycle': stage_pool_cycle, 'perm': stage_perm, 'w5-cfg': stage_w5_cfg, 'w5-open': stage_w5_open, 'w5-hdr': stage_w5_hdr, 'w5-enc': stage_w5_enc, 'w5-more': stage_w5_more, 'w5-reparam': stage_w5_reparam, 'w5-soak': stage_w5_soak, 'w5-clip': stage_w5_clip, 'dump': stage_dump, 'ls': stage_ls, 'get': stage_get, 'props': stage_props, 'selector': stage_selector, 'op': stage_op, 'call': stage_call, 'w5-state': stage_w5_state, 'w5-release': stage_w5_release}[args.stage](probe, args)
        except BaseException as exc:
            log('error', error=repr(exc))
            raise


if __name__ == '__main__':
    main()
