set -u
B=$HOME/.local/share/nikon-z8-qemu
D=$HOME/fxfix/v50-playback
export LIVE_QEMU_MIN_GAP_S=0 LIVE_QEMU_MAX=8 PORTRAIT_CARD=$HOME/fxfix/portrait/card-on
cd "$B/live-audio/regress"
for version in v50 v51; do
 (
 "$B/venv/bin/python" -u -B "$D/recovery-noobserve-driver/retry-driver/run_live.py" --name "$version-pb-noobserve" --main "$HOME/fxfix/$version/q/main.main" --mode pb --seconds 900 --qemu "$B/merged/work/build/qemu-system-aarch64" > "$D/logs/$version-pb-noobserve.log" 2>&1
 echo $? > "$D/logs/$version-pb-noobserve.exit"
 ) &
 echo "$version pid=$!"
done
wait
