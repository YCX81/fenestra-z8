#!/bin/bash
# cap.sh NAME : capture regression (merged QEMU) for ~/fxfix/onemov/NAME/q/main.main
N=$1; D=/path/to/user/fxfix/onemov; B=/path/to/user/.local/share/nikon-z8-qemu; cd $D
rm -rf driver-$N; cp -r /path/to/user/fxfix/v53-audio/core-pcm-diag-g/driver driver-$N
SHA=$(sha256sum $N/q/main.main | cut -d' ' -f1)
grep -rl c14894087d5fd03b0124c7d94f6cd521e94def58831a087738145ef91b60fcd2 driver-$N | xargs sed -i "s/c14894087d5fd03b0124c7d94f6cd521e94def58831a087738145ef91b60fcd2/$SHA/g"
rm -rf $B/live-audio/regress/runs/$N-capture*
LIVE_QEMU_MIN_GAP_S=0 "$B/venv/bin/python" -u "$D/driver-$N/retry-driver/run_live.py" --name $N-capture --main $D/$N/q/main.main --mode on --qemu /path/to/user/fxfix/v53-tx16-counter-merged-v3/qemu-system-aarch64 --sfx-resource /path/to/user/fxfix/v53-shutter/production/resource-fixture/resource4e-tone2-synthetic.bin --seconds 900 > $N-capture.log 2>&1
echo "$N capture exit $?"; ls -la $B/live-audio/regress/runs/$N-capture/files/LIVE/100NCZ_8/ 2>/dev/null
