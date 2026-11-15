# v53 shutter adapter: source review complete, hardware pending

The source candidate is `work/` (the contents of `src-fix37`), based on delivered v52 G. The combined patch is `shutter-production.patch`, SHA-256 `de9f3c8532bcdfa5dc39b0388379c99f2699c7b9d9ac20dace52b8e4aeaa69b7`. `changed-files.json` fixes all nine old/new source identities and includes the final audio startup patch `f3465b009d14b134297298945959abb03877dc69734027b72f2abfc09862e94d`. The original source trees and physical camera/card were not modified.

## Behavior

- Hook only the original SFX task manager's tail branch at `0x40e30e3c` (expected bytes `3c0f0014`). The original dispatcher at `0x40e34b2c` remains callable. All held message completion and cancellation run on this original serialized task.
- Retain only original normal shutter tone 1/2, repeat 1, exact original constructor/callback envelope. Preserve user mute, stock mute, photo-mode, Live-request and stock-handoff gates. Other tones/messages pass to the original dispatcher.
- Live's worker closes microphone capture with its original asynchronous `108` protocol. It preserves the audio allocation, pre-roll, mark and absolute sample clock. It requires our own stop callback, idle native driver and drained entered callbacks before dispatching sound.
- A single persistent wake message includes a generation. A wake from a canceled prepare cannot submit the newer fire message. Native STOP of an unsubmitted held message completes the original failed-start callback sequence and cancellation cleanup once, without sending hardware STOP for that held start.
- Accepted `2004` is not completion. The wrapper forwards the original callback. It waits for native `2005`, native SFX idle, driver idle and cleared native pending before resuming capture. `2005` also occurs for rejection/cancel: it means the native operation ended, not proof sound was audible. The existing `played` diagnostic counter counts native submissions, not verified acoustic playback.
- Resume repeats GO and waits for its own ACK and a complete post-unmute PCM block. Only this explicit shutter pause authorizes zero-filled PCM between old and new absolute sample times. Startup missing history is never repaired with fabricated silence. The video clock and paired Apple marker are unchanged.
- Capture blocks are 2400 stereo s16 frames (50 ms) with 40 staging slots. Stage memory remains 384000 bytes (2 seconds), and the existing five-block native ring base/count remain unchanged. The 50 ms value is a buffering period, not a promised maximum sound delay or recording gap.

## Ownership and ABI

`sizeof(live_runtime)=23744`, formerly 23264. `live_audio_z8=1040`, formerly 744; appended shutter state is 184 bytes. The 296-byte audio backend increase comes from timestamps for 40 slots and explicit resume state. `w5_close_lock` is now at 19284; the private Wave5 static assertions were updated. Root must regenerate driver offsets and rebuild every source consumer.

The source has no new static writable state. Root owns the linker/builder and the `.live_shutter` bank in the existing PB payload reservation. Local actual object sizes are primary delta −16 bytes, `.live_extra` delta +224 bytes, `.live_shutter` 3856 bytes, relative to G and including the audio startup change. The final clang-21 F ELF/MAIN independently verifies these sizes and leaves 16 bytes in the shutter bank. All 406 immutable source identities were checked. Do not place a binary spanning a gap or overwrite size/meta helpers. The hook target must use the final ELF symbol, never the test emulator address.

## Validation

- `adapter-arm.json`: 31 cases compile the actual adapter as AArch64 and execute the delivered G manager, dispatcher, constructors, callbacks and heap cleanup with the exact proposed single branch. Live backend/registry and lower DAC operations are explicit shims. Cases cover software cancel, accepted-versus-done, stale wake, queue failure, timeout/off/handoff/mute, future timestamps, resume publication races and partial resume failure.
- `pause-tests.json`: 9 ASan/UBSan groups execute the actual backend and PCM core (only section attributes removed for host compilation). They preserve exact pre-roll/mark/allocation and a 300 ms explicit silence gap, stop ACK plus in-flight callback lifetime, cancellation, failed restart, 40-slot overflow, and freeze between resume and first valid PCM. A frozen mux ring is byte-stable, and drain retains the bridge until thaw.
- Independent audio review: `../../../audio/combined-tests/result.json` and `partial-resume-result.json`. Two pauses in one 3-second clip preserve exact PCM and two known gaps; partial restart plus a new held request emits STOP and retains allocation while an entered callback remains. The old STARTING-cleanup branch fails the same counterexample.
- Independent playback review under `../../../evf/shutter-review/`: future-time subtraction, partial-restart/new-holder cleanup and freeze-after-resume were independently reproduced and fixed. These were actual defects in earlier private review candidates, not waived checks.
- `../capture-blocks.json`: actual original 109/106/709/111 instructions propagate 50/100/250/500 ms sizes; native capture configuration is not fixed at 500 ms.
- `../capture-stop-order.json`: actual STOP705, original event worker, lower 70c/70f, and high 111/116 constructors/handlers order the pending full block and final partial block before our 108 ACK. The post-stop original ADC callback observes the stop flag and publishes no new event. Actual serial hardware drain is explicitly shimmed; this does not simulate an already-entered concurrent physical IRQ.

## Remaining physical boundary and minimum check

The copied G card logs contained no EV60 shutter events; they cover different short runtime histories. They neither prove nor refute the microphone gate for the user's affected exposures. Actual G instructions do reject normal shutter sound with driver state 5 and admit it with idle state 1, and the candidate implements that resource transfer. Whole-image QEMU behavioral results are owned by root and physical output is a separate check; this source review is not a claim of flash or acoustic success.

The precise gap's relation to physical exposure has not been measured. Prepare/fire requests originate from the original capture path, but their deferred playback is scheduled after microphone STOP ACK. Prepare may be canceled before worker pause; this uses the original cancellation contract so exposure is not held merely to insist both sounds play. If sound is submitted, its onset can lag the original event. The capture-ring timeline remains continuous using deliberate silence; some real ambient audio around the shutter is necessarily missing with this handoff design.

Minimum physical validation after root's final build/regression:

1. With the normal shutter sound profile and mute off, compare one Live Off exposure with one fully Ready Live On exposure. Verify sound, immediate still capture, saved Live and restored Ready; do not count a submission counter as audible sound.
2. Repeat with user mute on: no shutter sound and no unwanted speaker click. Restore mute off and take two quick exposures, then enter playback/movie mode while a Live is finishing; no pending sound, pinned mic or mode hang may remain.
3. Record continuous speech or steady external sound while taking one Live. Inspect the saved audio across the shutter and through its end: a bounded intentional silence gap may occur, but no repeated half-second block, extra silent prefix, truncation or timing shift. Compare audio/video durations and Apple pairing. If the sound/gap delay is objectionable, capture the smallest native event/ACK and waveform observation before widening the hook.

Physical ADC/DAC switch latency, codec settling/clicks, real speaker routing, already-entered IRQ timing and alternate shutter sound profiles remain unverified. No forced speaker gain, mute override, native success result, hardware completion, or shared driver reset is introduced.

## Final linked F audit

Final MAIN SHA-256 `b0cd7c40c3c6ffdf07b87fcbab20b98fbe9ed0c2a848550855d989b9e64fd8b0`, build ID `0x15cea168`. `review/binary-audit.json` independently reconstructs the complete image from v51d raw changes and every later overlay, then separately replays all 156 aggregate ranges from `base-c770f340`. Both match byte-for-byte. All 406 source files, every ELF alloc section, generated hook, 40-byte EVF core adapter, 419 v52 portrait records plus 77 v53 records, original font restoration, and the size/meta reservation pass. Region free space: primary 16, extra 12, PB 16, shutter 16 bytes. An official-container replacement/CRC round trip runs only in memory; no package or card is written.

Final-image ARM checks also pass: actual boot requests 23744 bytes; `40e30e3c` branches to `live_shutter_dispatch` at `4019f5b0`; passive prepare/fire wrappers retain their original stock targets. The relocated crop at `4019ff40` passes all 528 cases, and relocated aspect at `4019ffc0` passes all 8448 cases, including native NFP ratios, bounds, registers and literal/trampoline returns. These tests execute the final image bytes, not newly assembled copies. See `review/final-{abi,crop-test,aspect-result}.json`.
