#!/bin/bash
# usage: run-boot.sh NAME [actions] [main]
P=$(cd -- "$(dirname -- "$0")/.." && pwd) || exit 1
: "${FENESTRA_NIKON_WORKSPACE:?Set FENESTRA_NIKON_WORKSPACE to the separate research workspace}"
STATE=${FENESTRA_QEMU_STATE:-"$HOME/.local/share/nikon-z8-qemu"}
LOCK=${FENESTRA_QEMU_LOCK:-"$FENESTRA_NIKON_WORKSPACE/z8-integration-20261014/.local/integration/z8-lock.sh"}
PY=${FENESTRA_PYTHON:-"$STATE/integration-20261014/venv/bin/python"}
L="$STATE/fenestra-z8"; mkdir -p "$L"
M=${3:-$P/build/z8-core/z8-core.main}
cd "$P/port-z8" && FX_SCR=0xa1172a000 "$LOCK" ex "$PY" -B fenestra_boot.py --name "$1" --main "$M" --actions "${2:-menu,left}" ${4:+--module "$4"} ${5:+--tail-s "$5"} >"$L/$1.driver.log" 2>&1
echo "exit=$?" | tee -a "$L/$1.driver.log"
