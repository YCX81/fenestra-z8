#!/bin/bash
# go.sh NAME SRCPATCH CARD [CONTACTS] : patch+build+compose+run
set -e
N=$1; PATCH=$2; CARD=$3; CONTACTS=${4:-/path/to/user/fxfix/v53-playback/rec-newmov-g/rec-contacts.json}; VS=${VS:-40}
P=~/.local/share/nikon-z8-qemu/venv/bin/python
D=/path/to/user/fxfix/onemov; B=/path/to/user/.local/share/nikon-z8-qemu
cd $D
if [ ! -f $N/q/main.main ]; then
 rm -rf ${N}src $N ~/v37build/onemov-$N; cp -r ~/fxfix/v53-root/integration-g-final/work ${N}src
 $P $PATCH ${N}src
 $P build_onemov.py --v36-dir ~/v37build/v36 --source-dir ${N}src --output-dir ~/v37build/onemov-$N 2>&1 | tail -1
 $P ~/fxfix/v53-root/compose53.py ~/v37build/onemov-$N $D/$N > $N-compose.log 2>&1
fi
sha256sum $N/q/main.main
OBS=$($P -c "import json;print('%x'%json.load(open('/path/to/user/v37build/onemov-$N/pb/changed-ranges.json'))['symbols']['pb_nb_observe'])")
export NB_OUT=$D/$N-trace.jsonl NB_PTS=$OBS:406364d0:403ad6a4:403f6fac:40711328:4085fcc4:402dd4f8:40cfc820:40cf7d0c:403f6e88:403f6f10:4063b95c:403f7244${EXTRA_PTS:+:$EXTRA_PTS}
rm -rf run-$N run-$N.exit $NB_OUT
[ -f $N-card.raw ] || cp --reflink=auto $CARD $N-card.raw
cd /path/to/Nikon/rdwing-z8-qemu
set +e
Z8_TOLERATE=15431000:15432000 "$B/venv/bin/python" -u "$D/run-nb.py" --firmware $D/$N/q/main.main --qemu "${QEMU:-$B/rdw/build/qemu-system-aarch64}" --raw /path/to/Nikon/z8-qemu/.local/input.raw --mcu /path/to/user/fxfix/exmcu/EX.bi --output "$D/run-$N" --mcu-pins G1=1:G13=1:G14=1:G15=1 --experimental-shared221 --experimental-boundary-inputs --experimental-sensor-source-clock --experimental-yuv-channels --experimental-movie-selector16 --card "$D/$N-card.raw" --experimental-playback --experimental-movie-playback --movie-audio-output none --contact-replay $CONTACTS --seconds 1500 --virtual-seconds $VS > "$D/run-$N.log" 2>&1
echo $? > "$D/run-$N.exit"; echo "exit $(cat $D/run-$N.exit)"; tail -2 $D/run-$N.log
