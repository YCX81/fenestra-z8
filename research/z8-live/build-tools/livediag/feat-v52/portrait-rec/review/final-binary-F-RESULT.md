# F structural audit, withdrawn behavioral candidate

F MAIN `ceefa749d298066d3666c083d4a15a1985939f22ca13ebd8d80244b28963857b` passes the strict structural audit in `final-binary-f.json`, but **is not a delivery candidate**: its full QEMU run rejected initial open because the first readiness implementation incorrectly required shared result `2788 == 0`.

The source audit initially detected 27 differing AppleDouble metadata files. Root restored exact original bytes without modifying compiled inputs; the unchanged strict 402-file manifest check then passed. No assertion or source-file check was skipped.

Verified: exact immutable source hashes, all linked sections and hooks, independently reconstructed final composition, all 154 aggregate ranges replaying from base c770f340, 419 portrait records/3518 bytes, 40-byte EVF integration, preserved size/meta hole, v49 default fonts and Preparing text, recomputed build ID `a737dc65`, and original firmware identities. Primary Live uses 110544 bytes with 48 bytes free; extra Live uses 2064 bytes with 1496 free; playback uses 36848 bytes with 4112 free.

An in-memory official-container replacement and independent decode passed module integrity checks. Predicted package SHA-256 is `82748e065f12cfdc646a53c8a378c42040312367dcdbb4bc24e3f6148c8322f5`, 99051117 bytes. No package/delivery was created by this review. This hash must not be used as approval to distribute F.

The exact F ELF passed 66 owner/early-close checks and 45 readiness/open tests. Those tests demonstrated implementation branches against the supplied fixtures; they did not establish that the shared-result-zero premise held in the complete firmware. G must use the corrected state-based readiness implementation and be audited separately.
