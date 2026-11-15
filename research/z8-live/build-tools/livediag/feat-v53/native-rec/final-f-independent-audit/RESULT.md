# Exact-F independent acceptance boundary

The tested firmware is MAIN `b0cd7c40c3c6ffdf07b87fcbab20b98fbe9ed0c2a848550855d989b9e64fd8b0`, loaded from `/path/to/user/fxfix/v53-root/v53f/q/main.main`. The complete task is **not yet accepted** by these receipts.

## REC-only run with actual modeled TX reads

Run: `/path/to/user/fxfix/v53-audio/rec-tx16-f/run-rec`. Private rdw binary `c1e0b9b6f59e0cdd61d68c5b49eaef66278fed7dad6655b90e1453888989d036`.

Independent byte/hash checking confirms four consumed 192000-byte blocks: 768000 bytes total, SHA-256 `486122a8fff0e63257ed001b175e8dd8ed83bce28cfd31a1c62ea7cd71c52682`. The first 608608 bytes exactly match the decoded source PCM; the remaining 159392 bytes are zero. Native DMA descriptors progress through successive ring sources and refill via requests 27/28. Final TX state has four jobs, no pending descriptor and no fault. PB finishes by its own duration with `playing=0`, `audio_stage=0`, `audio_gate=0`, `audio_errors=0`; 25 rendered frames, not all 45 JPEGs, are recorded.

This establishes submitted nonzero audio, functional device RAM consumption, native IRQ/refill and cleanup for that run. It does not establish a physical DAC, speaker, acoustic volume or measured hardware rate. The declared functional clock uses an initial 1 ms prefill, followed by one second per 192000-byte block. No host audio backend was active.

The full runner is **235/236**. It still fails `native run has no unmodeled MMIO` at write `0x0d154340 = 0x01ffffff`, PC `0x08201164`. PB had already reached its automatic end, but the precise dynamic reason for entering the later global service path is not established here. Parent's static investigation identifies a model dispatcher conflict; that has not been suppressed or treated as an acceptance pass.

## Ready2 full capture

Run: `/path/to/user/.local/share/nikon-z8-qemu/live-audio/regress/runs/v53f-audio-pcm-ready2-capture`. Private merged binary `cb696539bf7f2ca233cde1363c494fb2599a7b48139c6c7f73862503a1d07bee`.

JPG, MOV and MAN were committed with `transaction_count=1`, `transaction_error=0`, `writer_returns=1`. MOV SHA-256 `31b9e74204c25261149203e40463d4ecd605e73f83ffbf90510a9c8a1dc2f0fa`: 96 HEVC frames, 1024×682, duration 3.203200 seconds; 153753 stereo PCM frames at 48 kHz, duration 3.203187 seconds. Independent complete video-plus-audio FFmpeg decode returns 0 with no diagnostic output.

The extracted PCM has no initial silence but **75335 trailing zero frames (1.569479 seconds)**. The second SFX input was zero bytes; the original constructor emitted a 262144-byte underflow descriptor, and the original two-second cancellation path ran. `shutter.completed=2` is a terminal counter and cannot override that event evidence to prove two normally played sounds. The missing tone-2 input and sound lifecycle are undergoing a separate explicitly synthetic-resource test.

After saving, video encoding resumed to 244 frames from snapshot 111, with `stream.job=0`, `stream.error=0`, `crumb_pad=0`. Microphone state is active/unmuted and has 215 blocks with zero drops, but **`audio_warm=1` at the last snapshot**. Thus video pre-recording recovered; final full audio/video Ready was not demonstrated by this run.

All 2386 recorded checks pass, but the runner terminates with `PCM feeder missed an entire50ms block`. This remains a failed full runner. Its stale `firmware_sha256=2b43…` label describes prepared stock material, not the actually loaded image: command kernel and `executed_main_sha256` identify exact F `b0cd…`. Original results are preserved unchanged.

## Next validation

The reviewed merged TX16/SFX v2 candidate is private binary `5fb67d5a…`, with exact observed format gating, descriptor-bound deadlines, original DMA IRQs, and explicit synthetic tone-2 ROM input. Audio owns the complete capture → REC → Ready run. Its eventual evidence must separately show normal SFX completion, microphone resumption, no long zero tail, REC consumption and final Ready; the above earlier failures cannot be relabeled as passes.

## First merged-v2 full lane: additional results

Run `v53f-final-audio-capture-rec` reached Ready and completed the two sound subchains. The actual TX capture is 9600 bytes: both 4800-byte halves independently hash `38299bea8895a8cbea19c72f6a741b5908e2d6691c7b6525f6370510c625d10a`, identical to the native tone-1 bytes and the explicitly synthetic tone-2 copy. Original driver event `42040110` leads to service `4318001b`, followed by native `16/1c` cleanup and microphone START. Both `cancel_posted` values remain zero. This is actual modeled source consumption and native normal completion evidence, rather than the earlier two-second cancellation.

The newly saved MOV is 1825549 bytes, SHA-256 `734000a0ea7f9ba9ec06f1d1213b5f25ff424b9d45fca6e30b591909bcbf611a`. Independent full video and audio decoding both return 0 without errors. Its audio has 148949 frames and **zero trailing all-zero frames**. The old 1.569479-second silent tail is absent in this artifact.

The full lane still fails: after 2520 passing checks, external PCM input is rejected because it no longer matches the enabled RX bank. REC and final Ready were not reached. No model guard or firmware assertion was relaxed. Audio owns the input/transition investigation. These new sound and media results do not convert this full runner into a pass.

Independent clock review confirms that the current QEMU integer 24 MHz timer period is 41 ns. The firmware clock therefore advances at `125/123` times QOM time; a 50 ms guest audio block corresponds to 49.2 ms QOM time. This external input clock-domain mismatch can cause sustained lag but does not explain away real shutter pauses or authorize ignoring input failures. See `clock-domain-review.json` for the exact scope and the remaining declared TX-clock difference.

## Counter-domain input run: product duration defect

Run `v53f-final-counter-audio-capture-rec` passes all 3147 runner checks with no runner exception, presents 45 REC frames and returns to audio/video Ready (`audio_warm=0`, audio active, capture phase 2, encoded 1074). Nevertheless **F is rejected** by independent payload verification.

The actual MOV (`7dcea6e0…`) retains LVJ1 header `45 × 66667 = 3000015 us`, but its legitimate audio record contains 147347 frames at 48 kHz: 3.069729167 seconds, or 589388 bytes after S24→S16 conversion. Actual modeled REC TX contains only 576000 bytes, exactly the matching source prefix; 13388 source bytes (3347 frames, 69.729167 ms of declared PCM) were not consumed by this model before the preview's early STOP. This byte deficit is functional atomic-DMA evidence, not a measured physical FIFO/DAC loss amount. Independently, the source proves `frame()` reaches the fixed image EOS and calls `hide()`, setting `audio_stop`, before the audio's declared endpoint. It is a product timing defect rather than permission to shorten the fixture.

`final-core-review.json` now binds this failed counter summary (`01546f884d9232646c24272628e93cb057b69bee2c24baef49a31d925c315083`) with `passed=false`. The prior clock1 review remains separate. The minimal PB correction has passed 217 actual-C combinations; new writer duration and the consistent counter-domain peripheral-clock model remain separate candidates requiring final G integration and complete-run receipts.
