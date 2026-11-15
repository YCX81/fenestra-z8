#!/usr/bin/env bash
# batch.sh MAIN SYMBOLS NAME1[:opts] NAME2[:opts] ...  -- starts runs 100 s apart, waits for all.
# opts are driver options joined with '+', e.g. sw-p:--pose+left
here=$(cd "$(dirname "$0")" && pwd)
main=$1; symbols=$2; shift 2
first=1
sleep "${DELAY:-0}"   # offset against another batch started at the same time
for spec in "$@"; do
  name=${spec%%:*}; opts=""
  [ "$spec" != "$name" ] && opts=$(echo "${spec#*:}" | tr '+' ' ')
  [ $first = 1 ] || sleep 100
  first=0
  avail=$(awk '/MemAvailable/{print int($2/1048576)}' /proc/meminfo)
  [ "$avail" -lt 3 ] && { echo "skip $name: ${avail} GiB available"; continue; }
  # shellcheck disable=SC2086
  setsid nohup bash "$here/run.sh" "$name" "$main" "$symbols" $opts >/dev/null 2>&1 &
  echo "started $name ($opts)"
done
wait
for spec in "$@"; do bash "$here/status.sh" "${spec%%:*}" 0 | head -1; done
