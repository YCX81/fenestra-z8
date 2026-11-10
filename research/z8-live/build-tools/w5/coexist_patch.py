#!/usr/bin/env python3
"""Wave5 sharing with stock movie REC / playback (2026-11-05).

Applied to a fresh src-w5 tree and relive_w5.py by setup_src.sh:
  * hooks.S: live_w5_bringup_entry at the stock bring-up 0x402f2d58 (first instruction
    `sub sp,sp,#0x80` replaced by a branch; the trampoline replays it and resumes at +4).
  * runtime.c: live_w5_bringup_notify(client) -> live_encoder_preempt(&stream.encoder).
  * live_codec.h: LIVE_ENCODER_BUSY + live_encoder_preempt prototype.
  * live_stream.c: LIVE_ENCODER_BUSY drops the frame instead of failing Live.
  * relive_w5.py: new hook site + resume symbol + manifest range for the site.

usage: coexist_patch.py SRC_W5_DIR RELIVE_W5_PY"""
import sys
from pathlib import Path

src, relive = Path(sys.argv[1]), Path(sys.argv[2])


def patch(path, old, new, count=1):
    t = path.read_text()
    assert t.count(old) == count, f'{path}: expected {count}x {old[:70]!r}'
    path.write_text(t.replace(old, new))


patch(src / 'codec/live_codec.h',
      'int live_encoder_sizes(unsigned w,unsigned h,size_t *persist,size_t *scratch);',
      '#define LIVE_ENCODER_BUSY (-30)   /* Wave5 held by a stock user: drop the frame, retry later */\n'
      'int live_encoder_sizes(unsigned w,unsigned h,size_t *persist,size_t *scratch);\n'
      'void live_encoder_preempt(live_encoder *,uint32_t client);')

patch(src / 'capture/live_stream.c',
      ' if(rc){s->error=-rc;STORE(&c->phase,LA_FAILED);return;}',
      ' if(rc==LIVE_ENCODER_BUSY){c->dropped++;s->copy_dropped++;goto finish;}\n'
      ' if(rc){s->error=-rc;STORE(&c->phase,LA_FAILED);return;}')

patch(src / 'runtime.c',
      'static void live_try_encode(struct live_runtime *r);\n',
      'static void live_try_encode(struct live_runtime *r);\n'
      '/* Stock Wave5 bring-up hook (0x402f2d58), caller task: give Wave5 back to movie/playback. */\n'
      'void live_w5_bringup_notify(uint32_t client) {\n'
      ' struct live_runtime *r=state();if(r)live_encoder_preempt(&r->stream.encoder,client);\n'
      '}\n')

# Live audio holds the audio service in its capture state (5) via the SoundTest messages.
# Playback, voice memo, menus etc. stop photo live view but keep the photo flag, so audio
# also requires a live-view frame in the last 1.5 s, or a clip being cut (press -> mux),
# which keeps audio across shutter blackout and long exposures after the press.
patch(src / 'runtime.c',
      '  live_audio_step(r,photo && !LOAD(&r->shutdown) && LOAD(&r->requested),now_us());',
      '  uint64_t t=now_us(),lv=LOAD(&r->capture.irq_timestamp);\n'
      '  int lv_on=lv && t>=lv && t-lv<UINT64_C(1500000);\n'
      '  live_audio_step(r,photo && !LOAD(&r->shutdown) && LOAD(&r->requested) && (lv_on || LOAD(&r->stream.job)),t);')

with open(src / 'hooks.S', 'a') as f:
    f.write('''.global live_w5_bringup_entry
live_w5_bringup_entry:
 save_all
 bl live_w5_bringup_notify
 restore_all
 sub sp,sp,#0x80
 b live_w5_bringup_resume
''')

patch(relive,
      "         0x40ea3638: ('live_writer_wrap', True), 0x406cd468: ('live_open_wrap', True)}",
      "         0x40ea3638: ('live_writer_wrap', True), 0x406cd468: ('live_open_wrap', True),\n"
      "         0x402f2d58: ('live_w5_bringup_entry', False)}\n"
      "W5_SITE, W5_ORIG = 0x402f2d58, bytes.fromhex('ff0302d1')   # sub sp,sp,#0x80")
patch(relive,
      "            live_old_pool=target(0x40e8b328))",
      "            live_old_pool=target(0x40e8b328), live_w5_bringup_resume=W5_SITE + 4)")
patch(relive,
      "cur = lambda at, n: main[at - LOAD:at - LOAD + n]",
      "cur = lambda at, n: main[at - LOAD:at - LOAD + n]\n"
      "assert cur(W5_SITE, 4) in (W5_ORIG, hooks[W5_SITE]), cur(W5_SITE, 4).hex()")
patch(relive,
      "new_manifest['ranges'] = [updated(r) for r in manifest['ranges']]",
      "new_manifest['ranges'] = [updated(r) for r in manifest['ranges']]\n"
      "if W5_SITE not in ranges:\n"
      "    new_manifest['ranges'].append(dict(address=hex(W5_SITE), size=4,\n"
      "                                       purpose='live_w5_bringup_entry (Wave5 sharing)', expected=W5_ORIG.hex(),\n"
      "                                       replacement=hooks[W5_SITE].hex()))")
print('coexist patch applied')
