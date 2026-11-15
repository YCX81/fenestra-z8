set -u
B=$HOME/.local/share/nikon-z8-qemu
D=$HOME/fxfix/v50-playback
export LIVE_QEMU_MIN_GAP_S=0 LIVE_QEMU_MAX=8 PORTRAIT_CARD=$HOME/fxfix/portrait/card-on
cd "$B/live-audio/regress"
"$B/venv/bin/python" -u -B "$D/recovery-error-driver/retry-driver/run_live.py" --name v50-pb-error-observer --main "$HOME/fxfix/v50/q/main.main" --mode pb --seconds 900 --qemu "$B/merged/work/build/qemu-system-aarch64" > "$D/logs/v50-pb-error-observer.log" 2>&1
rc=$?
echo "$rc" > "$D/logs/v50-pb-error-observer.exit"
echo "recovery_exit=$rc"
