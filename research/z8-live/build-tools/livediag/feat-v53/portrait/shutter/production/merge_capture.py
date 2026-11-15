from pathlib import Path
import json,hashlib
P=Path(__file__).resolve().parent;W=P/'work';A=P.parents[2]/'audio'/'capture-work'/'src-fix37'
# Audio's frozen input and the existing private pause prototype remain immutable.
m=json.loads((A.parent.parent/'capture-changes.json').read_text())
for row in m['files']:
 p=A/row['path'].removeprefix('src-fix37/');assert hashlib.sha256(p.read_bytes()).hexdigest()==row['new_sha256']
oldc=(W/'audio/live_audio.c').read_text();bridge=oldc[oldc.index('/* Explicitly fill'):oldc.index('int live_audio_freeze')]
oldz=(W/'audio/live_audio_z8.c').read_text();pause=oldz[oldz.index('/* Do not use live_audio_stop/start'):]
for row in m['files']:
 rel=row['path'].removeprefix('src-fix37/');(W/rel).write_bytes((A/rel).read_bytes())
p=W/'audio/live_audio.c';s=p.read_text().replace('int live_audio_freeze',bridge+'int live_audio_freeze');p.write_text(s)
p=W/'audio/live_audio.h';s=p.read_text().replace('int live_audio_freeze','int live_audio_bridge_silence(struct live_audio *,uint64_t first_us);\nint live_audio_freeze');p.write_text(s)
p=W/'audio/live_audio_z8.h';s=p.read_text().replace('#define LAZ8_BLOCK_FRAMES 24000u','#define LAZ8_BLOCK_FRAMES 2400u').replace('#define LAZ8_SLOTS 4u','#define LAZ8_SLOTS 40u');s=s.replace(' uint64_t stop_t;',' uint64_t stop_t;\n uint32_t resume_bridge;');s=s.replace('#endif','int live_audio_z8_pause(struct live_audio *,struct live_audio_z8 *);\nint live_audio_z8_resume(struct live_audio *,struct live_audio_z8 *);\n#endif');p.write_text(s)
p=W/'audio/live_audio_z8.c';s=p.read_text().replace('z->frames=0;z->anchor_us=0;z->unmuted=0;z->saved_valid=0;','z->frames=0;z->anchor_us=0;z->unmuted=0;z->saved_valid=0;z->resume_bridge=0;');s=s.replace('  int r=live_audio_push(a,pcm,','  if(z->resume_bridge){int e=live_audio_bridge_silence(a,first);if(e)return e;z->resume_bridge=0;}\n  int r=live_audio_push(a,pcm,');s+='\n'+pause;p.write_text(s)
p=W/'runtime.h';s=p.read_text().replace('#include "audio/live_audio_z8.h"','#include "audio/live_audio_z8.h"\n#include "audio/live_shutter_z8.h"');s=s.replace(' uint32_t vhead,vpad;struct live_ev vring[64]; /* v23: view/playback changes, RAM only (no card saves) */',' uint32_t vhead,vpad;struct live_ev vring[64]; /* v23: view/playback changes, RAM only (no card saves) */\n struct live_shutter_z8 shutter; /* v53: persistent native queue ownership */');p.write_text(s)
p=W/'runtime.c';s=p.read_text().replace('static void live_audio_step','static __attribute__((section(".live_shutter"),noinline)) void live_audio_step');s=s.replace(' uint32_t job=LOAD(&r->stream.job),phase=LOAD(&r->audio_phase);',' uint32_t job=LOAD(&r->stream.job),phase=LOAD(&r->audio_phase);\n if(live_shutter_step(r,want,now))goto clip_audio;');s=s.replace(' if(job==1 && !phase)',' clip_audio:\n if(job==1 && !phase)',1);s=s.replace('static void live_audio_ready_step(struct live_runtime *r,uint64_t now) {','static void live_audio_ready_step(struct live_runtime *r,uint64_t now) {\n /* Intentional sound pause retains the previous readiness; never promotes a\n  * cold capture. First valid resumed PCM repairs only this explicit gap. */\n if(LOAD(&r->shutter.phase))return;');p.write_text(s)
print('merged frozen capture source + private pause, bridge, readiness and dispatcher interfaces')
