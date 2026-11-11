#!/usr/bin/env bash
# re.sh CMD ARGS... ; several commands separated by a literal "--"
here=$(cd "$(dirname "$0")" && pwd)
py=$(cd "$here/../../../../../../.." && pwd)/.venv/bin/python
cmd=()
for a in "$@" --; do
  if [ "$a" = "--" ]; then
    [ ${#cmd[@]} -gt 0 ] && { echo "## ${cmd[*]}"; "$py" "$here/mainre.py" "${cmd[@]}"; }
    cmd=()
  else cmd+=("$a"); fi
done
