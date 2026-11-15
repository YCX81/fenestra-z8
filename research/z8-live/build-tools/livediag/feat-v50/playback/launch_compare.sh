set -eu
B=$HOME/.local/share/nikon-z8-qemu
D=$HOME/fxfix/v50-playback
mkdir -p "$D/logs"
export LIVE_QEMU_MIN_GAP_S=0 LIVE_QEMU_MAX=8 PORTRAIT_CARD=$HOME/fxfix/portrait/card-on
cd "$B/live-audio/regress"
nohup "$B/venv/bin/python" -u -B rerun-v41/retry-driver/run_live.py --name v50cmp-v46-pb --main "$HOME/fxfix/v46/q/main.main" --mode pb --seconds 900 --qemu "$B/merged/work/build/qemu-system-aarch64" > "$D/logs/v46-pb.log" 2>&1 < /dev/null &
echo baseline_pid=$!
nohup "$B/venv/bin/python" -u -B rerun-v41/retry-driver/run_live.py --name v50cmp-v46r-pb --main "$HOME/fxfix/liveplay46r/q/main.main" --mode pb --seconds 900 --qemu "$B/merged/work/build/qemu-system-aarch64" > "$D/logs/v46r-pb.log" 2>&1 < /dev/null &
echo candidate_pid=$!
wait
