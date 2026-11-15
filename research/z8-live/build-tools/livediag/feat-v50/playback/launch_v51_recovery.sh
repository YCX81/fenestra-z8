set -u
B=$HOME/.local/share/nikon-z8-qemu
D=$HOME/fxfix/v50-playback
export LIVE_QEMU_MIN_GAP_S=0 LIVE_QEMU_MAX=8 PORTRAIT_CARD=$HOME/fxfix/portrait/card-on
cd "$B/live-audio/regress"
echo '1b0ecc754467c893e37085653f286479f7ea9341d3c10653b28ab1aca93e782b  /path/to/user/fxfix/v51/q/main.main' | sha256sum -c - || exit 1
"$B/venv/bin/python" -u -B "$D/recovery-driver/retry-driver/run_live.py" --name v51-final-pb-recovery --main "$HOME/fxfix/v51/q/main.main" --mode pb --seconds 900 --qemu "$B/merged/work/build/qemu-system-aarch64" > "$D/logs/v51-final-pb-recovery.log" 2>&1
rc=$?
echo "$rc" > "$D/logs/v51-final-pb-recovery.exit"
echo "v51_recovery_exit=$rc"
