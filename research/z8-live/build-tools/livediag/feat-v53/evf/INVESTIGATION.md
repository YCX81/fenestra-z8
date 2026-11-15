# v53 EVF shooting badge investigation

Baseline: exact v52 G MAIN `ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613`.
The user reports that the shooting badge is absent in EVF, while REC video is visible there. Original video playback with Live On now works on hardware; that handoff implementation is preserved.

## Established original instruction paths

* `40398570` returns output 1 when byte `a03229eb0` is zero, otherwise 4. The real switch `40398d18` masks out bit 1 of its argument and selects outputs 1/4; output 2 is not an alternative EVF identity.
* `4031e3c0` installs the dimensions callback `4032581c` (caller `403257c4`). That callback reads the bound graphics tuple then calls `4034079c`, which swaps output/group and calls `4033d8cc`; dimensions come directly from descriptor `+8/+a`. Thus the badge's equality check against these descriptor dimensions has no demonstrated static mismatch.
* Shared root index 4 is `a03229ee0 + 4 * 0x118`. The v52 adapters handle output/group argument order and only reconstruct/update the owned, empty root. No foreign-root guard has been relaxed.
* Badge-specific paths still under investigation: shooting view ID/state, stale-root removal, descriptor backing/format, direct-child obstruction rejection, badge placement and actual native draw dispatch. The v52 fixtures mocked native endpoints; they did not prove physical EVF switching.

## Eye input traced from original firmware

1. Eye initialization `40c94458` requests logical GPIO `0x2c` through `40c00370` and `40c001f4`.
2. The initialized GPIO table starts at `a02808bf8`, entries 40 bytes. Entry 44 is `a028092d8`: ID `0x2c`, bank 3, pin 13. Its interrupt descriptor is `a0280c370`: IRQ bank 1, bit 4, INTID 68, ISR callback `40c162e0`.
3. Original `40c15c80` reads the data register for the GPIO bank and extracts bit 13. Bank 3 is `0x0d15100c`. `40c94538` in auto mode returns the inverse of this level; forced modes 1/2 return 1/0 without reading the pin.
4. Eye service `40c9407c..40c94100` supplies the constant at `416538f0` (`2,0`) plus callback `40c948b8` to `40c15e80`, reads the current level and arms the interrupt.
5. `40c15e80` mode 2 sets polarity opposite the current physical level and sets IRQ type bit (second argument 0). The actual ISR `40c16358` acknowledges pending through W1C, reads the pin, reverses polarity, then calls the registered callback. It does not directly modify GUI output.

## Isolated experiment

`model/build_eye.py` copies only the machine C file into `/path/to/user/fxfix/v53-evf/model`, compiles a private object, and links a private QEMU executable. Existing source, object and shared binary are untouched. Baseline binary SHA is `a88a921c9759deced131e372b2c9881b1f648690cfa306b51f3814b0d7961e5d`; the first private binary SHA is `67c3b332c142807fc1db497a3b1541d8ac743320c47f55154576cd8d69a081fc`.

The only new input is the external level of bank 3 pin 13. It latches the corresponding IRQ only on a matching changed edge with original polarity/type configuration, honors the original mask, and clears on native W1C. It supplies no guest RAM/GUI/output state and no fabricated completion. Physical electrical timing is unverified.

`diagnostic/observer.py` samples the live owner, native view, owned root, child rectangles, actual graphics descriptor and UI counters every two virtual seconds. Snapshots are read-only. Scheduled pin levels are 1/0/1/0/1 at virtual seconds 2/14/22/30/38. The unmodified G firmware runs from a private card clone containing a valid persisted Live-On configuration. This experiment must demonstrate actual original output switching before it can diagnose the EVF badge; a model input alone is not a pass.

## Final bounded investigation result (2026-11-14)

No firmware change is justified yet. All owned UI and icon files still match immutable v52 G. The user's physical EVF failure remains open. The existing handoff repair is preserved.

### What the isolated original-firmware runs prove

Runs 7 and 8 complete with 443 retained driver checks each, no unexpected stop/unmodeled MMIO, and the owned QEMU stopped normally. Both load exact G MAIN. The private model/input fixture is distinct from the delivered firmware and from public rdw QEMU.

The original eye service was enabled with a narrowly bounded I2C config-write peer at controller 3/address 0x38. No sensor reading or device identification was invented. Original GPIO bank 3 pin 13, IRQ bank 1 bit 4/INTID 68 then supplied two actual original ISR edges and two W1C acknowledgements. Mode transitioned LCD 1 → EVF 4 → LCD 1. This is an external logical peripheral fixture, not verified physical wiring/timing.

The EVF transition required a 16-bit SPI9 configuration write. The private peer permits dummy RX only for the proven original write-and-discard helper `402d926c → 40ca4a1c → 402d9300`: one 16-bit word, RX = TX−4, actual saved return at TX−0x44 equals `402d9300`, and exact transmitted words must match. Original helper never reads the RX scratch after the call. Earlier rejecting prototypes remain archived. No broad 16-bit response or panel ID/status fixture was added.

During stable EVF in run 7, original current view remains `a0282dab8`, ID 3/state 1. Owned root tuple is `(group=1, output=4, target=4)`, dimensions 720×480, one owned child. Native callbacks increase, error=0, obstruction counter=0, descriptor enabled=1, backing `a1c17da00`. The source image contains Live pixels with half-open ink bbox `(1,101,108,139)`.

### Distinct numbering spaces and actual composition

Output 4 is the EVF output bit, not graphics target index 4. Each output has a group-1 array of five descriptors. In output 4, targets 0/1/2 use 1280×960; targets 3/4 use 720×480. Original target 2 contains the VIEW, exposure, AF and other EVF icons; our owned root targets index 4.

The original compositor `4033805c` explicitly iterates all five group-1 targets (`403382a4..403382ac`, `40338378..40338390`, `403389b0..403389b8`). It queries per-target enabled and the target dimensions/rectangles before transformation.

More than callbacks are observed: run-8 stderr job 64 composites original target-2 source `a1b441800`, size 1280×960, into YYUV+A destination `a17efe400` (1,228,800 pixels); job 65 composites Live target-4 `a1c17da00`, size 720×480, into that same destination (1,093,120 pixels). Jobs 99/103/107 through 131 repeat the same EVF composition pattern. `diagnostic/native-composition-events.json` retains exact original stderr lines. Thus the target-4 plane is neither rejected by the owner guards nor statically omitted from this original EVF compositor path.

### Crop hypothesis tested, not assumed

Captured descriptor rectangles are full-frame. `model/test_native_geometry.py` executes the original descriptor getters and `40332820` transform with those captured bytes. Composite sizes are the observed native graphics extents, and both panel-mode branches are explicitly fixtures. On 1280×960 the 720×480 Live layer transforms to approximately `(0,53.33,1280,906.67)`; c5 mode gives `(0,53.75,1280,906.25)`. Its actual nontransparent ink maps to approximately x=1.78..192, y=233..301. It is inside the composite canvas. The tests provide no basis to label a left crop or move the badge as a proven fix.

### What is still missing

The scanout model becomes inactive during the EVF transition and retains the old LCD 1024×682 image. Therefore the screenshot is **not** an EVF visibility result. Run-8 plane dumps occurred just before output transition settled and after switching back, so they are labeled descriptor/backing observations, not final EVF screenshots. All screenshots and tests retain this limit.

The cheapest discriminating next hardware observation is the existing UI state while the user actually sees the missing badge: requested/photo/view ID+state, selected output, owned root tuple, descriptor size/enabled, callback/error/blocked/target-lost counters, and a compositor-submission checkpoint. Capture that small RAM state from the existing worker and persist it after returning to LCD; avoid file I/O in GUI callbacks and avoid reading display MMIO. If callbacks stop or an owner guard rejects, the branch is directly identifiable; if pixels and composite submission are present, investigation moves to actual panel/output configuration. No such camera operation or new firmware diagnostic was performed here.

Evidence: `RESULT.json`, `diagnostic/summary.json`, `diagnostic/native-geometry-arm.json`, `diagnostic/native-composition-events.json`, `diagnostic/native-compositor-and-spi.txt`, plus `diagnostic/evidence.tar.gz` (SHA-256 `8a14b89ff65719ae2a1a0dee8ea657ed472129b1602a84d3b2a5201436b56149`). The archive preserves all eight prototypes, full original failure/results, private model patches and exact identities, final drivers, captured descriptors and planes.
