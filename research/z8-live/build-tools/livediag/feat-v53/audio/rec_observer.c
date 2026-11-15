#define _GNU_SOURCE
#include <qemu-plugin.h>
#include <glib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
QEMU_PLUGIN_EXPORT int qemu_plugin_version=QEMU_PLUGIN_VERSION;
static FILE *fp;static GMutex lock;
static struct qemu_plugin_register *ct[4],*cf[4];static int tf[4],ff[4];
static struct qemu_plugin_register *r0[4];static int found[4];static GByteArray *b[4],*m[4];
static void hex(FILE*f,const uint8_t*p,size_t n){for(size_t i=0;i<n;i++)fprintf(f,"%02x",p[i]);}
static uint64_t read64(unsigned cpu,uint64_t a){uint64_t v=0;if(a>=0xa00000000ull&&a<0xa40000000ull&&qemu_plugin_read_memory_vaddr(a,m[cpu],8))memcpy(&v,m[cpu]->data,8);return v;}
static uint64_t runtime(unsigned cpu){uint64_t a=read64(cpu,0xa032b50d8ull);a=read64(cpu,a+663*96+80);a=read64(cpu,a+2528);a=read64(cpu,a+16);return a;}
static void snapshot(unsigned cpu){
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
 struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
 if(pc==0x40198120||pc==0x40197e88||pc==0x40198560||pc==0x401977f8){
  g_mutex_lock(&lock);fprintf(fp,"{\"pc\":\"%llx\",\"x0\":\"%llx\",\"host_ns\":%llu",(unsigned long long)pc,(unsigned long long)a,(unsigned long long)ts.tv_sec*1000000000+ts.tv_nsec);
  if(tf[cpu]&&qemu_plugin_read_register(ct[cpu],b[cpu])){uint64_t v=0;memcpy(&v,b[cpu]->data,8);fprintf(fp,",\"cntpct\":%llu",(unsigned long long)v);}
  if(ff[cpu]&&qemu_plugin_read_register(cf[cpu],b[cpu])){uint64_t v=0;memcpy(&v,b[cpu]->data,8);fprintf(fp,",\"cntfrq\":%llu",(unsigned long long)v);}
  snapshot(cpu);fprintf(fp,"}\n");fflush(fp);g_mutex_unlock(&lock);return;
 }
 if(a<0x800000000ull||a>=0xa40000000ull)return;
 if(!qemu_plugin_read_memory_vaddr(a,m[cpu],64))return;uint32_t id;uint64_t p;memcpy(&id,m[cpu]->data+32,4);memcpy(&p,m[cpu]->data+40,8);
 g_mutex_lock(&lock);fprintf(fp,"{\"pc\":\"%llx\",\"cpu\":%u,\"event\":\"%08x\",\"payload\":",(unsigned long long)pc,cpu,id);
 if(p>=0x800000000ull&&p<0xa40000000ull&&qemu_plugin_read_memory_vaddr(p,m[cpu],64)){fputc('"',fp);hex(fp,m[cpu]->data,64);fputc('"',fp);}else fprintf(fp,"null");
 if(qemu_plugin_read_memory_vaddr(0xa031f8c4cull,m[cpu],0x110)){fprintf(fp,",\"service_cache\":\"");hex(fp,m[cpu]->data,0x110);fputc('"',fp);}
 if(qemu_plugin_read_memory_vaddr(0xa02d4cb48ull,m[cpu],0x60)){fprintf(fp,",\"driver\":\"");hex(fp,m[cpu]->data,0x60);fputc('"',fp);}
 snapshot(cpu);fprintf(fp,"}\n");fflush(fp);g_mutex_unlock(&lock);
}
static void trans(struct qemu_plugin_tb*tb,void*ud){(void)ud;for(size_t i=0;i<qemu_plugin_tb_n_insns(tb);i++){struct qemu_plugin_insn*in=qemu_plugin_tb_get_insn(tb,i);uint64_t pc=qemu_plugin_insn_vaddr(in);if(pc==0x40d233d0||pc==0x40c80988||pc==0x40198120||pc==0x40197e88||pc==0x40198560||pc==0x401977f8)qemu_plugin_register_vcpu_insn_exec_cb(in,hit,QEMU_PLUGIN_CB_R_REGS,(void*)(uintptr_t)pc);}}
static void init(unsigned cpu,void*ud){(void)ud;if(cpu>=4)return;b[cpu]=g_byte_array_new();m[cpu]=g_byte_array_new();GArray*a=qemu_plugin_get_registers();for(unsigned i=0;i<a->len;i++){qemu_plugin_reg_descriptor*d=&g_array_index(a,qemu_plugin_reg_descriptor,i);if(!strcmp(d->name,"x0")){r0[cpu]=d->handle;found[cpu]=1;}if(!g_ascii_strcasecmp(d->name,"CNTPCT_EL0")){ct[cpu]=d->handle;tf[cpu]=1;}if(!g_ascii_strcasecmp(d->name,"CNTFRQ_EL0")){cf[cpu]=d->handle;ff[cpu]=1;}}g_array_free(a,1);}
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,const qemu_info_t*info,int argc,char**argv){(void)info;if(argc!=1)return-1;fp=fopen(argv[0],"wx");if(!fp)return-1;qemu_plugin_register_vcpu_init_cb(id,init,0);qemu_plugin_register_vcpu_tb_trans_cb(id,trans,0);return 0;}
