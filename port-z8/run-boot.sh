#!/bin/bash
# usage: run-boot.sh NAME [actions] [main]
P=/mnt/c/Users/ycx/Desktop/Nikon/third-party-20261029/fenestra-z8
LOCK=/mnt/c/Users/ycx/Desktop/Nikon/z8-integration-20261014/.local/integration/z8-lock.sh
PY=/home/ycx/.local/share/nikon-z8-qemu/integration-20261014/venv/bin/python
L=/home/ycx/.local/share/nikon-z8-qemu/fenestra-z8; mkdir -p $L
M=${3:-$P/build/z8-core/z8-core.main}
cd $P/port-z8 && FX_SCR=0xa1172a000 "$LOCK" ex "$PY" -B fenestra_boot.py --name "$1" --main "$M" --actions "${2:-menu,left}" ${4:+--module "$4"} ${5:+--tail-s "$5"} >"$L/$1.driver.log" 2>&1
echo "exit=$?" | tee -a "$L/$1.driver.log"
