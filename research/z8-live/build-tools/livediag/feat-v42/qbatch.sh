#!/bin/bash
R=$HOME/.local/share/nikon-z8-qemu/live-audio/regress; B=$HOME/.local/share/nikon-z8-qemu; PY=$B/venv/bin/python; Q=$B/merged/work/build/qemu-system-aarch64
M=$HOME/fxfix/v42/q/main.main; L=$HOME/fxfix/v42/logs; mkdir -p $L; rm -f $L/status
cd $R
for n in v42-live v42-pb v42-nofen v42-ja; do rm -rf runs/$n; done
PORTRAIT_CARD=$HOME/fxfix/portrait/card-on $PY -u -B rerun-w10/retry-driver/run_sound.py --name v42-live --main $M --mode on --seconds 900 --qemu $Q > $L/v42-live.log 2>&1 &
sleep 100
$PY -u -B rerun-v41/retry-driver/run_live.py --name v42-pb --main $M --mode pb --seconds 900 --qemu $Q > $L/v42-pb.log 2>&1 &
sleep 100
NO_FEN=1 $PY -u -B rerun-v41/retry-driver/run_live.py --name v42-nofen --main $M --mode pb --seconds 900 --qemu $Q > $L/v42-nofen.log 2>&1 &
sleep 100
PORTRAIT_CARD=$HOME/fxfix/portrait/card-on $PY -u -B rerun-w10/retry-driver/run_lang.py --name v42-ja --main $M --mode on --seconds 900 --qemu $Q > $L/v42-ja.log 2>&1 &
( for i in $(seq 1 90); do sleep 10; ls runs/v42-ja 2>/dev/null | grep -q live-seek-09 && break; done; touch runs/v42-ja/stop.request ) &
wait; echo ALLDONE >> $L/status
