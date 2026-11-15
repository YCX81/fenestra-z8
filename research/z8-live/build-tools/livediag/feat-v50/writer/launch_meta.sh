set -eu
D=/path/to/user/fxfix/v50-writer
B=/path/to/user/.local/share/nikon-z8-qemu
cd /path/to/Nikon/rdwing-z8-qemu
(
 set +e
 Z8_TOLERATE=15431000:15432000 "$B/venv/bin/python" -u /path/to/user/fxfix/w5dec/run_native_any.py --firmware "$D/v50-meta.main" --qemu "$B/rdw/build/qemu-system-aarch64" --raw /path/to/Nikon/z8-qemu/.local/input.raw --mcu /path/to/user/fxfix/exmcu/EX.bi --output "$D/native-meta" --mcu-pins G1=1:G13=1:G14=1:G15=1 --experimental-shared221 --experimental-boundary-inputs --experimental-sensor-source-clock --experimental-yuv-channels --experimental-movie-selector16 --card "$D/meta-ready.raw" --experimental-playback --experimental-movie-playback --movie-audio-output none --contact-replay /path/to/user/fxfix/w5dec/contacts-one.json --seconds 1500 --virtual-seconds 40
 echo $? > "$D/native-meta.exit"
) > "$D/native-meta.log" 2>&1
cat "$D/native-meta.exit"
