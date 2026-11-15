# G independent binary audit

Structural audit **PASS** for MAIN SHA-256 `ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613`, 81807709 bytes, build ID `8fb1ba4c`. Full QEMU behavioral results remain separate and are required before packaging.

The 402-file immutable local source and remote build source match `source-g.json` exactly. The merged encoder SHA-256 is `df8ebae5aacd18de39c07b286447255ab2a316647f3b1c454f914ea80ef78f8e`. No ABI sizes changed. The exact final ELF, rather than a standalone replacement binary, passed 66 owner/early-close cases and 46 readiness/open cases. Tests accept state 2 after shared result 2788 has been overwritten with -1; they retain strict ownership, pre-request state 0, nonzero SET_PARAMS rejection and one-second bounds.

The audit independently applies the raw rebuild delta and all overlays, recomputes the build ID, and obtains the final MAIN byte for byte. Separately, all 154 aggregate ranges reconstruct the complete MAIN from base `c770f34014849b2c42f20b9258a32b8959253cfb296e48f83417572781e00864`. Every generated hook and all linked read-only sections are present.

Live primary occupies 110528 bytes and leaves 64 bytes before `401fd000`. Live extra occupies 2064 bytes from `401fd218`, leaving 1496 bytes before `401fe000`. Playback occupies 36848 bytes, leaving 4112 bytes. All bounds and non-overlap checks pass, including the preserved 536-byte size/meta interval. The 40-byte EVF integration and all 419 portrait records / 3518 edited bytes match the independently frozen reference. V49 font behavior and “Preparing” remain present; old blanket Latin-font retargeting remains absent. Other v51 overlays and `04.06.p0` internal/footer identity are preserved by complete reconstruction.

An in-memory official firmware-container replacement and independent decode verify the body CRC, container integrity, exact MAIN and all unselected bytes/modules. Predicted package SHA-256: `e4a61d2af3480f66b37a83ec967607a179f18fbbc454c2df7673285e80890f0f`, 99051117 bytes. The unchanged three plugin/startup files are regular files and retain their audited hashes. No delivery files or package were written by this review.

Build/compose tools retain their previously reviewed hashes. Updated `pack52.py` additionally requires the packet hash to match this independent prediction. It requires explicit QEMU/core and binary verification, exact MAIN SHA, unchanged official source SHA and unoccupied output paths; test-movie names are confined to the intended directory. The obsolete raw-build packaging flag remains removed. Tool hashes are in `final-binary-tools-g.json`.

Evidence: `final-binary-g.json`, `final-binary-owner-g.json`, `final-binary-ready-g.json`. These checks establish image composition and tested instruction behavior. They do not establish hardware audio, EVF layout or physical-camera timing, and do not substitute for the complete G QEMU regression.
