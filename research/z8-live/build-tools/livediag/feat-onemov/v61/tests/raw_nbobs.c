/* QEMU plugin: log x0..x7,x30 and memory at x0/x1 at given PCs. args: out=FILE pts=hex,hex,... */
#define _GNU_SOURCE
#include <qemu-plugin.h>
#include <glib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
QEMU_PLUGIN_EXPORT int qemu_plugin_version=QEMU_PLUGIN_VERSION;
/* Test-only extra reads at the bridge observer; compile-time offsets from final header. */
#ifndef RAW_OBSERVE_PC
#error RAW observer layout required
#endif
static FILE *fp;static GMutex guard;
static struct qemu_plugin_register *reg[4][9];static unsigned found[4];
static GByteArray *bytes[4],*mem[4];
static uint64_t points[64];static unsigned npoints;
static void dump(unsigned cpu,const char *name,uint64_t addr,unsigned n){
 if(addr<0x40000000ull||addr>0x1000000000ull-n)return;
 if(!qemu_plugin_read_memory_vaddr(addr,mem[cpu],n))return;
 fprintf(fp,",\"%s\":\"",name);
 for(unsigned i=0;i<mem[cpu]->len;i++)fprintf(fp,"%02x",mem[cpu]->data[i]);
 fputc('"',fp);
}
static void hit(unsigned cpu,void *opaque){
 if(cpu>=4)return;uint64_t v[9]={0},pc=(uintptr_t)opaque;
 for(unsigned i=0;i<9;i++){
  g_byte_array_set_size(bytes[cpu],0);
  if(!(found[cpu]&(1u<<i))||!qemu_plugin_read_register(reg[cpu][i],bytes[cpu])||bytes[cpu]->len!=8)return;
  memcpy(v+i,bytes[cpu]->data,8);
 }
 g_mutex_lock(&guard);
 fprintf(fp,"{\"pc\":\"%llx\",\"cpu\":%u,\"regs\":[",(unsigned long long)pc,cpu);
 for(unsigned i=0;i<9;i++)fprintf(fp,"%s\"%llx\"",i?",":"",(unsigned long long)v[i]);
 fputc(']',fp);dump(cpu,"x0_mem",v[0],256);dump(cpu,"x1_mem",v[1],96);
 if(pc==RAW_OBSERVE_PC && v[0]>=RAW_REQUEST_OFFSET){
  uint64_t base=v[0]-RAW_REQUEST_OFFSET,node=0;
  dump(cpu,"selected_native",base+RAW_NATIVE_OFFSET,64);
  dump(cpu,"manifest_native",base+RAW_MANIFEST_OFFSET,64);
  if(qemu_plugin_read_memory_vaddr(base+RAW_NODE_OFFSET,mem[cpu],8)&&mem[cpu]->len==8){
   memcpy(&node,mem[cpu]->data,8);if(node)dump(cpu,"dcf_member_mask",node,2);
  }
 }

 dump(cpu,"file_info",0xa045f08f8ull,0x60);dump(cpu,"crop",0xa0319b3b8ull,0x40);
 fprintf(fp,"}\n");fflush(fp);g_mutex_unlock(&guard);
}
static void trans(struct qemu_plugin_tb *tb,void *ud){
 (void)ud;
 for(size_t i=0;i<qemu_plugin_tb_n_insns(tb);i++){
  struct qemu_plugin_insn *in=qemu_plugin_tb_get_insn(tb,i);uint64_t pc=qemu_plugin_insn_vaddr(in);
  for(unsigned j=0;j<npoints;j++)if(pc==points[j])
   qemu_plugin_register_vcpu_insn_exec_cb(in,hit,QEMU_PLUGIN_CB_R_REGS,(void*)(uintptr_t)pc);
 }
}
static void init(unsigned cpu,void *ud){
 (void)ud;if(cpu>=4)return;bytes[cpu]=g_byte_array_new();mem[cpu]=g_byte_array_new();
 const char *names[]={"x0","x1","x2","x3","x4","x5","x6","x7","x30"};GArray *a=qemu_plugin_get_registers();
 for(unsigned i=0;i<a->len;i++){
  qemu_plugin_reg_descriptor *d=&g_array_index(a,qemu_plugin_reg_descriptor,i);
  for(unsigned j=0;j<9;j++)if(!strcmp(d->name,names[j])){reg[cpu][j]=d->handle;found[cpu]|=1u<<j;}
 }
 g_array_free(a,1);
}
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,const qemu_info_t *info,int argc,char **argv){
 (void)info;
 for(int i=0;i<argc;i++){
  if(!strncmp(argv[i],"out=",4)){fp=fopen(argv[i]+4,"w");}
  else if(!strncmp(argv[i],"pts=",4)){char *s=g_strdup(argv[i]+4);for(char *t=strtok(s,":");t&&npoints<64;t=strtok(0,":"))points[npoints++]=strtoull(t,0,16);}
 }
 if(!fp||!npoints)return -1;
 qemu_plugin_register_vcpu_init_cb(id,init,0);qemu_plugin_register_vcpu_tb_trans_cb(id,trans,0);return 0;
}
