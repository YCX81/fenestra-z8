#!/usr/bin/env bash
set -eu
RUN_NAME=${1:?Usage: launch_validated_recovery.sh UNIQUE_RUN_NAME}
B=$HOME/.local/share/nikon-z8-qemu
D=$HOME/fxfix/v50-playback
DRIVER=$D/validated-v51d-recovery-driver/retry-driver/run_live.py
export LIVE_QEMU_MIN_GAP_S=0 LIVE_QEMU_MAX=8 PORTRAIT_CARD=$HOME/fxfix/portrait/card-on
[ "$(sha256sum "$HOME/fxfix/v51d/q/main.main" | cut -d ' ' -f1)" = aa6e94e0042fb445c666de73c0f6b9e7e569010b7993a3ac3da0979b174a9433 ]
[ "$(sha256sum "$B/merged/work/build/qemu-system-aarch64" | cut -d ' ' -f1)" = 5ccbfc7e051a78dd24176aec98b3ab767709bc078dd3e179923c74b2f50a2ac2 ]
[ "$(sha256sum "$DRIVER" | cut -d ' ' -f1)" = 1ec568271fd93739a4d92005b56790e765f19a9da6f3a1782e8a84ccb45a6add ]
cd "$B/live-audio/regress"
exec "$B/venv/bin/python" -u -B "$DRIVER" --name "$RUN_NAME" --main "$HOME/fxfix/v51d/q/main.main" --mode pb --seconds 900 --qemu "$B/merged/work/build/qemu-system-aarch64"
