Coordinator (user approved "rerun"): new round with a fresh allowance of ONE QEMU batch (+ at most one retry). Keep REPORT.md's previous results; add a new section "v4 rerun" and update the conclusion.

1. Switch to the new QEMU binary /path/to/user/.local/share/nikon-z8-qemu/qemu-port/build/qemu-system-aarch64 (sha 2c43029b...). It contains qemu-v2.patch plus the ported 153 alternate bank, D980 fresh-submission rearm, deferred JPEG source binding and the playback renderer (see ../qemu-port/REPORT.md); repeated captures ~2 s apart should now be modelled. Do not rebuild it.
2. Encoder quality: minih264 speed 10 disables deblocking (minih264e.h ~11517). Switch the default to speed 9 (deblock on) via build.py, keep 8 Mbit/s and 0.5 s IDR. Report the per-frame guest encode time change vs. speed 10.
3. Batch (parallel, 90-100 s apart):
   - `v4b-on-1`, `v4b-on-2`: Live on, one press each (same recipe as v4-on).
   - `v4b-off-1`, `v4b-off-2`: Live off, same recipe: this tells whether the "LV stops before the press" seen in v4-two / v4-retry-on also happens without Live (QEMU flakiness) or only with Live.
   - `v4b-two`: Live on, two presses ~2 s apart: both native stills must be saved; second Live ignored by design is fine.
   For any LV stop, record where the 1542 completion IRQs stop (irq_seq, last PC/events) and whether our hooks were active, and compare on/off.
4. If the LV stop happens only with Live on, find the cause (best-effort reader timing, cache maintenance, worker priority, allocator) and fix it in this round before the retry batch.
