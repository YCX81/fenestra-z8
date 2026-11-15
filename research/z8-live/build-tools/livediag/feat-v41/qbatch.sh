#!/bin/bash
R=$HOME/.local/share/nikon-z8-qemu/live-audio/regress; B=$HOME/.local/share/nikon-z8-qemu; PY=$B/venv/bin/python; Q=$B/merged/work/build/qemu-system-aarch64
M=$HOME/fxfix/v41/q/main.main; L=$HOME/fxfix/v41/logs; mkdir -p $L; rm -f $L/status
cd $R
for n in v41-live v41-pb v41-nofen v41-ja; do rm -rf runs/$n; done
PORTRAIT_CARD=$HOME/fxfix/portrait/card-on $PY -u -B rerun-w10/retry-driver/run_sound.py --name v41-live --main $M --mode on --seconds 900 --qemu $Q > $L/v41-live.log 2>&1 &
sleep 100
$PY -u -B rerun-v41/retry-driver/run_live.py --name v41-pb --main $M --mode pb --seconds 900 --qemu $Q > $L/v41-pb.log 2>&1 &
sleep 100
NO_FEN=1 $PY -u -B rerun-v41/retry-driver/run_live.py --name v41-nofen --main $M --mode pb --seconds 900 --qemu $Q > $L/v41-nofen.log 2>&1 &
sleep 100
PORTRAIT_CARD=$HOME/fxfix/portrait/card-on $PY -u -B rerun-w10/retry-driver/run_lang.py --name v41-ja --main $M --mode on --seconds 900 --qemu $Q > $L/v41-ja.log 2>&1 &
( for i in $(seq 1 90); do sleep 10; ls runs/v41-ja 2>/dev/null | grep -q live-seek-09 && break; done; touch runs/v41-ja/stop.request ) &
wait; echo ALLDONE >> $L/status
