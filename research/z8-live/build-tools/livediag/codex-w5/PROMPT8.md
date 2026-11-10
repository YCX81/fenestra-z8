Read-only root-cause analysis, in Chinese. Do not modify anything, no USB calls. Label: proven by disassembly / proven by hardware / inference. Read history/astra-*.md first (astra-bfz5f70qd.md is the latest on this crash). Files: bg34.main (base 0x40160000; v32 = v31 minus the worker close at mode switch, see v32_patch.py), src-fix31/, W5-BRINGUP.md, W5-INIT-REPORT.md, w5seq.py, w5drain.py.

All hardware outcomes so far, Live has been encoding in photo mode this boot, stream healthy:
A. v25/v29 USB-attached in movie mode, host runs the full sequence (STOP, normal trigger, drain trigger, worker 5, FINISH, EXIT), VDI 0, then unplug, REC: WORKS (twice: v25 and v29).
B. v31: worker runs the full sequence at the photo->movie switch, no USB: reboot at the switch, without REC.
C. v31 with release_req=1 written by USB in photo mode (worker close off, Live stops encoding), unplug, switch to movie: no reboot; waited; then re-attached USB in movie mode (reads failed with WPD errors then persistent PTP 0x2005, no data); unplug; REC: WORKS. Unknown whether a client bring-up/preempt closed the stream during this.
D. v32 (no worker close, preempt only), no USB, no Live shot this boot, switch, wait 10 s, REC: reboot at REC.
E. v32 with release_req=1 written by USB in photo mode, unplug in photo mode, NOT reconnected, switch, wait, REC: reboot at REC.
F. v29 and v25: REC reboot without any USB.
So the only successes had USB attached while in movie mode before REC. Live view is stopped while USB Debug is attached.

Evaluate this hypothesis and alternatives: the hand-back succeeds only when movie live view is not running, because EXIT's board sequence (0x402f3128-0x402f323c resets 0x43-0x4b, clocks 0x79-0x81, power, 0x26/0x27) or the VPU teardown disturbs running live view / other blocks; in C the USB attach in movie mode itself may have triggered some stock teardown or preempt while LV stopped. Note stock exit between recordings runs with movie LV on, so explain the difference: what does stock do differently before EXIT (0x40d0712c region, 0x40d07dec, LV/encoder feed stop, 0x402f58f4 etc.)? In D/E, preempt runs in the REC bring-up caller: which task, what locks/state, what has REC already started before calling bring-up at 0x40d07ba4 (e.g. encoder feed from LV into Wave5 buffers, IRQ targets), and what would the Live close break?
Also does USB attach run a stock path that releases Wave5 (event 33/34, USB mode, 0x402f2d58 or 0x402f3084 callers)? Find it if it exists: it may be the actual working recipe.

Then propose concrete options, ranked, each with exact addresses and what to verify. Include these:
1. Service-only close: instance FINISH then service close/deinit (0x40c3325c path, map clear 0x402f2bac) without the board-level part of EXIT, relying on stock bring-up re-running clocks/resets. Is this valid (does the bring-up tolerate already-on blocks)?
2. Hand back at a stock safe point: hook the stock place where LV is stopped or where stock itself would exit a previous session.
3. Live uses the arena stock uses so that the 'second open must pass same arena' check passes, sharing the service, without exit at all, or hand over only the stream.
4. Close while still in photo mode before the switch (is there any earlier signal), or stop using Wave5 when a dial/menu goes toward movie.
5. Running the sequence via a stock-owned task to mirror context.
For each, the smallest experiment; prefer no flash; say honestly if a flash is needed (USB Debug stops LV, so LV-running cases need firmware code; a RAM-armed variant may be possible). What instrumentation survives the reboot to give crash evidence next time (e.g. a record written before each stage to a RAM area that survives warm reset, or to card) so we stop guessing; does DRAM survive this reboot, and is there a stock crash log on the card or in NOR (NIKON folder, ERR files, panic/exception dump)?
