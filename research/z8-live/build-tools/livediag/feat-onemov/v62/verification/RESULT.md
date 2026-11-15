# V62 functional audio verification

Target image: `36c564c9e785822ccf784816882d5055ee8e5ca0b796e1c4fb8e459dfc1ae4b5`.
All production code changes belong to the parent/shutter/menu tasks. This
directory only owns test drivers, isolated QEMU derivatives and evidence.

## Completed native tone10 case

`capture-on-tone10-r3.json` records `v62-36c5-tone10-on-r3` on the exact final
image. An ordinary MODE/Main-minus MCU chord visibly entered M, with 1/32000
shown on the shooting screen (`r3-mode-exposure.png`). The native Type B menu
then selected the actual tone10 branch.

- The actual consumed DMA is exactly 28,800 bytes from `0xa15c1b200`, SHA256
  `1a0a4f9641500aba12c1ade65b80fd1ab0494e107f08baf267d2cbd0dfb3a123`, identical
  to the official tone10 asset. No synthetic tone2 fixture is loaded.
- One natural completion/STOP/close chain is recorded at protocol event lines
  236→238→242→243→244→247→248. There are no shutter errors or cancellation.
- After successful saving, nine actual microphone/Ready checkpoints continue
  encoding from 193 to 993 frames. `LIVE-Z8.1` persists Live1/mute0 with valid CRC.
- New MOV SHA256
  `02f1a253585e9a8a8c2e50546c153c3d5e51755595eaf55f4fb3cce874e0ba03` contains 92
  video frames and 147,347 stereo 48k audio frames; complete ffmpeg decode is
  clean and the V60 frame-count guard is retained.
- The shared-codec handoff produces one measured 0.25675s silence interval in
  this model's saved audio. This result does not establish gapless audio or
  physical speaker loudness.

The run exits 0 and all 4,617 actual native assertions pass. Its closed card was
privately snapshotted by the menu agent. No additional replay of this third card
is planned: the independent replay scope is On-r2 and Off-r4.

## Completed native Type B paired-sound case

`capture-on-paired-r2.json` records the completed whole-image run
`v62-36c5-tone10-on-r2`. The directory name was the intended target; actual
camera UI fell back from S to A without an electronic lens, and the firmware
selected **tone11/12**, not tone10. The report labels that distinction.

- 4,127 native harness assertions passed; no runner error.
- Ordinary MCU inputs selected Camera sounds → Type → Type B, Live On and
  Shutter sound On. No sound-resource fixture or guest-RAM setting was injected.
- Actual TX DMA consumed 9,600 bytes at `0xa15c22280`, then 19,200 bytes at
  `0xa15c24800`. The concatenated 28,800 bytes match the two independently
  verified official MAIN sample assets exactly.
- Each sound naturally completed the native service chain through completion,
  STOP and close acknowledgement. No cancellation timeout or shutter error.
- The photo and new Live movie saved successfully. After the second completion
  and save, five checkpoints show microphone driver5, active PCM, warm0,
  capture phase2 and no stream job/error, with encoding continuing from398 to
  1,213 frames. The original driver then idles long enough to enter native
  standby. The final idle state is preserved, not mislabeled Ready.
- New `DSC_0001.MOV`: SHA256
  `21142258ce8c114d1391f72452657a645990a8a31239665041a5813d04977b19`,
  92 video frames, 147,347 stereo48k audio frames, complete clean ffmpeg decode.
  The frame count respects the retained V60 3k+1 avoidance rule.
- PCM has two measured silence intervals of99.42ms and154.88ms. This is the
  shared-codec microphone handoff cost in this functional test, not gapless
  simultaneous recording/output.

The menu agent completed independent RDW native MOV playback using a fresh
private copy. The original 16 summary checks and 237 native assertions passed,
including native entry, return to the original photo and temporary MOV cleanup.
The final frame-by-frame CRC audit narrows the original event-count claim:
92 native output events contain a repeated first frame and source frames 0..90;
source frame 91 was not output. Therefore this run does not establish complete
native playback coverage of its 92-frame movie. The file itself fully decodes
with ffmpeg as recorded above. This is a reboot of the same newly saved card,
not capture and playback in one boot. Source card size and mtime were unchanged.

## Completed Off control

`capture-off-r4.json` records `v62-36c5-tone10-off-r4` on the same final image.
Ordinary MCU inputs selected M and native Type B, then enabled Live and changed
Live Shutter sound from On to Off before FULL. The page/selection observations
and persistent configuration establish that the requested setting was reached.

- All 4,170 original native assertions passed, including the unchanged host-MCU
  ACK-high assertion. The runner exits 0 without error.
- Actual SFX TX contains zero bytes, no SFX jobs and no native SFX START.
  Shutter captured/played/completed/errors/cancelled counters all remain zero.
- One photo/Live save completes with transaction error 0. Six post-save Ready
  checkpoints retain driver 5, active microphone, warm 0 and capture phase 2,
  with encoding progressing from 244 to 862 frames. Final state remains Ready.
- Latest `LIVE-Z8.0` generation 2 persists Live 1 / mute 1 with valid CRC,
  SHA256 `3cc43478230f0d6ead3dea5acb13266b3332320cbc047ba9d5892e7b787c96bd`.
- New `DSC_0001.MOV` SHA256
  `9415a5749e2e302e44b75d2376846bb4432246df4051ed2975ea3235efade21c`
  contains 93 video frames and 148,949 stereo 48k audio frames. Complete ffmpeg
  decode is clean, audio is nonempty, no silence interval reaches 10 ms, and
  the retained V60 frame-count guard passes.

Only this private driver's planned post-photo idle was shortened from 57 to
27 seconds, allowing its final checkpoint to precede native standby. Firmware,
UI assertions and transport assertions were unchanged. This timing difference
is explicit in each capture receipt and is not treated as a sound-On/Off
firmware difference. The menu agent completed native replay from a private
snapshot of the closed card: all 16 pathway checks and 235 native assertions
passed, including return and cleanup. Its final CRC audit verified all source
frames 0..92: 94 native output events contain one repeated first frame and all
93 distinct source frames. Complete native frame coverage is established for
this Off-r4 movie. This separate replay rebooted the saved card; no same-boot
capture/replay claim is made. Evidence is independently owned by the menu agent.

## Model verification

Both isolated model derivatives pass104 extracted-function ASan/UBSan cases.
The actual capture uses QEMU
`34059c8c62b3f32896b3d5685a461cd4da5a427ae5b9d3ba4e0f04318461f418`.
It adds only exact native finite SFX lengths/times to the G capture model,
retaining strict DMA tuple, source, format, concurrency, budget and completion
guards. Shared QEMU binaries are unchanged. See `PLAN.md` and the model/build
receipts for provenance and boundaries.

## Retained harness limitations and retries

- r1 never sent FULL: the driver initially treated the Camera sounds section
  heading as an actionable submenu. Actual row inspection corrected it.
- Off-r2 never sent FULL: a menu reopen was missed and the test explicitly
  refused to claim that the sound toggle was selected. The adaptive navigator
  now reopens from the observed page and re-enters Live after On returns to its
  parent before selecting the sound row.
- Off-r3 never sent FULL: the existing host-MCU ACK-high assertion failed during
  Setup navigation before Live was enabled. Off-r4 completed serially with all
  original assertions intact. The ACK-high cause remains unresolved; a passing
  retry does not establish parallel host load as its cause.
- The IPC audit found unique named QMP/qtest/GDB FIFOs and separate virtual
  cards for each run, with no shared MCU socket or TCP port. The successful
  Off-r4 had no other task-owned QEMU running; an unrelated pre-existing QEMU
  was observed and left untouched. See `ipc-and-retries.json`.

Physical speaker loudness, codec clicks and real camera timing remain outside
these functional-model and byte-consumption results. No camera, USB operation,
physical card write, firmware install or flash was performed by this task.

All capture processes owned by this verification task have exited. Virtual
cards are closed; no further capture or model tests are planned. The
`verification.json` index binds successful capture reviews, source evidence,
private model receipts, retry records and this report. The summary is limited
to the checks stated here, and does not turn retained harness failures into
passing runs.
