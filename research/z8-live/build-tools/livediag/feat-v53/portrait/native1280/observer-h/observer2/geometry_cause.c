#define _GNU_SOURCE
#include <qemu-plugin.h>
#include <glib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
QEMU_PLUGIN_EXPORT int qemu_plugin_version=QEMU_PLUGIN_VERSION;
/* Host monotonic time only: virtual clock access from a TCG callback is unsafe under icount. */
static FILE *fp;static GMutex guard;
static struct qemu_plugin_register *reg[4][8];
static unsigned found[4];
static GByteArray *bytes[4],*mem[4];
static const uint64_t points[]={0x40719000,0x40719988,0x403f7038,0x403f6dc0,0x40cf7c8c,0x40cf7d08,0x40cf7e18,0x40ca2664,0x40cf8000,0x40d02e24,0x40d02e3c,0x40cb84cc,0x40cb7718,0x40cb7b54,0x40c270d0,0x403f6fac,0x40718484,0x4071867c,0x40719a90,
 0x40cf8668,0x40cf8710,0x40cfc820,0x40cf8614,0x403f6e88,0x403f6f10,
 0x40712ca8,0x406bdd50,0x408747d8,0x40344b20,0x402dd4f8,
 0x403ec3a0,0x403ad6a4,0x40634728,0x406364d0,0x40635fd8,
 0x4066c3f8,0x403f7244,0x403f6fe8,0x40710820,0x4085a8e0,0x407129e0,0x407b95e4};
static void dump(unsigned cpu,const char *name,uint64_t addr,unsigned n){
 if(addr<0x800000000ull||addr>0xa40000000ull-n)return;
 if(!qemu_plugin_read_memory_vaddr(addr,mem[cpu],n))return;
 fprintf(fp,",\"%s\":\"",name);
 for(unsigned i=0;i<mem[cpu]->len;i++)fprintf(fp,"%02x",mem[cpu]->data[i]);
 fputc('"',fp);
}
static void hit(unsigned cpu,void *opaque){
 if(cpu>=4)return;uint64_t v[8]={0},pc=(uintptr_t)opaque;
 for(unsigned i=0;i<8;i++){
  g_byte_array_set_size(bytes[cpu],0);
  if(!(found[cpu]&(1u<<i))||!qemu_plugin_read_register(reg[cpu][i],bytes[cpu])||bytes[cpu]->len!=8)return;
  memcpy(v+i,bytes[cpu]->data,8);
 }
 if(pc==0x403ec3a0&&v[0]!=0x4910&&v[0]!=0x4919&&v[0]!=0x491a)return;
 if((pc==0x40719000||pc==0x40719988)&&v[0]!=0x8000000)return;
 if(pc==0x403f7038&&v[0]!=0x4090105)return;
 g_mutex_lock(&guard);
 fprintf(fp,"{\"pc\":\"%llx\",\"cpu\":%u,\"regs\":[",(unsigned long long)pc,cpu);
 for(unsigned i=0;i<8;i++)fprintf(fp,"%s\"%llx\"",i?",":"",(unsigned long long)v[i]);
 fputc(']',fp);fprintf(fp,",\"host_monotonic_us\":%lld",(long long)g_get_monotonic_time());if(pc==0x403f6dc0)dump(cpu,"stop_events",0xa032cd300ull,16);dump(cpu,"x0_mem",v[0],128);dump(cpu,"x1_mem",v[1],512);
 if(pc!=0x402dd4f8){dump(cpu,"current_movie",0xa045f07d8ull,0x120);dump(cpu,"movie_ids",0xa045f4208ull,40);dump(cpu,"file_info",0xa045f08f8ull,0x60);}
 dump(cpu,"crop",0xa0319b3b8ull,64);dump(cpu,"decoder",0xa0319f270ull,64);dump(cpu,"config",0xa031957e8ull,128);
 fprintf(fp,"}\n");fflush(fp);g_mutex_unlock(&guard);
}
static void trans(struct qemu_plugin_tb *tb,void *ud){
 (void)ud;
 for(size_t i=0;i<qemu_plugin_tb_n_insns(tb);i++){
  struct qemu_plugin_insn *in=qemu_plugin_tb_get_insn(tb,i);uint64_t pc=qemu_plugin_insn_vaddr(in);
  for(unsigned j=0;j<sizeof(points)/sizeof(points[0]);j++)if(pc==points[j])
   qemu_plugin_register_vcpu_insn_exec_cb(in,hit,QEMU_PLUGIN_CB_R_REGS,(void*)(uintptr_t)pc);
 }
}
static void init(unsigned cpu,void *ud){
 (void)ud;if(cpu>=4)return;bytes[cpu]=g_byte_array_new();mem[cpu]=g_byte_array_new();
 const char *names[]={"x0","x1","x2","x3","x30","x8","x9","x19"};GArray *a=qemu_plugin_get_registers();
 for(unsigned i=0;i<a->len;i++){
  qemu_plugin_reg_descriptor *d=&g_array_index(a,qemu_plugin_reg_descriptor,i);
  for(unsigned j=0;j<8;j++)if(!strcmp(d->name,names[j])){reg[cpu][j]=d->handle;found[cpu]|=1u<<j;}
 }
 g_array_free(a,1);
}
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,const qemu_info_t *info,int argc,char **argv){
 (void)info;if(argc!=1||strncmp(argv[0],"out=",4))return -1;
 fp=fopen(argv[0]+4,"wx");if(!fp)return -1;
 qemu_plugin_register_vcpu_init_cb(id,init,0);qemu_plugin_register_vcpu_tb_trans_cb(id,trans,0);return 0;
}
