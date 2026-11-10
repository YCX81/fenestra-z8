# Task: plan A v4 — full B1 1024x680, encode-as-you-go on CPU2, compressed pre-roll ring

Write `REPORT.md` here, in Chinese, concise: changes, numbers, QEMU evidence (run names, screenshots, media + ffprobe/exiftool), unproven items. Emulation only: no update container, no flashing, no commit/push. Do not stop early: finish implementation, static/host tests and the QEMU batch before writing the final report; a partial report only if a hard error blocks you, named exactly.

## Start point
Read ../live-a-v3/REPORT.md. Copy ../live-a-v3 sources (build.py, hooks.S, runtime, capture, codec, ui) here and change the copy. QEMU binary: /path/to/user/.local/share/nikon-z8-qemu/live-a-v2-qemu/build/qemu-system-aarch64 (do not rebuild shared binaries; ../qemu-port is working on a separate build). Driver: ../live-a-driver (as used by v3, incl. the status-block dump).

## Design (decided)
1. Source: B1 at its native size (1024x680 in run07/08; read geometry at runtime) with the v2/v3 best-effort reader. No downscale (crop/pad to the encoder's 16-pixel macroblock grid as needed: 1024x688 with const_input_flag / cropping in SPS, as noted in ../verify-a).
2. Encode-as-you-go: the CPU2 worker (processor=3, affinity=4, low priority) converts each accepted frame NV12 -> I420 (or feed NV12 if minih264 supports it) and encodes immediately with minih264 Baseline, IDR every 0.5 s (configurable), rate control targeting ~6-8 Mbit/s (configurable). Store access units in a compressed ring sized for >= 2.5 s of bitstream at the max bitrate plus margin (a few MB) — allocate it when Live is turned on, from the General heap or the class used by v3, free when off.
3. On the full press: the clip starts at the latest IDR at or before press-1.5 s; keep encoding until press+1.5 s; then freeze those AUs and mux the MOV (still-image-time at the press frame, identifiers, /LIVE/<DCF folder>/<stem>/{PAIR.JPG,CLIP.MOV,MANIFEST} as in v3) after the native still is closed. Encoding of new frames for the next shot continues; a second press during the post window shares history as far as possible or is ignored for Live (document).
4. Backpressure: if the encoder falls behind (queue > N frames), drop frames (keep timestamps correct) and record the effective fps; never block the LV callback or the native shutter. If sustained fps < 15, report it.
5. Keep the encoder behind the existing interface (frames in, Annex-B AUs out) so a Wave5 backend can replace it.
6. Status block: frames captured / encoded / dropped, encode time per frame (avg/max, guest time), effective fps, bitstream ring fill.

## Numbers to report
Per-frame encode cost in QEMU (guest time and, if available, guest instruction count via icount/plugin); a hardware estimate with stated assumptions (core type/frequency unknown — use ranges and say so). RAM used vs. v3.

## Test (fix first; ONE QEMU batch, at most one retry)
- `v4-on`: Live on, >= 5 s LV, one full press, wait for completion: native JPG 8256x5504 + /LIVE/ PAIR.JPG + CLIP.MOV; ffprobe: 1024 wide, frame count, duration ~3 s (starting at an IDR), bitrate/size, frames differ; still-image-time and identifiers match; status block numbers.
- `v4-off`: no extra files; native pixels equal the control.
- `v4-two`: two single presses 2 s apart (the 0.3 s double press is a known QEMU gap): both native stills; report Live results.
QEMU rules: count by free memory (~2.5 GB each incl. other agents, max 10, 90 s apart), FX_ICOUNT_SLEEP=off for menu steps, `cp --sparse=always`, WSL ext4, check `df -h /mnt/c`, stop if C: < 30 GB, delete big raw/bin after, keep logs/JSON/PNG and media. Python /path/to/user/.local/share/nikon-z8-qemu/integration-20261014/venv/bin/python -B. Never name a file dis.py.

## Coordinator notes
A previous attempt stopped early (see ./terra-attempt/REPORT.md). Its useful parts are already in this directory: the copied v3 sources, `codec/au_ring.c/.h` (byte ring with wrap, eviction, last-IDR-at-or-before lookup; host tests pass; not yet wired in, and it lacks protection of a frozen clip from eviction) and the fixed `native_adapter_test.py` offset. Build on them. Remaining: everything in "Design" 1-6, then the batch. Do not stop before the batch has run.
