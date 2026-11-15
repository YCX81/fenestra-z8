#!/bin/bash
set -eu
D=/path/to/user/fxfix/v53-native-rec
B=/path/to/user/.local/share/nikon-z8-qemu
LANE=reference
case "$LANE" in cfr) CARD="$D/cfr-card.raw";; reference) CARD="/path/to/user/fxfix/v53-root/native-g/card-real.raw";; *) exit 2;; esac
cd /path/to/Nikon/rdwing-z8-qemu
set +e
Z8_TOLERATE=15431000:15432000 "$B/venv/bin/python" -u "/path/to/user/fxfix/v53-root/native-g/run-real.py" --firmware /path/to/user/fxfix/v53-root/v53g/q/main.main --qemu "$B/rdw/build/qemu-system-aarch64" --raw /path/to/Nikon/z8-qemu/.local/input.raw --mcu /path/to/user/fxfix/exmcu/EX.bi --output "/path/to/user/fxfix/v53-root/native-g/run-real" --mcu-pins G1=1:G13=1:G14=1:G15=1 --experimental-shared221 --experimental-boundary-inputs --experimental-sensor-source-clock --experimental-yuv-channels --experimental-movie-selector16 --card "$CARD" --experimental-playback --experimental-movie-playback --movie-audio-output none --contact-replay /path/to/user/fxfix/w5dec/contacts-one.json --seconds 1500 --virtual-seconds 40 > "/path/to/user/fxfix/v53-root/native-g/run-real.log" 2>&1
RC=$?
printf '%s\n' "$RC" > "/path/to/user/fxfix/v53-root/native-g/run-real.exit"
exit "$RC"
