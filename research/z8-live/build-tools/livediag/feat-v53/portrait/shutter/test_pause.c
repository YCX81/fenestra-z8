/* Private prototype only: no native SFX deferral or hardware timing asserted. */
#define main baseline_lifecycle_tests
#include "baseline_lifecycle.c"
#undef main
static void block_value(int16_t value_,int poll){
 for(unsigned i=0;i<sizeof pcm/2;i++)((int16_t*)pcm)[i]=value_;
 live_audio_z8_block(pcm,sizeof pcm);if(poll)assert(!live_audio_z8_poll(&a,&z));
}
static void begin_with_history(void){
 init();assert(!live_audio_start(&a));consume_all();
 live_audio_set_duck(&a,(struct live_audio_duck){0,0,0,32768});
 for(unsigned i=0;i<10;i++){now+=LAZ8_BLOCK_US;block_value(100,1);}
 assert(a.count==24000);assert(!live_audio_mark(&a,now-250000));
}
static void pause_complete(void){
 void*descriptor=a.descriptor;uint32_t freed=frees,count=a.count;uint64_t next=a.next_index,press=a.press_us;
 assert(live_audio_z8_pause(&a,&z)==LAUDIO_BUSY);uint32_t head=z.head;
 block_value(999,0);assert(z.head==head); /* callback while admission closed */
 consume_all();assert(!live_audio_z8_pause(&a,&z));
 assert(z.phase==LAZ8_IDLE&&a.active&&a.descriptor==descriptor&&frees==freed&&a.count==count&&a.next_index==next&&a.marked&&a.press_us==press&&!a.cancelled);
}
int main(void){
 baseline_lifecycle_tests();
 begin_with_history();uint64_t begin=now-500000;uint32_t allocated=allocs;void*descriptor=a.descriptor;
 pause_complete();now+=300000;assert(!live_audio_z8_resume(&a,&z));
 uint32_t head=z.head;block_value(999,0);assert(z.head==head); /* old callback before new GO/state5 */
 consume_all();for(unsigned i=0;i<20;i++){now+=LAZ8_BLOCK_US;block_value(200,1);}
 assert(a.descriptor==descriptor&&allocs==allocated&&a.marked);
 assert(!live_audio_freeze(&a,(struct live_audio_window){begin,begin+1500000}));
 int16_t*out=calloc(72000*2,sizeof *out);uint32_t got;struct live_audio_info info;
 assert(!live_audio_get(&a,0,out,72000,&got,&info)&&got==72000&&info.frames==72000);
 for(unsigned i=0;i<72000;i++)for(unsigned ch=0;ch<2;ch++)assert(out[2*i+ch]==(i<24000?100:i<38400?0:200));free(out);
 assert(live_audio_stop(&a)==LAUDIO_BUSY);consume_all();assert(!live_audio_stop(&a));
 puts("PASS pause preserves exact pre-roll/mark/allocation; resume inserts exactly300ms silence; original absolute audio window unchanged");

 begin_with_history();pause_complete();live_audio_cancel(&a);assert(live_audio_z8_resume(&a,&z)==LAUDIO_STATE);
 assert(!live_audio_stop(&a));assert(allocs==frees);block_value(777,0);
 puts("PASS cancel while paused releases only after native idle; resume rejected; late closed callback inert");

 begin_with_history();pause_complete();fail_post=0x4204010b;assert(live_audio_z8_resume(&a,&z)==LAUDIO_UNAVAILABLE);
 assert(a.active&&a.descriptor&&z.phase==LAZ8_STARTING);assert(live_audio_stop(&a)==LAUDIO_BUSY);consume_all();assert(!live_audio_stop(&a));
 puts("PASS partial resume failure retains PCM/staging until queued native STOP ACK");

 begin_with_history();uint64_t next=a.next_index;uint32_t count=a.count;uint64_t press=a.press_us;
 assert(live_audio_bridge_silence(&a,now+4000125)==LAUDIO_RANGE);assert(a.next_index==next&&a.count==count&&a.press_us==press&&a.marked);
 assert(live_audio_bridge_silence(&a,now-125)==LAUDIO_GAP);assert(a.next_index==next&&a.count==count&&a.marked);
 for(unsigned i=0;i<40;i++){now+=LAZ8_BLOCK_US;block_value(300,0);}uint32_t head40=z.head;
 now+=LAZ8_BLOCK_US;block_value(400,0);assert(z.head==head40&&z.overrun&&z.dropped==1);
 assert(live_audio_z8_poll(&a,&z)==LAUDIO_GAP);assert(z.tail==head40&&!z.overrun);
 assert(live_audio_stop(&a)==LAUDIO_BUSY);consume_all();assert(!live_audio_stop(&a));
 printf("PASS50ms×40 stage retains2s capacity; overflow bounded, no silent overwrite; ABI z8=%zu stage=%u\n",sizeof z,LAZ8_STAGE_BYTES);
 return 0;
}
