#!/usr/bin/env python3
"""Field offsets of the v37 heap objects for the read-only QEMU driver.
Compiles two tiny AArch64 objects that include the real sources and reads the
resulting tables back; nothing is linked into the firmware.
usage: offsets.py SRC_DIR PB_DIR OUT.json"""
import json, struct, subprocess, sys, tempfile
from pathlib import Path

src, pb, out = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve(), Path(sys.argv[3]).resolve()
LIVE = ['requested', 'task_error', 'transaction_count', 'transaction_error', 'capture.phase', 'stream.job',
        'stream.frozen_count', 'stream.encoded', 'stream.dcim_copy', 'stream.dcim_copies', 'stream.error',
        'stream.cleanup_failed', 'stream.tune.icon_refresh_ms', 'capture.irq_seq', 'capture.copied', 'capture.dropped',
        'capture.reason'] + ['stream.diag.' + k for k in ('steps', 'ineligible', 'new_seq', 'stale', 'interval', 'lease_fail', 'geometry',
        'valid1_fail', 'convert_fail', 'valid2_fail', 'seq_changed', 'cancelled', 'busy', 'ok', 'enc_error', 'alloc_retry',
        'width', 'height')] + ['stream.dup_skipped', 'stream.cached_lv_fail', 'stream.cached_work_fail', 'stream.fr_dropped', 'stream.allocation_requested', 'stream.fr_alloc_fail', 'capture.actual_bytes', 'stream.encoder.hw']
UI = ['engine', 'display_status', 'display.attached', 'display.frames', 'display.error', 'presents', 'refreshes',
      'present_fail', 'not_current', 'icon_retry', 'icon_fail_streak', 'icon_heals']
PB = ['error', 'started', 'playing', 'frames', 'cache_valid', 'current_id', 'blocked_key_epoch', 'lookup_error',
      'rendered_frames', 'ui_presented', 'ui_detached', 'consumed_rec', 'key_presses', 'clip_mode', 'decode_errors',
      'badge_heals', 'audio_stage', 'audio_errors', 'audio_plays', 'audio_bytes', 'preview.frames', 'preview.width', 'display.display.attached', 'native_path']


def table(code, flags, cwd):
    with tempfile.TemporaryDirectory() as tmp:
        c, o, b = Path(tmp) / 't.c', Path(tmp) / 't.o', Path(tmp) / 't.bin'
        c.write_text(code)
        subprocess.run(['clang', '--target=aarch64-none-elf', '-Os', '-ffreestanding', '-fno-builtin', '-mgeneral-regs-only',
                        '-mstrict-align', '-w', *flags, '-c', str(c), '-o', str(o)], check=True, cwd=cwd)
        subprocess.run(['llvm-objcopy', '-O', 'binary', '--only-section=.v37table', str(o), str(b)], check=True)
        data = b.read_bytes()
        return struct.unpack('<%dI' % (len(data) // 4), data)


live_flags = ['-DLA_WIDTH=1024', '-DLA_HEIGHT=680', '-DLA_RING_CAPACITY=96', '-DLA_SAMPLE_INTERVAL_US=25000',
              '-DLIVE_BITRATE=8000000', '-DLIVE_IDR_US=500000', '-DLIVE_ENCODE_SPEED=9', '-I', str(src),
              '-I', str(src / 'ui/core/include'), '-I', str(src / 'codec/freestanding')]
live = table('#include "%s"\n__attribute__((section(".v37table"),used)) const uint32_t t[]={sizeof(struct live_runtime),%s,sizeof(UiState),%s};\n'
             % (src / 'ui/live_ui.c', ','.join('offsetof(struct live_runtime,%s)' % f for f in LIVE),
                ','.join('offsetof(UiState,%s)' % f for f in UI)), live_flags, src)
pbt = table('#include "%s"\n__attribute__((section(".v37table"),used)) const uint32_t t[]={sizeof(PbRuntime),%s};\n'
            % (pb / 'runtime.c', ','.join('offsetof(PbRuntime,%s)' % f for f in PB)),
            ['-I', str(pb / 'core-include'), '-I', str(pb)], pb)
result = dict(live=dict(bytes=live[0], **dict(zip(LIVE, live[1:1 + len(LIVE)]))),
              ui=dict(bytes=live[1 + len(LIVE)], **dict(zip(UI, live[2 + len(LIVE):]))),
              pb=dict(bytes=pbt[0], **dict(zip(PB, pbt[1:]))))
out.write_text(json.dumps(result, indent=1) + '\n')
print(json.dumps({k: v['bytes'] for k, v in result.items()}))
