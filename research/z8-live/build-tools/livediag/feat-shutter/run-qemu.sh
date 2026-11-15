B=$HOME/.local/share/nikon-z8-qemu; PY=$B/venv/bin/python; M=$HOME/fxfix/shutter/q/main.main
export SOUND_POINTS=200:0x40f9ce60,201:0x40f9cb54,202:0x40f9cd54,205:0x40e312d8,210:0x40f9cb18,212:0x401e5194,213:0x40f9cb68
cd $B/live-audio/regress
gap(){ while :; do y=$(ps -o etimes= -C qemu-system-aarch64 | sort -n | head -1); [ -z "$y" ] || [ "$y" -ge 100 ] && break; sleep 10; done; }
run(){ for t in 1 2 3 4 5 6; do gap; rm -rf runs/$1; PORTRAIT_CARD=$HOME/fxfix/portrait/card-on $PY -u -B rerun-w10/retry-driver/run_sound.py --name $1 --main $M --mode on --seconds 900 --qemu $B/merged/work/build/qemu-system-aarch64 > runs/$1.log 2>&1 & p=$!; sleep 60; if kill -0 $p 2>/dev/null; then echo started $1; wait $p; return; fi; echo retry $1; done; }
SOUND_MUTE=1 run v39d-mute-1 & sleep 120
run v39d-sound-1 & sleep 120
SOUND_MUTE=1 run v39d-mute-2 &
wait; echo ALLDONE
