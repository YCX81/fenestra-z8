#!/bin/bash
# usage: run.sh SRC_DIR [-v]   (host build of capture/live_stream.c + live_capture.c + au_ring.c)
set -e
S=$1; D=$(mktemp -d); cp -a $S/. $D/
python3 - $D/capture/live_stream.c <<'P'
import re,sys
p=sys.argv[1];t=open(p).read()
t=re.sub(r'__asm__ volatile\("ldp.*?"memory"\);','memcpy(d,s,64);',t,flags=re.S)
open(p,'w').write(t)
P
cp $(dirname $0)/replay.c $D/
cd $D && gcc -O1 -w -I. -Icapture -Icodec -DLA_WIDTH=1024 -DLA_HEIGHT=680 -DLA_RING_CAPACITY=96 -DLA_SAMPLE_INTERVAL_US=25000 -DLIVE_BITRATE=8000000 -DLIVE_IDR_US=500000 \
  replay.c capture/live_stream.c capture/live_capture.c codec/au_ring.c -o replay 2>&1 | head -30 && ./replay ${2:-}
