from pathlib import Path
import hashlib, json, subprocess

P=Path(__file__).resolve().parent
F=P.parents[1]
W=F/'portrait/preview-duration/work'
M=json.loads((F/'portrait/preview-duration/changed-files.json').read_text())
for x in M['files']:
    assert hashlib.sha256((W/x['path']).read_bytes()).hexdigest()==x['new_sha256']
s=(W/'capture/live_stream.c').read_text()
a=s.index('int live_stream_preview_frame('); i=s.index('{',a)+1; depth=1
while depth:
    depth+=(s[i]=='{')-(s[i]=='}');i+=1
sample=s[a:i]
test=r'''
#include "live_stream.h"
#include "preview_writer.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
SAMPLE
int main(void){
 struct live_stream*s=calloc(1,sizeof *s);uint8_t*out=malloc(LIVE_PREVIEW_FRAME_BYTES);
 s->preview=malloc((size_t)LIVE_PREVIEW_FRAME*LIVE_PREVIEW_SLOTS);assert(s&&out&&s->preview);
 s->preview_count=LIVE_PREVIEW_SLOTS;s->preview_head=17;
 uint32_t spans[]={1499941,1499985,3069729,3069733,3533866,3999960};unsigned cases=0;
 for(unsigned large=0;large<2;large++)for(unsigned c=0;c<6;c++){
  uint64_t d=spans[c];s->clip_start=large?UINT64_MAX-4000001:100000000;
  s->clip_end=s->clip_start+d;
  for(unsigned j=0;j<LIVE_PREVIEW_SLOTS;j++){unsigned k=(17+j)%LIVE_PREVIEW_SLOTS;
   s->preview_pts[k]=s->clip_start+d*j/LIVE_PREVIEW_SLOTS;
   memset(s->preview+(size_t)k*LIVE_PREVIEW_FRAME,16+j,LIVE_PREVIEW_FRAME);}
  for(unsigned q=0;q<LIVE_PREVIEW_FRAMES;q++){
   assert(!live_stream_preview_frame(s,q,out,LIVE_PREVIEW_FRAME));
   uint64_t target=s->clip_start+d*q/LIVE_PREVIEW_FRAMES;assert(target>=s->clip_start&&target<s->clip_end);
   unsigned pick=0;for(unsigned j=0;j<LIVE_PREVIEW_SLOTS;j++)if(s->preview_pts[(17+j)%LIVE_PREVIEW_SLOTS]<=target)pick=j;
   assert(out[0]==16+pick);
  }cases++;
 }
 /* Exhaust every integer duration admitted by the unchanged reader cap.
  * Includes writer lower rounding boundary, not just rounded test spans. */
 uint32_t admitted=0;
 for(uint32_t d=0;d<=4000000;d++){
  uint32_t dt=(uint32_t)(((uint64_t)d+LIVE_PREVIEW_FRAMES-1)/LIVE_PREVIEW_FRAMES);
  if(dt>=33333&&dt<=4000000/LIVE_PREVIEW_FRAMES){
   uint64_t shown=(uint64_t)dt*LIVE_PREVIEW_FRAMES;
   assert(shown>=d&&shown-d<45&&shown<=4000000);
   assert(d>=1499941&&d<=3999960);admitted++;
  }
 }
 free(s->preview);free(out);free(s);
 printf("{\"passed\":true,\"sample_cases\":%u,\"admitted_integer_spans\":%u}\n",cases,admitted);
}
'''.replace('SAMPLE',sample)
(P/'writer-independent.c').write_text(test)
cmd=['clang','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
 '-I'+str(W/'capture'),'-I'+str(W/'codec'),str(P/'writer-independent.c'),'-o',str(P/'writer-independent')]
subprocess.run(cmd,check=True,capture_output=True)
r=subprocess.run([str(P/'writer-independent')],capture_output=True,text=True)
assert r.returncode==0,r.stderr
out=json.loads(r.stdout)
out.update(patch_sha256=M['patch_sha256'],files=M['files'],
 findings=[],
 source_review=[
 'Successful live_mov_finish_prefix validates strictly ascending added PTS, count and total<=UINT32_MAX before the cast; transaction uses the same count and last edge.',
 'freeze publishes first frozen PTS==clip_start and last_duration==clip_end-lastPTS, so writer duration equals sampling span without independent clocks.',
 'LVJ1 version1, reserved0, trailer, audio offsets and 45 images remain unchanged; output rounding is <45us and never earlier than clip EOS.',
 'No audio dependency for new duration: silent, unavailable and partial audio fallback use same video span. Original cancel/write/read error rollback paths remain in force.',
 'Production tuning initializes window1500000, CFR30000/1001, GOP15 and short-start warm500000. Nominal valid IRAP cadence fits existing4s reader. Generic helper now rejects shorter-than1499941us or longer-than3999960us spans.'
 ],
 reviewed_evidence=['portrait/preview-duration/tests/result.json','portrait/preview-duration/tests/transaction/result.json'],
 scope='Independent exact-source/hash review plus actual sampling ASan/UBSan; existing full writer and transaction test evidence inspected, not claimed as new reruns. Supports integration/full-machine validation, not camera acceptance.')
(P/'writer-independent-review.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps(out,indent=2))
