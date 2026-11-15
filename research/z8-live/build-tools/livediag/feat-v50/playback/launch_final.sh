set -u
B=$HOME/.local/share/nikon-z8-qemu
D=$HOME/fxfix/v50-playback
F=$HOME/fxfix/v50/q/main.main
P=$B/venv/bin/python
mkdir -p "$D/logs"
echo '4cb5235fc709220f64af5400680653999d2a03b0d5a4821975e0f6a7b2d50977  /path/to/user/fxfix/v50/q/main.main' | sha256sum -c - || exit 1
export LIVE_QEMU_MIN_GAP_S=0 LIVE_QEMU_MAX=8 PORTRAIT_CARD=$HOME/fxfix/portrait/card-on
cd "$B/live-audio/regress"
(
 "$P" -u -B rerun-v41/retry-driver/run_live.py --name v50-final-pb --main "$F" --mode pb --seconds 900 --qemu "$B/merged/work/build/qemu-system-aarch64" > "$D/logs/v50-final-pb.log" 2>&1
 echo $? > "$D/logs/v50-final-pb.exit"
) &
echo preview_pid=$!
(
 cd "$HOME/Nikon/rdwing-z8-qemu" || exit 1
 test ! -e "$D/v50-final-stock4k.card.raw" || exit 1
 cp --sparse=always "$HOME/fxfix/w5dec/card-live13.raw" "$D/v50-final-stock4k.card.raw" || exit 1
 Z8_TOLERATE=15431000:15432000 "$P" -u "$HOME/fxfix/w5dec/run_native_any.py" --firmware "$F" --qemu "$B/rdw/build/qemu-system-aarch64" --raw "$HOME/Nikon/z8-qemu/.local/input.raw" --mcu "$HOME/fxfix/exmcu/EX.bi" --output "$D/v50-final-stock4k" --mcu-pins G1=1:G13=1:G14=1:G15=1 --experimental-shared221 --experimental-boundary-inputs --experimental-sensor-source-clock --experimental-yuv-channels --experimental-movie-selector16 --card "$D/v50-final-stock4k.card.raw" --experimental-playback --experimental-movie-playback --movie-audio-output none --contact-replay "$HOME/fxfix/w5dec/contacts-one.json" --seconds 1500 --virtual-seconds 40 > "$D/logs/v50-final-stock4k.log" 2>&1
 echo $? > "$D/logs/v50-final-stock4k.exit"
) &
echo stock_pid=$!
wait
