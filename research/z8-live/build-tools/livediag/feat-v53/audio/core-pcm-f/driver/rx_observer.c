#define _GNU_SOURCE
#include <qemu-plugin.h>
#include <glib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
QEMU_PLUGIN_EXPORT int qemu_plugin_version=QEMU_PLUGIN_VERSION;
static int low_write[4];static FILE *fp;static GMutex lock;static char* pcm_path;static int captured;
static struct qemu_plugin_register *ct[4],*cf[4];static int tf[4],ff[4];
static struct qemu_plugin_register *r0[4],*r1[4];static int f1[4];static int found[4];static GByteArray *b[4],*m[4];
static void hex(FILE*f,const uint8_t*p,size_t n){for(size_t i=0;i<n;i++)fprintf(f,"%02x",p[i]);}
static uint64_t read64(unsigned cpu,uint64_t a){uint64_t v=0;if(a>=0xa00000000ull&&a<0xa40000000ull&&qemu_plugin_read_memory_vaddr(a,m[cpu],8))memcpy(&v,m[cpu]->data,8);return v;}
static uint64_t runtime(unsigned cpu){uint64_t a=read64(cpu,0xa032b50d8ull);a=read64(cpu,a+663*96+80);a=read64(cpu,a+2528);a=read64(cpu,a+16);return a;}
static void live_snapshot(unsigned cpu){
 uint64_t a=read64(cpu,0xa032b50d8ull);a=read64(cpu,a+663*96+80);a=read64(cpu,a+2528);a=read64(cpu,a+8);a=read64(cpu,a+8);
 if(a<0xa00000000ull||a>=0xa40000000ull||!qemu_plugin_read_memory_vaddr(a,m[cpu],23744))return;
 uint32_t magic;memcpy(&magic,m[cpu]->data,4);if(magic!=0x4c415231)return;
 fprintf(fp,",\"live\":{");
 {uint64_t v=0;memcpy(&v,m[cpu]->data+1752,8);fprintf(fp,"\"poll_count\":%llu",(unsigned long long)v);}
 {uint64_t v=0;memcpy(&v,m[cpu]->data+1696,8);fprintf(fp,",\"capture.irq_timestamp\":%llu",(unsigned long long)v);}
 {uint64_t v=0;memcpy(&v,m[cpu]->data+1688,8);fprintf(fp,",\"capture.irq_seq\":%llu",(unsigned long long)v);}
 {uint64_t v=0;memcpy(&v,m[cpu]->data+1620,4);fprintf(fp,",\"capture.phase\":%llu",(unsigned long long)v);}
 {uint64_t v=0;memcpy(&v,m[cpu]->data+56,8);fprintf(fp,",\"capture.descriptor\":%llu",(unsigned long long)v);}
 {uint64_t v=0;memcpy(&v,m[cpu]->data+12712,8);fprintf(fp,",\"stream.encoded\":%llu",(unsigned long long)v);}
 {uint64_t v=0;memcpy(&v,m[cpu]->data+12632,4);fprintf(fp,",\"stream.width\":%llu",(unsigned long long)v);}
 {uint64_t v=0;memcpy(&v,m[cpu]->data+9096,8);fprintf(fp,",\"stream.encoder.hw\":%llu",(unsigned long long)v);}
 {uint64_t v=0;memcpy(&v,m[cpu]->data+18088,4);fprintf(fp,",\"audio.active\":%llu",(unsigned long long)v);}
 {uint64_t v=0;memcpy(&v,m[cpu]->data+18100,4);fprintf(fp,",\"audio.cancelled\":%llu",(unsigned long long)v);}
 {uint64_t v=0;memcpy(&v,m[cpu]->data+19096,4);fprintf(fp,",\"audio_z8.phase\":%llu",(unsigned long long)v);}
 {uint64_t v=0;memcpy(&v,m[cpu]->data+18680,8);fprintf(fp,",\"audio_z8.go\":%llu",(unsigned long long)v);}
 {uint64_t v=0;memcpy(&v,m[cpu]->data+19100,4);fprintf(fp,",\"audio_z8.unmuted\":%llu",(unsigned long long)v);}
 {uint64_t v=0;memcpy(&v,m[cpu]->data+19136,4);fprintf(fp,",\"audio_z8.blocks\":%llu",(unsigned long long)v);}
 {uint64_t v=0;memcpy(&v,m[cpu]->data+19092,4);fprintf(fp,",\"audio_z8.closed\":%llu",(unsigned long long)v);}
 {uint64_t v=0;memcpy(&v,m[cpu]->data+19156,4);fprintf(fp,",\"audio_z8.stop_ack\":%llu",(unsigned long long)v);}
 {uint64_t v=0;memcpy(&v,m[cpu]->data+19144,4);fprintf(fp,",\"audio_z8.stop_timeouts\":%llu",(unsigned long long)v);}
 {uint64_t v=0;memcpy(&v,m[cpu]->data+19236,4);fprintf(fp,",\"audio_warm\":%llu",(unsigned long long)v);}
 {uint64_t v=0;memcpy(&v,m[cpu]->data+19220,4);fprintf(fp,",\"audio_error\":%llu",(unsigned long long)v);}
 {uint64_t v=0;memcpy(&v,m[cpu]->data+19224,4);fprintf(fp,",\"audio_starts\":%llu",(unsigned long long)v);}
 {uint64_t v=0;memcpy(&v,m[cpu]->data+20076,4);fprintf(fp,",\"crumb_pad\":%llu",(unsigned long long)v);}
 {uint64_t v=0;memcpy(&v,m[cpu]->data+23576,4);fprintf(fp,",\"shutter.phase\":%llu",(unsigned long long)v);}
 {uint64_t v=0;memcpy(&v,m[cpu]->data+23604,4);fprintf(fp,",\"shutter.captured\":%llu",(unsigned long long)v);}
 {uint64_t v=0;memcpy(&v,m[cpu]->data+23608,4);fprintf(fp,",\"shutter.played\":%llu",(unsigned long long)v);}
 {uint64_t v=0;memcpy(&v,m[cpu]->data+23612,4);fprintf(fp,",\"shutter.completed\":%llu",(unsigned long long)v);}
 {uint64_t v=0;memcpy(&v,m[cpu]->data+23620,4);fprintf(fp,",\"shutter.errors\":%llu",(unsigned long long)v);}
fprintf(fp,"}");uint64_t v=read64(cpu,0xa02d3274c);fprintf(fp,",\"w5_state\":%u",(unsigned)v);}
static void snapshot(unsigned cpu){
 live_snapshot(cpu);
 fprintf(fp,",\"volume_cache\":%u,\"volume_preference\":%u",(unsigned)(read64(cpu,0xa033868d0ull)&65535),(unsigned)(read64(cpu,0xa03d47afcull)&65535));
 uint64_t r=runtime(cpu);if(r<0xa00000000ull||r>=0xa40000000ull)return;
 if(!qemu_plugin_read_memory_vaddr(r,m[cpu],9352))return;
 uint32_t magic=0;memcpy(&magic,m[cpu]->data,4);if(magic!=0x50425549)return;
 fprintf(fp,",\"pb\":{");
 const unsigned offs[]={72,76,488,9284,9292,9304,9300,9296,9332,9328,9340,8220,8224,24};
 const char*names[]={"frames","playing","start_us","audio_stage","audio_bytes","audio_t","audio_plays","audio_errors","audio_source_frames","audio_reason","audio_gate","rendered_frames","consumed_rec","start_request"};
 for(unsigned i=0;i<14;i++){uint64_t v=0;memcpy(&v,m[cpu]->data+offs[i],i==2||i==5?8:4);fprintf(fp,"%s\"%s\":%llu",i?",":"",names[i],(unsigned long long)v);}fprintf(fp,"}");
}
static void hit(unsigned cpu,void *opaque){
 if(cpu>=4)return;uint64_t a=0,pc=(uintptr_t)opaque;g_byte_array_set_size(b[cpu],0);
 if(!found[cpu]||!qemu_plugin_read_register(r0[cpu],b[cpu])||b[cpu]->len!=8)return;memcpy(&a,b[cpu]->data,8);
 if(pc==0x40c7f824){low_write[cpu]=a<=0x16;if(!low_write[cpu])return;}if(pc==0x40c7f944&&!low_write[cpu])return;
 if(pc==0x401fb7c0){
  uint64_t count=0;g_byte_array_set_size(b[cpu],0);if(f1[cpu]&&qemu_plugin_read_register(r1[cpu],b[cpu])&&b[cpu]->len==8)memcpy(&count,b[cpu]->data,8);
  g_mutex_lock(&lock);fprintf(fp,"{\"pc\":\"401fb7c0\",\"native_pcm_callback_bytes\":%llu,\"pcm_first64\":\"",(unsigned long long)count);
  if(count==9600&&qemu_plugin_read_memory_vaddr(a,m[cpu],64))hex(fp,m[cpu]->data,64);fprintf(fp,"\"");snapshot(cpu);fprintf(fp,"}\n");fflush(fp);g_mutex_unlock(&lock);return;
 }
 struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
 if(pc==0x40c7fd08||pc==0x40c7fe3c||pc==0x40c7f824||pc==0x40c7f944||pc==0x40c16674||pc==0x40c17954||pc==0x40c17c48||pc==0x40c17cf8||pc==0x40198128||pc==0x40197e90||pc==0x40198568||pc==0x40197800){
  g_mutex_lock(&lock);fprintf(fp,"{\"pc\":\"%llx\",\"x0\":\"%llx\",\"host_ns\":%llu",(unsigned long long)pc,(unsigned long long)a,(unsigned long long)ts.tv_sec*1000000000+ts.tv_nsec);
  g_byte_array_set_size(b[cpu],0);if(tf[cpu]&&qemu_plugin_read_register(ct[cpu],b[cpu])&&b[cpu]->len==8){uint64_t v=0;memcpy(&v,b[cpu]->data,8);fprintf(fp,",\"cntpct\":%llu",(unsigned long long)v);}
  g_byte_array_set_size(b[cpu],0);if(ff[cpu]&&qemu_plugin_read_register(cf[cpu],b[cpu])&&b[cpu]->len==8){uint64_t v=0;memcpy(&v,b[cpu]->data,8);fprintf(fp,",\"cntfrq\":%llu",(unsigned long long)v);}
  if(pc==0x40c16674&&f1[cpu]){g_byte_array_set_size(b[cpu],0);if(qemu_plugin_read_register(r1[cpu],b[cpu])&&b[cpu]->len==8){uint64_t q;memcpy(&q,b[cpu]->data,8);if(q>=0x800000000ull&&q<0xa40000000ull&&qemu_plugin_read_memory_vaddr(q,m[cpu],48)){fprintf(fp,",\"dma_descriptor\":\"");hex(fp,m[cpu]->data,48);fputc('"',fp);}}}
  if(pc==0x40c7fd08){
   if(qemu_plugin_read_memory_vaddr(0xa02d4c908ull,m[cpu],0x28)){fprintf(fp,",\"codec_globals\":\"");hex(fp,m[cpu]->data,0x28);fputc('"',fp);}
   uint64_t gp=read64(cpu,0xa02d4c908ull);if(gp>=0xa00000000ull&&qemu_plugin_read_memory_vaddr(gp,m[cpu],64)){fprintf(fp,",\"gpio_object\":\"");hex(fp,m[cpu]->data,64);fputc('"',fp);}
   if(qemu_plugin_read_memory_vaddr(0xa028135f0ull,m[cpu],0xe8*3)){fprintf(fp,",\"serial_handles012\":\"");hex(fp,m[cpu]->data,0xe8*3);fputc('"',fp);}
   if(qemu_plugin_read_memory_vaddr(0xa02808780ull,m[cpu],0x68*3)){fprintf(fp,",\"serial_objects012\":\"");hex(fp,m[cpu]->data,0x68*3);fputc('"',fp);}
  }
  snapshot(cpu);fprintf(fp,"}\n");fflush(fp);g_mutex_unlock(&lock);return;
 }
 if(a<0x800000000ull||a>=0xa40000000ull)return;
 if(!qemu_plugin_read_memory_vaddr(a,m[cpu],64))return;uint32_t id;uint64_t p;memcpy(&id,m[cpu]->data+32,4);memcpy(&p,m[cpu]->data+40,8);
 if(id==0x4318001a&&!captured&&p>=0x800000000ull&&qemu_plugin_read_memory_vaddr(p,m[cpu],16)){uint64_t src;uint32_t n,count;memcpy(&src,m[cpu]->data,8);memcpy(&n,m[cpu]->data+8,4);memcpy(&count,m[cpu]->data+12,4);if(count&&count<=8&&n<=1000000/count)n*=count;else n=0;if(n&&n<=1000000&&qemu_plugin_read_memory_vaddr(src,m[cpu],n)){FILE*pf=fopen(pcm_path,"wx");if(pf){fwrite(m[cpu]->data,1,n,pf);fclose(pf);captured=1;}}}
 g_mutex_lock(&lock);fprintf(fp,"{\"pc\":\"%llx\",\"cpu\":%u,\"event\":\"%08x\",\"payload\":",(unsigned long long)pc,cpu,id);
 if(p>=0x800000000ull&&p<0xa40000000ull&&qemu_plugin_read_memory_vaddr(p,m[cpu],64)){fputc('"',fp);hex(fp,m[cpu]->data,64);fputc('"',fp);}else fprintf(fp,"null");
 if(qemu_plugin_read_memory_vaddr(0xa031f8c4cull,m[cpu],0x110)){fprintf(fp,",\"service_cache\":\"");hex(fp,m[cpu]->data,0x110);fputc('"',fp);}
 if(qemu_plugin_read_memory_vaddr(0xa02d4cb48ull,m[cpu],0x60)){fprintf(fp,",\"driver\":\"");hex(fp,m[cpu]->data,0x60);fputc('"',fp);}
 snapshot(cpu);fprintf(fp,"}\n");fflush(fp);g_mutex_unlock(&lock);
}
static void trans(qemu_plugin_id_t id,struct qemu_plugin_tb*tb){(void)id;for(size_t i=0;i<qemu_plugin_tb_n_insns(tb);i++){struct qemu_plugin_insn*in=qemu_plugin_tb_get_insn(tb,i);uint64_t pc=qemu_plugin_insn_vaddr(in);if(pc==0x401fb7c0||pc==0x40c7fd08||pc==0x40c7fe3c||pc==0x40c7f824||pc==0x40c7f944||pc==0x40d233d0||pc==0x40c80988||pc==0x40198128||pc==0x40197e90||pc==0x40198568||pc==0x40197800||pc==0x40c17954||pc==0x40c17c48||pc==0x40c17cf8||pc==0x40c16674)qemu_plugin_register_vcpu_insn_exec_cb(in,hit,QEMU_PLUGIN_CB_R_REGS,(void*)(uintptr_t)pc);}}
static void init(qemu_plugin_id_t id,unsigned cpu){(void)id;if(cpu>=4)return;b[cpu]=g_byte_array_new();m[cpu]=g_byte_array_new();GArray*a=qemu_plugin_get_registers();for(unsigned i=0;i<a->len;i++){qemu_plugin_reg_descriptor*d=&g_array_index(a,qemu_plugin_reg_descriptor,i);if(!strcmp(d->name,"x0")){r0[cpu]=d->handle;found[cpu]=1;}if(!strcmp(d->name,"x1")){r1[cpu]=d->handle;f1[cpu]=1;}if(!g_ascii_strcasecmp(d->name,"CNTPCT_EL0")){ct[cpu]=d->handle;tf[cpu]=1;}if(strstr(d->name,"CNT")){fprintf(fp,"{\"register_name\":\"%s\",\"cpu\":%u}\n",d->name,cpu);fflush(fp);}if(!g_ascii_strcasecmp(d->name,"CNTFRQ_EL0")){cf[cpu]=d->handle;ff[cpu]=1;}}g_array_free(a,1);}
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,const qemu_info_t*info,int argc,char**argv){(void)info;if(argc!=1)return-1;pcm_path=g_strdup_printf("%s.pcm",argv[0]);fp=fopen(argv[0],"wx");if(!fp)return-1;qemu_plugin_register_vcpu_init_cb(id,init);qemu_plugin_register_vcpu_tb_trans_cb(id,trans);return 0;}
