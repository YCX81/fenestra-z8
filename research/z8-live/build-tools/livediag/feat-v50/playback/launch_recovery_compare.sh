set -u
B=$HOME/.local/share/nikon-z8-qemu
D=$HOME/fxfix/v50-playback
export LIVE_QEMU_MIN_GAP_S=0 LIVE_QEMU_MAX=8 PORTRAIT_CARD=$HOME/fxfix/portrait/card-on
cd "$B/live-audio/regress"
(
 "$B/venv/bin/python" -u -B "$D/recovery-driver/retry-driver/run_live.py" --name v50-recovery-control-r2 --main "$HOME/fxfix/v50/q/main.main" --mode pb --seconds 900 --qemu "$B/merged/work/build/qemu-system-aarch64" > "$D/logs/v50-recovery-control-r2.log" 2>&1
 echo $? > "$D/logs/v50-recovery-control-r2.exit"
) &
echo control_pid=$!
(
 "$B/venv/bin/python" -u -B "$D/recovery-driver/retry-driver/run_live.py" --name v51-final-pb-recovery-r3 --main "$HOME/fxfix/v51/q/main.main" --mode pb --seconds 900 --qemu "$B/merged/work/build/qemu-system-aarch64" > "$D/logs/v51-final-pb-recovery-r3.log" 2>&1
 echo $? > "$D/logs/v51-final-pb-recovery-r3.exit"
) &
echo candidate_pid=$!
wait
