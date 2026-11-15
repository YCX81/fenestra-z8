#!/bin/bash
set -eu
D=/path/to/user/fxfix/v53-audio/rec-c
B=/path/to/user/.local/share/nikon-z8-qemu
LANE=$1
case "$LANE" in cfr) CARD="$D/cfr-card.raw";; reference) CARD="$D/card.raw";; *) exit 2;; esac
cd /path/to/Nikon/rdwing-z8-qemu
set +e
Z8_TOLERATE=15431000:15432000 "$B/venv/bin/python" -u "$D/run.py" --firmware /path/to/user/fxfix/v53-root/v53c/q/main.main --qemu "$B/rdw/build/qemu-system-aarch64" --raw /path/to/Nikon/z8-qemu/.local/input.raw --mcu /path/to/user/fxfix/exmcu/EX.bi --output "$D/run-$LANE" --mcu-pins G1=1:G13=1:G14=1:G15=1 --experimental-shared221 --experimental-boundary-inputs --experimental-sensor-source-clock --experimental-yuv-channels --experimental-movie-selector16 --card "$CARD" --experimental-playback --experimental-movie-playback --movie-audio-output none --contact-replay /path/to/user/fxfix/v53-audio/rec-c/contacts.json --seconds 1500 --virtual-seconds 40 > "$D/run-$LANE.log" 2>&1
RC=$?
printf '%s\n' "$RC" > "$D/run-$LANE.exit"
exit "$RC"
