/* Passive original instruction/descriptor trace; public QEMU10 API, no writes. */
#define _GNU_SOURCE
#include <qemu-plugin.h>
#include <glib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
QEMU_PLUGIN_EXPORT int qemu_plugin_version=QEMU_PLUGIN_VERSION;
static FILE*f;static GMutex lock;static GByteArray*b[4],*m[4];
static struct qemu_plugin_register*r[4][5];static int seen[4][5];
static void bytes(unsigned cpu,uint64_t p,size_t n,const char*name){if(p<0x800000000ull||p>=0xa40000000ull)return;g_byte_array_set_size(m[cpu],0);if(!qemu_plugin_read_memory_vaddr(p,m[cpu],n))return;fprintf(f,",\"%s\":\"",name);for(size_t i=0;i<n;i++)fprintf(f,"%02x",m[cpu]->data[i]);fputc('"',f);}
static void hit(unsigned cpu,void*opaque){if(cpu>=4)return;uint64_t x[5]={0},pc=(uintptr_t)opaque;for(unsigned i=0;i<5;i++){g_byte_array_set_size(b[cpu],0);if(seen[cpu][i]&&qemu_plugin_read_register(r[cpu][i],b[cpu])==8)memcpy(x+i,b[cpu]->data,8);}g_mutex_lock(&lock);fprintf(f,"{\"pc\":\"%llx\",\"cpu\":%u,\"x0\":\"%llx\",\"x1\":\"%llx\",\"x2\":\"%llx\",\"lr\":\"%llx\"",(unsigned long long)pc,cpu,(unsigned long long)x[0],(unsigned long long)x[1],(unsigned long long)x[2],(unsigned long long)x[4]);if(pc==0x40c16674)bytes(cpu,x[1],48,"descriptor");if(pc==0x40c80988)bytes(cpu,x[0],56,"message");bytes(cpu,0xa02d4cb48,8,"driver");fprintf(f,"}\n");fflush(f);g_mutex_unlock(&lock);}
static void trans(qemu_plugin_id_t id,struct qemu_plugin_tb*tb){(void)id;for(size_t i=0;i<qemu_plugin_tb_n_insns(tb);i++){struct qemu_plugin_insn*in=qemu_plugin_tb_get_insn(tb,i);uint64_t pc=qemu_plugin_insn_vaddr(in);if(pc==0x40c16674||pc==0x40c18160||pc==0x40c80988||pc==0x40c7e558||pc==0x40c18014)qemu_plugin_register_vcpu_insn_exec_cb(in,hit,QEMU_PLUGIN_CB_R_REGS,(void*)(uintptr_t)pc);}}
static void init(qemu_plugin_id_t id,unsigned cpu){(void)id;if(cpu>=4)return;b[cpu]=g_byte_array_new();m[cpu]=g_byte_array_new();const char*names[]={"x0","x1","x2","x3","x30"};GArray*a=qemu_plugin_get_registers();for(unsigned i=0;i<a->len;i++){qemu_plugin_reg_descriptor*d=&g_array_index(a,qemu_plugin_reg_descriptor,i);for(unsigned j=0;j<5;j++)if(!strcmp(d->name,names[j])){r[cpu][j]=d->handle;seen[cpu][j]=1;}}g_array_free(a,1);}
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,const qemu_info_t*info,int argc,char**argv){(void)info;if(argc!=1||strncmp(argv[0],"out=",4))return-1;f=fopen(argv[0]+4,"wx");if(!f)return-1;qemu_plugin_register_vcpu_init_cb(id,init);qemu_plugin_register_vcpu_tb_trans_cb(id,trans);return 0;}
