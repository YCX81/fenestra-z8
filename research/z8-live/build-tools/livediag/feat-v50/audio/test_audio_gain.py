#!/usr/bin/env python3
"""Run the actual runtime PCM callback with the actual audio ring implementation.

The small wrapper substitutes only live_runtime's unrelated fields. The callback
body is copied unmodified from the supplied runtime.c, so the v49 defect fails.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio/live_audio.h"
struct live_runtime { struct live_audio audio; };
__CALLBACK__
static int16_t saturate(int16_t x) {
 int32_t v=(int32_t)x*4;
 return v>32767?32767:v<-32768?-32768:(int16_t)v;
}
static int16_t sample(uint32_t frame, unsigned ch) {
 static const int16_t values[]={100,-200,8191,8192,-8192,-8193,32767,-32768,0,37,-59};
 return values[(frame+ch*3u)%11u];
}
int main(void) {
 const uint32_t frames=3u*16384u+29u;
 struct live_runtime r={0};
 int16_t *input=calloc(frames*2u,sizeof *input);
 int16_t *full=calloc(frames*2u+2u,sizeof *full);
 int16_t *parts=calloc(frames*2u+2u,sizeof *parts);
 int16_t *ring=calloc(LIVE_AUDIO_CAPACITY*2u,sizeof *ring);
 assert(input&&full&&parts&&ring);
 for(uint32_t i=0;i<frames;i++)for(unsigned ch=0;ch<2u;ch++)input[2u*i+ch]=sample(i,ch);
 live_audio_init(&r.audio,0);
 r.audio.active=1;r.audio.ring=ring;
 assert(!live_audio_set_duck(&r.audio,(struct live_audio_duck){0,0,0,32768}));
 assert(!live_audio_push(&r.audio,input,frames,LAUDIO_S16LE,1000000u));
 assert(!live_audio_mark(&r.audio,1200000u));
 uint64_t end=1000000u+(uint64_t)frames*1000000u/48000u;
 assert(!live_audio_freeze(&r.audio,(struct live_audio_window){1000000u,end}));
 uint32_t got=777u;
 assert(!live_audio_ticket_get(&r,0,full,16384u,&got)&&got==16384u);
 for(uint32_t i=0;i<16384u*2u;i++) {
  if(full[i]!=saturate(input[i])) {
   fprintf(stderr,"64-KiB block gain mismatch frame=%u channel=%u actual=%d expected=%d\n",i/2u,i%2u,full[i],saturate(input[i]));
   return 2;
  }
 }
 full[2u*frames]=12345;full[2u*frames+1u]=-12345;
 assert(!live_audio_ticket_get(&r,0,full,frames+1u,&got));
 assert(got==frames);
 for(uint32_t i=0;i<frames*2u;i++) {
  if(full[i]!=saturate(input[i])) {
   fprintf(stderr,"gain mismatch frame=%u channel=%u actual=%d expected=%d\n",i/2u,i%2u,full[i],saturate(input[i]));
   return 2;
  }
 }
 assert(full[2u*frames]==12345&&full[2u*frames+1u]==-12345);
 const uint32_t caps[]={1,3,16384,24024,8191};
 for(unsigned k=0;k<sizeof caps/sizeof *caps;k++) {
  uint32_t off=0;
  while(off<frames) {
   assert(!live_audio_ticket_get(&r,off,parts+off*2u,caps[k],&got));
   assert(got&&got<=caps[k]&&got<=frames-off);off+=got;
  }
  assert(!memcmp(full,parts,frames*2u*sizeof *full));
 }
 assert(!live_audio_ticket_get(&r,0,0,0,&got)&&got==0);
 assert(!live_audio_ticket_get(&r,frames,parts,20,&got)&&got==0);
 int16_t keep=parts[0];live_audio_cancel(&r.audio);
 assert(live_audio_ticket_get(&r,0,parts,1,&got)==LAUDIO_CANCELLED&&got==0&&parts[0]==keep);
 free(input);free(full);free(parts);free(ring);
 puts("PASS stereo gain, 8192-frame seam, chunk invariance, saturation, tail, zero and cancelled reads");
 return 0;
}
'''

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    runtime = (args.source / "runtime.c").read_text()
    begin = runtime.index("static int live_audio_ticket_get(")
    end = runtime.index("\nstatic size_t live_audio_ticket_trak", begin)
    callback = runtime[begin:end]
    with tempfile.TemporaryDirectory(prefix="z8-audio-gain-") as tmp:
        test = Path(tmp) / "test.c"
        binary = Path(tmp) / "test"
        test.write_text(HARNESS.replace("__CALLBACK__", callback))
        subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-O1", "-g", "-fsanitize=address,undefined", "-I", str(args.source),
                        str(test), str(args.source / "audio/live_audio.c"), "-o", str(binary)], check=True)
        run = subprocess.run([str(binary)], text=True, capture_output=True)
    print(run.stdout, end="")
    print(run.stderr, end="")
    report = {"passed": run.returncode == 0, "returncode": run.returncode,
              "runtime_sha256": hashlib.sha256(runtime.encode()).hexdigest(),
              "callback_sha256": hashlib.sha256(callback.encode()).hexdigest(),
              "sanitizers": ["address", "undefined"], "stdout": run.stdout, "stderr": run.stderr}
    if args.report:
        args.report.write_text(json.dumps(report, indent=2)+"\n")
    raise SystemExit(run.returncode)

if __name__ == "__main__":
    main()
