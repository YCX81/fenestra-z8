# V62 independent functional verification

This directory owns test-only inputs, a private QEMU derivative, and passive
observers. It does not change production firmware or access a camera/card.

## Sound evidence and model boundary

The V53 G independent media review measured actual TX DMA bytes, native
completion, microphone recovery, and movie PCM. Its two 4,800-byte sounds used
a synthetic tone2 resource and therefore did not establish the current camera's
tone10 path. V60 physical logs identify prepare/fire tone10; the old handoff
admitted only tone1/2.

The native tone10 asset is built into MAIN, copied by the stock initializer from
0x43fbb850 to 0xa15c1b200. Its getters return 28,800 bytes and 150 ms. It is
byte-identical in V61 and an independently decoded official C3.11 package:
SHA256 1a0a4f9641500aba12c1ade65b80fd1ab0494e107f08baf267d2cbd0dfb3a123.
See `native-sfx-assets.json` for all other finite resources and provenance.

`native-sfx-model.patch` extends the old strict 4,800-byte functional sink only
to the other exact native lengths 9,600, 19,200, and 28,800. It retains request27,
the exact serial format, source-memory bounds, no overlapping owner/bank,
budget, transfer tuple, completion IRQ ordering, and no-host-DAC-output guards.
The deadline is the exact length divided by 48,000 stereo S16 frames/s, with
the unchanged optional 984/1000 counter scale. Unsupported lengths fail closed.

The model source functions are extracted verbatim for the 104-case ASan/UBSan
test (`test_tx16.c`). Negative cases include adjacent unsupported lengths,
concurrent DMA ownership, bad format/flags/source, host output, failed source
read, and changed pending DMA. `tx16-model-tests.json` and
`model-manifest.json` record the result and hashes.

Private build: `/path/to/user/fxfix/v62-qemu-native-sfx-59f9/qemu-system-aarch64`,
SHA256 9804d5734610465613f80dc762dd3bef0307571b0b016f140cdec81252ba6123.
`qemu-build-receipt.json` verifies the shared dd4da891 executable stayed intact.

Whole-capture requires the complete G capture model rather than the newer RDW
playback profile: the latter lacks several capture properties. The same small
length patch was independently applied to G's pinned merged-counter model:
`/path/to/user/fxfix/v62-qemu-native-sfx-merged/qemu-system-aarch64`, SHA256
34059c8c62b3f32896b3d5685a461cd4da5a427ae5b9d3ba4e0f04318461f418.
`merged-native-sfx-model.patch`, `merged-model-manifest.json`, and
`merged-tx16-model-tests.json` cover the actual merged source functions: all 104
ASan/UBSan cases pass. `merged-qemu-build-receipt.json` confirms the shared
0f341f32 executable is unchanged. Every prepared capture-command property was
cross-checked against this binary's machine help, with zero unknown properties.

## Whole-image checks

1. Pin the final reviewed V62 MAIN and QEMU hashes. Generate runtime, PB, and UI
   offsets by compiling the exact source for AArch64, including sizes/widths.
   Regenerate passive observers against the selected model's public plugin API
   (QEMU 10.2.4 for complete capture, 11.1.1 for RDW native playback).
2. Use normal MCU menu inputs to select native Camera sounds → Type → Type B
   (setting 0x02006101=1), then Live On and its Shutter sound On/Off row.
   Shutter sound is a section heading in Camera sounds, not a child menu.
   No guest-RAM setting, Ready, completion, or tone injection is allowed.
3. Tone selection additionally depends on the stock rational exposure compare
   against 34/6: current value >= threshold uses10/10; lower uses11/12. Record
   the actual prepare payload, never silently substitute tone1 or tone2.
4. Wait for real RX PCM and video Ready, capture, and save. On must produce the
   exact actual source PCM at TX, native SFX completion/STOP, microphone restart,
   no pending shutter owner/errors, and a usable saved Live movie with audio.
   Off must produce no SFX jobs while recording and saving microphone audio.
5. Export QOM `serial5-tx-pcm-capture`, drain state, protocol events, guest
   checkpoints, virtual card files, and model trace. Preserve failure evidence.
6. Same saved picture must enter native Live playback, decode, return to its
   original photo, and resume actual Live Ready. Use current nb_* PB fields;
   old embedded-preview `frames` assertions do not cover V61/V62 native bridge.

The functional model proves firmware protocol and source consumption under
its explicit assumptions. It does not prove physical DAC volume, audible
speaker output, ADC/DAC transition clicks, or camera timing.

The first whole-capture runs (`v62-final-tone10-{on,off}-r1`, image004e881a)
reached Camera sounds but incorrectly chose that section heading. They were
stopped before FULL. They are retained as harness failures and make no capture
or firmware pass/fail claim. Actual rendered page712 rows established the
correct Type row; the adaptive driver now prioritizes the deepest real row.

## Menu checks

The photographs concern the video shooting menu row below Video file type,
whose right value is S-Log3, and its blank title/three choices. They do not show
the Live/slot-card page.

Require Chinese rendered screen evidence for the parent row and child title /
all three choices on the final MAIN. Verify selection/value correspondence and
return/reopen persistence. Text callback values, ID tables, and instruction
tests are supporting evidence; they alone do not prove actual glyph rendering.
