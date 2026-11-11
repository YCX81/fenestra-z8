#!/usr/bin/env bash
# run.sh NAME MAIN SYMBOLS [driver options...]   (lab paths of this workstation)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
name=$1; main=$2; symbols=$3; shift 3
py=$HOME/.local/share/nikon-z8-qemu/integration-20261014/venv/bin/python
offsets=/tmp/v37-offsets.json
[ -f "$offsets" ] || python3 "$here/offsets.py" "$here/../../src-fix37" "$here/../../pb-fix37" "$offsets"
mkdir -p "$HOME/.local/share/nikon-z8-qemu/v37-20261111"
exec "$py" -u -B "$here/run_v37.py" --name "$name" --main "$main" --symbols "$symbols" --offsets "$offsets" "$@" \
  > "$HOME/.local/share/nikon-z8-qemu/v37-20261111/$name.log" 2>&1
