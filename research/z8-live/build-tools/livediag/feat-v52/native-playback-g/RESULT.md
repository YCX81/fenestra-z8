# v52g exact-image native playback and link audit

PASS for the Live Off native reference playback lane: exit 0, 95 DEC_PICTURE operations, 11 LPCM_JOB operations, all 237 checks passed, no error, and owned QEMU stopped naturally. The loaded `-kernel` file, frozen private copy, result SHA and identity record all agree on MAIN `ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613` (build ID `8fb1ba4c`). The driver SHA function is unmodified.

The independent linked-image audit passed. It re-created the complete composition from v51d, checked every allocated Live/PB ELF section against final MAIN, checked both payloads and the extra section for overlap, re-applied the complete aggregate manifest, and checked published symbols and undefined symbols. Live is 110528 bytes (64 free before the dimension patch), extra is 2064 bytes (1496 free), and PB is 36848 bytes (4112 free). The dimension/meta caves remain identical to the v51d base. All explicit EVF and 419 portrait records were preserved.

The only F-to-G source change is in `live_w5enc.c` (final source SHA `df8ebae5aacd18de39c07b286447255ab2a316647f3b1c454f914ea80ef78f8e`): remove the invalid persistent-result predicate on `0xa02d32788` and correct its explanation. The worker state and instance/owner checks, unchanged bounded wait, and persistent HEADER reply storage remain. F's failed predicate and earlier failed runs are retained separately.

This result verifies the native Live Off playback path using the functional host libavcodec backend and real MCU firmware with explicit model inputs. It does not prove Live On resource handoff on physical hardware, and does not substitute for the distinct close/free/reopen lane. Root owns the G production REC/recovery and Gclose results.

Evidence is in `evidence/`: compact summary, linked audit, full original result and stderr, actual loader identity, launch scripts, exit status and two screenshots. The remote archive is `/path/to/user/fxfix/v52g-native/evidence.tar.gz`.
