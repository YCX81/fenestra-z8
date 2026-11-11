#!/usr/bin/env bash
# compare.sh TAG : three v37 test-image runs and three v36 control runs of the same
# scenario, interleaved 100 s apart (v37, v36, v37, ...). Lab paths of this workstation.
here=$(cd "$(dirname "$0")" && pwd)
tag=$1
for i in 1 2 3; do
  [ $i = 1 ] || sleep 100
  setsid nohup bash "$here/run.sh" "$tag-n$i" "$HOME/v37-swenc/qemu-input/swenc-test.main" "$HOME/v37-swenc/pb/changed-ranges.json" >/dev/null 2>&1 &
  sleep 100
  setsid nohup bash "$here/run.sh" "$tag-c$i" "$HOME/v36-swenc/qemu-input/swenc-test.main" "$HOME/v36-swenc/pb/changed-ranges.json" >/dev/null 2>&1 &
done
wait
echo done
