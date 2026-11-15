# Independent review of final F capture and SFX subchain

The enclosing `v53f-final-audio-capture-rec` run did **not** complete: it stopped at playback entry when an external PCM input was rejected after native microphone STOP changed the RX descriptor. REC was not requested. The original failed result and all observations are retained.

The executed MAIN is `b0cd7c40c3c6ffdf07b87fcbab20b98fbe9ed0c2a848550855d989b9e64fd8b0`, taken from `live_a.executed_main_sha256` and the input manifest. The top-level `firmware_sha256` describes the prepared stock image, not this override. The actual private emulator is `5fb67d5aa9d48a71f299967adbd84d46587502a23c846fd2b146c3b6408f36a4`. Startup resource `eabd3f39...` at `0x46460000` is explicitly synthetic: original MAIN tone1 PCM reused as a short tone2 resource. No physical tone2 resource or speaker behavior is implied.

At `step-108-full-press`, the actual runtime is audio_warm=0, audio.active=1, capture.phase=2, encoded=63. One transaction was saved. Independent ffmpeg decoding of saved MOV `734000a0ea7f9ba9ec06f1d1213b5f25ff424b9d45fca6e30b591909bcbf611a` succeeds for all 93 HEVC frames and all 148949 stereo PCM frames. Video duration is 3.103100 seconds and audio duration 3.103104 seconds: less than one 48 kHz sample difference.

Both SFX BUFFER requests contain 4800 bytes, not the prior zero length. The exported TX is exactly 9600 bytes and equals the official tone1 PCM twice, byte for byte (SHA-256 `180febf0932e6d29ef8e0064d692f0ad62f28640e42b1a75b01aec907cff068f`). In the passive original-service log, each native START is followed by driver block-done `42040110`, service block-done `4318001b`, native STOP/close, and microphone restart. First sequence is lines 381–399, second is 438–456 in `audio-events.jsonl=on`. `shutter.cancel_posted` remains zero; completed advances to 1 and 2, with no errors. This closes natural software completion for these admitted synthetic functional inputs; it is stronger than the completed counter alone and does not rely on the old two-second cancellation path. Snapshot IRQ timestamps are not exact call timestamps, so this report does not infer a precise physical sound duration from them.

Decoded PCM has just two continuous all-zero runs of at least 10 ms:

| PCM frames [first,end) | Clip-relative interval | Meaning |
| --- | --- | --- |
| 74459–81659 | 1.551229–1.701229 s (150 ms) | Exactly the existing press−50 ms to press+100 ms duck, within one sample. The first microphone pause fits within this already muted interval. |
| 89422–96083 | 1.862958–2.001729 s (138.771 ms) | Second SFX pause/resume bridge. Its end matches the recorded resume anchor 91255613 µs within one sample. |

PCM resumes for the final 1.101375 seconds; there is no long zero tail. The earlier Ready2 sample's 1.569479-second tail is absent under this explicit resource and model input. These gaps remain real missing recorded sound; the result does not claim gap-free audio or prove a physical ADC/DAC clock, routing, acoustic output, or true camera tone2 timbre/duration.

The external feeder still used 50 ms QOM intervals, while the existing guest counter converts QOM ns with divisor 984. Its cadence and STOP race are separate test-input issues under review. This successful capture/SFX subchain cannot substitute for the unfinished capture→REC→restored-Ready whole run.
