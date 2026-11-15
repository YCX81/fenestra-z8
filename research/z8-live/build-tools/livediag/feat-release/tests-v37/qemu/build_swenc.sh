#!/usr/bin/env bash
# build_swenc.sh V36_DIR OUT_DIR : QEMU-only software-encoder test image plus harness manifest.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
llvm=${LLVM21:-$HOME/llvm21/bin}
export PATH="$llvm:/usr/local/bin:/usr/bin:/bin"
python=${PYTHON:-$(cd "$here/../../../../../../.." && pwd)/.venv/bin/python}
rm -rf "$2"
"$python" -B "$here/build_swenc.py" "$1" "$2" ${3:+"$3" "$4"}   # optional: src-fix36 pb-fix36 for the control image
"$python" -B "$here/derive_manifest.py" "$1/core-baseline.main" "$2/pool/swenc-test.main" "$2/pb/changed-ranges.json" "$2/qemu-input"
