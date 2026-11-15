set -u
B=$HOME/.local/share/nikon-z8-qemu
D=$HOME/fxfix/v50-playback
export LIVE_QEMU_MIN_GAP_S=0 LIVE_QEMU_MAX=8 PORTRAIT_CARD=$HOME/fxfix/portrait/card-on
actual=$(sha256sum "$HOME/fxfix/v51d/q/main.main" | cut -d ' ' -f1)
[ "$actual" = aa6e94e0042fb445c666de73c0f6b9e7e569010b7993a3ac3da0979b174a9433 ] || exit 1
cd "$B/live-audio/regress"
"$B/venv/bin/python" -u -B "$D/recovery-secondary-stop-driver/retry-driver/run_live.py" --name v51d-secondary-stop-model --main "$HOME/fxfix/v51d/q/main.main" --mode pb --seconds 900 --qemu "$B/merged/work/build/qemu-system-aarch64" > "$D/logs/v51d-secondary-stop-model.log" 2>&1
rc=$?
echo "$rc" > "$D/logs/v51d-secondary-stop-model.exit"
echo "recovery_exit=$rc"
