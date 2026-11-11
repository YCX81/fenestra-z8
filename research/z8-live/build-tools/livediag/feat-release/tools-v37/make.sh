#!/usr/bin/env bash
# Local convenience wrapper: make.sh V36_DIR OUT_DIR [STOCK.bin]
# PYTHON defaults to the repo venv (pyelftools, capstone, keystone-engine).
# LLVM21 defaults to ~/llvm21/bin (override with LLVM21=/path/to/bin).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
llvm=${LLVM21:-$HOME/llvm21/bin}
export PATH="$llvm:/usr/local/bin:/usr/bin:/bin"
extra=()
[ $# -ge 3 ] && extra=(--stock "$3")
python=${PYTHON:-$(cd "$here/../../../../../.." && pwd)/.venv/bin/python}
exec "$python" -B "$here/build.py" --v36-dir "$1" --output-dir "$2" --live-clang "$llvm/clang" --llvm-bin "$llvm" "${extra[@]}"
