#!/usr/bin/env bash
# status.sh NAME [wait-seconds]: wait until the run finishes or the time is up, then summarize.
name=$1; wait=${2:-0}
log=$HOME/.local/share/nikon-z8-qemu/v37-20261111/$name.log
end=$((SECONDS + wait))
while [ $SECONDS -lt $end ]; do
  grep -q "^Finished\|Traceback" "$log" 2>/dev/null && break
  sleep 3
done
echo "checkpoints: $(grep -c CHECKPOINT "$log" 2>/dev/null)  qemu: $(pgrep -c qemu-system)"
grep CHECKPOINT "$log" | tail -1 | cut -c1-900
grep -A14 Traceback "$log" | tail -8
grep "^Finished" "$log"
