#!/usr/bin/env python3
"""PRIVATE NON-INTEGRATED prototype: preserve ring through native stop/start.
No firmware image or production source is changed. Native SFX deferral is not
implemented until its owned callback cancellation semantics are closed.
"""
from pathlib import Path
import hashlib,json,difflib
P=Path(__file__).resolve().parent;B=P.parents[1]/'audio/work/src-fix37/audio';W=P/'work/audio';W.mkdir(parents=True,exist_ok=True)
names=['live_audio.c','live_audio.h','live_audio_z8.c','live_audio_z8.h']
changes=[];diff=[]
for name in names:
 old=(B/name).read_text();new=old
 if name=='live_audio.h':
  key='int live_audio_freeze(struct live_audio *,struct live_audio_window);'
  new=new.replace(key,'/* Private prototype: authorize silence only for a deliberate capture pause. */\nint live_audio_bridge_silence(struct live_audio *,uint64_t first_sample_us);\n'+key)
 elif name=='live_audio.c':
  at=new.index('int live_audio_freeze(')
  new=new[:at]+'''/* Explicitly fill the capture gap after an owned pause; never used for
 * unplanned overruns. Keep the original press/PCM timeline and mark. */
int live_audio_bridge_silence(struct live_audio *a,uint64_t us) {
 if(CANCEL(a))return LAUDIO_CANCELLED;
 if(!a->active || a->frozen)return LAUDIO_STATE;
 if(!a->count)return LAUDIO_OK;
 uint64_t at=sample_floor(us),next=a->next_index;
 if(at<next)return next-at<=1u?LAUDIO_OK:LAUDIO_GAP;
 uint64_t gap=at-next;
 if(gap>LIVE_AUDIO_CAPACITY)return LAUDIO_RANGE;
 for(uint64_t i=next;i<at;i++) {
  uint32_t slot=(uint32_t)(i%LIVE_AUDIO_CAPACITY)*2u;
  a->ring[slot]=a->ring[slot+1u]=0;
 }
 if(CANCEL(a))return LAUDIO_CANCELLED;
 a->next_index=at;
 a->count=gap>LIVE_AUDIO_CAPACITY-a->count?LIVE_AUDIO_CAPACITY:a->count+(uint32_t)gap;
 a->first_index=at-a->count;
 return LAUDIO_OK;
}
''' +new[at:]
 elif name=='live_audio_z8.h':
  new=new.replace('#define LAZ8_BLOCK_FRAMES 24000u','#define LAZ8_BLOCK_FRAMES 2400u\n#define LAZ8_BLOCK_US (LAZ8_BLOCK_FRAMES * 1000000ull / LIVE_AUDIO_RATE)')
  new=new.replace('#define LAZ8_SLOTS 4u','#define LAZ8_SLOTS 40u')
  new=new.replace(' uint64_t stop_t;',' uint64_t stop_t;\n uint32_t resume_bridge;')
  new=new.replace('int live_audio_z8_poll(struct live_audio *,struct live_audio_z8 *);','int live_audio_z8_poll(struct live_audio *,struct live_audio_z8 *);\n/* PRIVATE prototype, worker-only; caller must serialize SFX/other ownership. */\nint live_audio_z8_pause(struct live_audio *,struct live_audio_z8 *);\nint live_audio_z8_resume(struct live_audio *,struct live_audio_z8 *);')
  new=new.replace('5 blocks of\n * 24000 s16 stereo frames (0x17700 B, 0.5 s).','5 blocks; this private prototype requests 2400 s16 stereo frames\n * (9600 B, 50 ms). Native API propagation is tested; physical timing is not.')
 elif name=='live_audio_z8.c':
  new=new.replace('z->frames=0;z->unmuted=0;z->saved_valid=0;','z->frames=0;z->unmuted=0;z->saved_valid=0;z->resume_bridge=0;')
  new=new.replace(' if(!z->stage || !pcm)goto done;',' if(!z->stage || !pcm || z->phase!=LAZ8_RUNNING || *z->native.service_state!=5u)goto done;')
  new=new.replace('z->stamp_us[slot]>500000u?z->stamp_us[slot]-500000u:0','z->stamp_us[slot]>LAZ8_BLOCK_US?z->stamp_us[slot]-LAZ8_BLOCK_US:0')
  new=new.replace('every block is exactly 500000 us.','every block has the configured sample count.')
  marker='  int r=live_audio_push(a,pcm,LAZ8_BLOCK_FRAMES,LAUDIO_S16LE,first);'
  new=new.replace(marker,'  if(z->resume_bridge){int e=live_audio_bridge_silence(a,first);if(e)return e;z->resume_bridge=0;}\n'+marker)
  new+='''
/* Do not use live_audio_stop/start: those discard marked pre-roll PCM.
 * This keeps the allocation and core active flag while native108 closes
 * and drains its callbacks. No SFX operation or user mute is changed here. */
int live_audio_z8_pause(struct live_audio *a,struct live_audio_z8 *z){
 if(!a->active || a->frozen || LOAD(&a->cancelled))return LAUDIO_STATE;
 if(z->phase==LAZ8_RUNNING){int e=live_audio_z8_poll(a,z);if(e)return e;}
 return z8_stop(z);
}
int live_audio_z8_resume(struct live_audio *a,struct live_audio_z8 *z){
 if(!a->active || a->frozen || LOAD(&a->cancelled) || z->phase!=LAZ8_IDLE)return LAUDIO_STATE;
 int e=z8_start(z);if(e)return LAUDIO_UNAVAILABLE;
 z->resume_bridge=1;return LAUDIO_OK;
}
'''
 (W/name).write_text(new)
 changes.append(dict(path='audio/'+name,old_sha256=hashlib.sha256(old.encode()).hexdigest(),new_sha256=hashlib.sha256(new.encode()).hexdigest()))
 diff.extend(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/audio/'+name,tofile='b/audio/'+name))
(P/'prototype.patch').write_text(''.join(diff));(P/'prototype-files.json').write_text(json.dumps(dict(production_ready=False,native_sound_deferral_implemented=False,files=changes),indent=2)+'\n')
