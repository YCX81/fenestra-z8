# Private TX16 / SFX observer

The reviewed candidate is **merged-v2**, not the initial `merged` build. It changes the private QEMU model only. Production F MAIN remains `b0cd7c40c3c6ffdf07b87fcbab20b98fbe9ed0c2a848550855d989b9e64fd8b0`.

Remote executable: `/path/to/user/fxfix/v53-tx16-sfx-merged-v2/qemu-system-aarch64`

SHA-256: `5fb67d5aa9d48a71f299967adbd84d46587502a23c846fd2b146c3b6408f36a4`

Add three explicit properties to the existing merged core profile:

```
serial5-tx-drain-model=on
serial5-tx16-ring-model=on
serial5-tx16-sfx-model=on
```

No host-audio backend or additional playback-aperture property is introduced. Existing SPI/RX16 inputs remain required. No public source, object or executable was overwritten. The original merged executable still hashes `5ccbfc7e051a78dd24176aec98b3ab767709bc078dd3e179923c74b2f50a2ac2`; the original rdw executable remains `a88a921c9759deced131e372b2c9881b1f648690cfa306b51f3814b0d7961e5d`.

## Admitted contracts

- Serial channel 0 control must be exactly `0x450a2000`, as observed during actual Ready2 SFX in `audio-peer.jsonl` lines 92–93. The original exact-F ARM start and stop functions preserve format bits while setting/clearing TX bit `0x2000`. This is a functional model declaration, not a physical clock or sample-format measurement.
- REC: two independent request banks 27/28, physical FIFO `0x0e200104`, exactly 192000 bytes per mapped source, no duplicate or overlapping active banks. One initial 1 ms prefill then one second per 192000-byte block represents explicitly assumed 48 kHz stereo S16.
- SFX: one request 27 only, same FIFO, exactly 4800 bytes. A 25 ms functional deadline is used. A second active bank, mixed owner, unexpected control, or unexpected length is rejected. Zero-length native input underflows to a 262144-byte DMA descriptor and remains rejected.
- Deadline ownership includes the physical source, length, DMA channel, all seven DMA words, all eight serial configuration words, and the bank enable word. Reprogramming starts a fresh deadline. An explicit re-arm of the same channel also cancels the old deadline. Tick checks the tuple again before reading.
- A successful source RAM read, capture and checksum happen before the unchanged `pdma_progress` and original DMA IRQ. The model does not write callback state or guest success flags. Stop, invalid mapping, unknown shape, RAM read failure, or bounded budget exhaustion cannot produce completion.
- Limits remain 16 MiB and 1024 jobs. With all new properties off, there is no timer or new read handling. The merged observer does not admit the old rdw general 32-bit TX mode.

SFX tone 2 requires the explicitly synthetic resource fixture maintained by portrait. It copies the bundled 4800-byte tone 1 into the missing tone 2 resource input. It is not recovered factory tone 2 and cannot prove its physical sound, quality or duration.

## Verification

- Exact final merged header: 55 actual-C cases under ASan/UBSan, including REC bank rotation, source wrap, stop, invalid descriptors, exact format, SFX length and deadline changes.
- Independent portrait review: 63 ASan/UBSan cases, including default-off, late callback, future-bank re-arm, post-arm read failure and budget failure. `portrait/shutter/production/review/tx-model/v2/independent-review.json`.
- Exact-F native ARM: original serial TX/RX start/stop, four cases. Existing native ring test executes 60 original constructor/refill calls and verifies zero-length underflow and stopped-callback behavior. Leaves in that test are explicitly recorded shims.
- Seven shared functions remain byte-for-byte unchanged in the port: RAM mapping, DMA progress/IRQ, serial config/enable and RX16 producer. Ten structural checks pass. QOM help exposes all three properties on the actual `nikon-z8-bringup` machine.

The first SFX model was rejected during independent review because it admitted arbitrary enabled serial formats and could inherit a previous descriptor's deadline after a source rewrite. It was never run as an acceptance candidate. Both counterexamples are retained and fixed in v2.

## Current execution boundary

The REC-only rdw TX16 lane used the earlier private `c1e0b9b6…` model and exact F MAIN. Four actual device RAM reads captured 768000 bytes, SHA-256 `486122a8fff0e63257ed001b175e8dd8ed83bce28cfd31a1c62ea7cd71c52682`. The first 608608 bytes exactly match source PCM (`f651b8b3139702ca579a8bc35e23ea50d4ba23451bb67fe257b64ea5c426cbad`); the remaining 159392 bytes are all zero. Original DMA callbacks refill the successive native ring blocks; the final receipt has four jobs, no fault and no pending descriptor. This is functional device consumption and native lifecycle evidence, not speaker/DAC output.

That full runner remains **235/236**, failing the existing unknown-MMIO assertion at `0x0d154340`. This failure has not been ignored, tolerated or fixed here. The merged-v2 full capture/REC/recovery lane is owned by audio and must be evaluated separately when its receipts arrive.

The first merged-v2 lane subsequently consumed both 4800-byte SFX buffers exactly and observed native normal completion/cleanup/microphone resumption, with no cancellation request. Its new MOV fully decodes and has no all-zero audio tail. That runner nevertheless failed an external RX-bank input match after saving, before REC/final Ready. See `../final-f-independent-audit/final-lane1-sfx-audit.json` and `final-lane1-media-audit.json`; full-machine acceptance remains pending.
