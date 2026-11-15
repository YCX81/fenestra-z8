# Private TX counter-clock profile v3

This is an opt-in QEMU functional peripheral clock correction. It does not change firmware, ARM generic-timer configuration, PCM content, fixture length, Ready requirements or guest timeouts. Existing public and private v2 binaries are preserved.

The pinned ARM model computes `gt_cntfrq_period_ns = floor(1e9/24e6) = 41` and CNTPCT from QOM virtual nanoseconds divided by 41. The native clock then divides the count by 24. Therefore one native microsecond corresponds to 984 QOM nanoseconds (a stable counter offset changes the epoch only). The external ADC feeder already uses 49.2 ms QOM for each 50 ms native block. The new `serial5-tx-counter-clock=on` option applies the same 984/1000 conversion to TX's admitted functional delays: SFX 25 ms, first prefill 1 ms, and subsequent REC 1 s per 192000 bytes.

Default false retains the previous QOM-clock delays. The setter rejects domain changes after active TX/RX, completed traffic or nonzero phase. Source/count/control identity binding, strict format 0x450a2000, mapped RAM reads before DMA/IRQ, budgets and stop cancellation are unchanged from reviewed v2. This is an atomic DMA consumption model with a declared prefill interval, not a physical FIFO, measured DAC rate or acoustic proof.

Use the existing profile flags plus:

```
serial5-tx-drain-model=on
serial5-tx16-ring-model=on
serial5-tx16-sfx-model=on
serial5-tx-counter-clock=on
```

Do not enable a host audio backend for the narrow 16-bit profile. It is rejected. Use each existing machine recipe with its own binary; merged and rdw are not interchangeable profiles.

| Profile | Remote binary | SHA-256 |
| --- | --- | --- |
| merged10, `nikon-z8-bringup` | `/path/to/user/fxfix/v53-tx16-counter-merged-v3/qemu-system-aarch64` | `0f341f3285e968f3121d40bf08828fb7faeb40ae6aa65b2a720ef5290b795914` |
| rdw11, `nikon-z8-full-lab` | `/path/to/user/fxfix/v53-tx16-counter-rdw-v3/qemu-system-aarch64` | `dd4da89142651ca3658b0e70b9d2d0b62ae7eb38e7fe1b3ab1778525260a0762` |

Both builds use private board objects and link response files. Public source/binaries were not overwritten. `hashes.json` and `rdw-v3/hashes.json` pin sources and patches. `build_v3.py` and `build_rdw_v3.py` document exact inputs and link operations. `help-smoke.json` confirms property registration on both exact binaries.

Own actual-function ASan/UBSan harness: 61 cases per profile. Four combined cases use actual F/fixed PB frame functions and actual model request/schedule/tick with explicit RAM, timer and IRQ leaves. A 3.003-second clip, without changing its duration, produces 3/3/3/4 completed banks for old-clock/F, old-clock/fixed-PB, counter-clock/F, counter-clock/fixed-PB. Thus the model change does not hide F's premature fixed-three-second image EOS. Independent merged review adds setter, fresh rearm, future-bank, stop and armed-read-failure cases: 70 pass in `portrait/shutter/production/review/tx-model/v3/independent-review.json`.

`test_rdw_port.py` verifies seven shared functions and the counter setter against merged after normalizing type names, then compiles and executes the exact rdw implementations. rdw retains its earlier master-only legacy S32 route and optional host backend; merged has no such route and requires one of the private 16-bit flags. The narrow 16-bit request/tick/timing semantics are the same.

These tests authorize whole-machine validation of the next firmware candidate. They do not establish an end-to-end firmware pass. F remains rejected by its captured 3.069729-second clip and premature REC stop; full-G evidence must separately show exact image identity, complete PCM source consumption, normal REC exit and restored Ready.
