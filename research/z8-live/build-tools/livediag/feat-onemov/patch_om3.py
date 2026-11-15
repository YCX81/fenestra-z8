"""v55 = om1 + (a) sound flag follows the clip's real audio track, (b) no 150 ms mute at the
shutter, (c) one flight-log record with the shutter-sound hand-off counters per shot."""
import sys,subprocess
from pathlib import Path
W=Path(sys.argv[1])
subprocess.run([sys.executable,str(Path(__file__).with_name('patch_om1.py')),str(W)],check=True)
def sub(path,old,new,count=1):
    p=W/path;t=p.read_text();assert t.count(old)==count,(path,old[:60],t.count(old));p.write_text(t.replace(old,new))
R='pb-fix37/runtime.c'
# (a) a clip whose recording failed still gets a (silent) sound track, like every stock movie.
C='src-fix37/runtime.c'
sub(C,"static size_t live_audio_ticket_trak(void *ctx,uint8_t *out,size_t cap,uint64_t at) {",
"""static int live_audio_silence_get(void *ctx,uint32_t off,int16_t *out,uint32_t cap,uint32_t *got) {
 struct live_runtime *r=ctx;uint32_t n=off<r->audio_info.frames?r->audio_info.frames-off:0;if(n>cap)n=cap;
 for(uint32_t i=0;i<n*2u;i++)out[i]=0;
 *got=n;return 0;
}
static size_t live_audio_ticket_trak(void *ctx,uint8_t *out,size_t cap,uint64_t at) {""")
sub(C,"  still.audio_ctx=r;still.audio_frames=r->audio_info.frames;r->audio_tracks++;}\n",
"""  still.audio_ctx=r;still.audio_frames=r->audio_info.frames;r->audio_tracks++;}
 else{r->audio_info=(struct live_audio_info){LIVE_AUDIO_RATE,2,16,count*8008u/5u,r->stream.clip_start,r->stream.clip_start,r->stream.clip_end};
  still.audio_get=live_audio_silence_get;still.audio_trak=live_audio_ticket_trak;still.audio_ctx=r;still.audio_frames=r->audio_info.frames;}
""")
P='src-fix37/codec/preview_writer.c'
s=(W/P).read_text();x=s.index("__attribute__((minsize)) int live_preview_write(");y=s.index("/* NCTG type 2 = ASCII")
s=s[:x]+s[y:];assert s.count('static void le32(')==1;s=s.replace('static void le32(','__attribute__((unused)) static void le32(');(W/P).write_text(s)
# (b) the recording is no longer muted around the exposure
sub('src-fix37/audio/live_audio.c',"a->duck=(struct live_audio_duck){50000,100000,5000,0};","a->duck=(struct live_audio_duck){0,0,0,32768};")
# (c) shutter hand-off state in the flight log, once per shot
sub('src-fix37/runtime.c'," crumb(r,now_us(),30u /* v28 EV_AUDIO */,",
 " crumb(r,now_us(),31u /* v55 shutter hand-off */,LOAD(&r->shutter.phase)|(r->shutter.captured&255u)<<8|(r->shutter.played&255u)<<16|(r->shutter.completed&255u)<<24,\n"
 "  (uint64_t)(r->shutter.cancelled&255u)|(uint64_t)(r->shutter.errors&255u)<<8|(uint64_t)(live_shutter_mute_get()&1u)<<16|(uint64_t)*(volatile uint8_t *)(uintptr_t)UINT64_C(0xa03bc7008)<<24|(uint64_t)(*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa02d4cb48)&255u)<<32|(uint64_t)(r->audio_z8.phase&255u)<<40);\n"
 " crumb(r,now_us(),30u /* v28 EV_AUDIO */,")
print('om3 patched')
