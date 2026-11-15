> Publication update: the user confirmed V62 physical validation and completed compilation/testing on 2026-11-15. No V62 build/runtime tests were repeated for publication. The following is the historical delivery record.

# V62 delivered test candidate

- Final MAIN `36c564c9e785822ccf784816882d5055ee8e5ca0b796e1c4fb8e459dfc1ae4b5`, build ID `0xfb84a7ed`, identity `04.06.p0`.
- Package `/path/to/Nikon/delivery/Z8-v62-Log-Shutter-36c564c9/Z_8_0406.bin`, SHA256 `ddb949630186af582d51993286ff468383292800ed53571a2147a95c46853d89`.
- ZIP `/path/to/Nikon/delivery/Z8-v62-Log-Shutter-36c564c9.zip`, SHA256 `d73d310ef25bb64cf26fb02a39bed8defc8f19a51d8349474b80af6765df799a`.
- Copied to the authorized SD card (identifier omitted), readback hash verified, safely ejected disk4. Card configuration hashes/media metadata unchanged. No physical camera/USB/flash operation. See `final/sd-copy.json`.

## Changes and validation

The final source and reproducibility are detailed in RESULT.md and final/BINARY-AUDIT.md. Only two source files change from V61, plus the guarded wrapper-only menu patch. Entire PB bank/ELF and video freeze code remain unchanged. User separately confirmed V61 RAW-priority badge/playback/return on hardware.

- Menu final ARM114164 assertions, shutter122 and original SoundTask23 cases pass.
- Private native SFX model104 ASan/UBSan cases pass.
- Full final-image On-paired, On-tone10 and Off capture tests pass with actual factory PCM/zero-output control, mic recovery, persistent settings, valid new MOV/audio and continued encoding.
- Both new-card reboot native playbacks enter/return/clean up. **Off-r4 covers all93 source frames** via independent decoded NV12 CRC;94 events include a repeated first frame.
- **On-r2 complete frame coverage does not pass**:92 events are repeated first frame plus source frames0..90; final source frame91 is not output. The earlier claim of complete92-frame playback was corrected in all final reports. No new QEMU or video change was added for this separate tail-frame limitation.
- Earlier MCU ACK assertion before capture remains unexplained; successful retry kept the same checks. Full retry record retained.

`final/verification.json` explicitly has `release_checks_passed=true`, `all_observations_passed=false`, with exact acceptance scope and On-tail limitation. Package contains19 hashed evidence files; ZIP/contents/hash validation passed. Physical menu pixels/acoustics/timing still require user acceptance. Enabled sound intentionally pauses mic; tone10 model silence was0.25675s. Native Silent remains authoritative; loop/Bulb protocols unchanged.

All subagents finished; all task-owned QEMU guests are closed. No pending build/test/card operations. Earlier attempts and STATUS-history-before-final-checks.md are historical, not delivery candidates. Do not overwrite the frozen package or silently treat On-r2 frame coverage as passing.
