#include "pause_helpers.inc"
int main(void){
 begin_with_history();
 for(unsigned i=0;i<50;i++){now+=LAZ8_BLOCK_US;block_value(100,1);}
 a.press_us=now-1500000;a.exposure_us=a.press_us;uint64_t end=now;
 pause_complete();now+=100000;assert(!live_audio_z8_resume(&a,&z));consume_all();assert(!live_audio_z8_poll(&a,&z));
 assert(z.resume_bridge && !a.frozen);assert(!live_audio_freeze(&a,(struct live_audio_window){end-3000000,end}));
 uint64_t first=a.freeze_first;uint32_t frames=a.freeze_frames;void *saved=malloc(LIVE_AUDIO_BYTES);memcpy(saved,a.ring,LIVE_AUDIO_BYTES);
 now+=LAZ8_BLOCK_US;block_value(200,0);int rc=live_audio_z8_poll(&a,&z);
 printf("freeze-after-resume poll_rc=%d frozen=%u first=%llu frames=%u bridge=%u next=%llu\n",rc,a.frozen,(unsigned long long)a.freeze_first,a.freeze_frames,z.resume_bridge,(unsigned long long)a.next_index);
 assert(a.frozen&&a.freeze_first==first&&a.freeze_frames==frames);
 assert(rc==LAUDIO_OK && z.resume_bridge && !memcmp(saved,a.ring,LIVE_AUDIO_BYTES));free(saved);
 live_audio_thaw(&a);now+=LAZ8_BLOCK_US;block_value(300,0);assert(!live_audio_z8_poll(&a,&z));
 assert(!z.resume_bridge && a.count==LAZ8_BLOCK_FRAMES && !a.frozen);
 assert(live_audio_stop(&a)==LAUDIO_BUSY);consume_all();assert(!live_audio_stop(&a));
 return 0;
}
