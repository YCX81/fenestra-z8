/* SPDX-License-Identifier: GPL-2.0-or-later
 * Passive QEMU public-plugin-API observer: no guest writes, no MMIO reads,
 * no private host offsets, no clock control. Host CLOCK_MONOTONIC only orders
 * diagnostic events; it is NOT a guest encode-time measurement. */
#define _GNU_SOURCE
#include <qemu-plugin.h>
#include <glib.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
QEMU_PLUGIN_EXPORT int qemu_plugin_version=QEMU_PLUGIN_VERSION;
#define CAP 65536u
struct header {uint64_t magic,version,record_bytes,capacity,head,overwritten,irq_count,hook_irq_count,reg_errors,reserved[3];};
struct record {uint64_t seq,host_ns,pc,x0,x1,x2,x3,lr;uint32_t cpu,kind;};
static struct header *hdr;static struct record *records;
static struct qemu_plugin_register *regs[4][6];static GByteArray *bufs[4];
static GMutex mutex;
struct site {uint64_t pc;unsigned kind;};
static struct site sites[64]={
 {0x40c25a9c,1},{0x40e90838,2},{0x40e908a0,3},{0x40e908b8,4},
 {0x40c26804,5},{0x40c26818,6},{0x40c26828,7},
 {0x40c26810,8},{0x40c26820,9},{0x40c26830,10},{0x40c26848,11},
 {0x40e90d38,12},{0x40e9bb5c,13},{0x40ea0674,14},
 {0x4104d2dc,15},{0x4104d360,16},{0x4104d600,17},{0x40fdc9c8,18}};
static unsigned site_count=18;
static void hit(unsigned cpu,void *opaque){
 if(cpu>=4)return;struct site *s=opaque;struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
 uint64_t x[5]={0};unsigned errors=0;
 for(unsigned i=0;i<5;i++){g_byte_array_set_size(bufs[cpu],0);if(regs[cpu][i]&&qemu_plugin_read_register(regs[cpu][i],bufs[cpu])==8)memcpy(&x[i],bufs[cpu]->data,8);else errors++;}
 if(s->kind==401){g_byte_array_set_size(bufs[cpu],0);if(regs[cpu][5]&&qemu_plugin_read_register(regs[cpu][5],bufs[cpu])==8)memcpy(&x[3],bufs[cpu]->data,8);else errors++;}
 struct record e={.host_ns=(uint64_t)ts.tv_sec*1000000000+ts.tv_nsec,.pc=s->pc,.x0=x[0],.x1=x[1],.x2=x[2],.x3=x[3],.lr=x[4],.cpu=cpu,.kind=s->kind};
 g_mutex_lock(&mutex);e.seq=hdr->head;records[e.seq%CAP]=e;
 hdr->reg_errors+=errors;
 if(e.x0==0x41070030u && (e.x1&1)){if(e.kind==2)hdr->irq_count++;if(e.kind==100)hdr->hook_irq_count++;}
 hdr->overwritten=e.seq+1>CAP?e.seq+1-CAP:0;__atomic_store_n(&hdr->head,e.seq+1,__ATOMIC_RELEASE);g_mutex_unlock(&mutex);
}
static void translate(qemu_plugin_id_t id,struct qemu_plugin_tb *tb){
 (void)id;for(size_t i=0;i<qemu_plugin_tb_n_insns(tb);i++){
  struct qemu_plugin_insn *in=qemu_plugin_tb_get_insn(tb,i);uint64_t pc=qemu_plugin_insn_vaddr(in);
  for(unsigned j=0;j<site_count;j++)if(pc==sites[j].pc){qemu_plugin_register_vcpu_insn_exec_cb(in,hit,QEMU_PLUGIN_CB_R_REGS,&sites[j]);break;}
 }
}
static void init_cpu(qemu_plugin_id_t id,unsigned cpu){
 (void)id;if(cpu>=4)return;bufs[cpu]=g_byte_array_sized_new(16);GArray *rs=qemu_plugin_get_registers();
 const char *names[]={"x0","x1","x2","x3","x30","x8"};
 for(unsigned i=0;i<rs->len;i++){qemu_plugin_reg_descriptor *d=&g_array_index(rs,qemu_plugin_reg_descriptor,i);for(unsigned j=0;j<6;j++)if(d->name&&!strcmp(d->name,names[j]))regs[cpu][j]=d->handle;}
 g_array_free(rs,TRUE);
}
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,const qemu_info_t *info,int argc,char **argv){
 (void)info;const char *path=0;
 for(int i=0;i<argc;i++){
  if(!strncmp(argv[i],"out=",4))path=argv[i]+4;
  else if(!strncmp(argv[i],"point=",6)){char *end;unsigned kind=strtoul(argv[i]+6,&end,0);if(*end!=':'||site_count==64)return -1;uint64_t pc=strtoull(end+1,0,0);sites[site_count++]=(struct site){pc,kind};}
  else return -1;
 }
 if(!path)return -1;int fd=open(path,O_CREAT|O_EXCL|O_RDWR,0600);size_t bytes=sizeof(*hdr)+CAP*sizeof(*records);
 if(fd<0||ftruncate(fd,bytes))return -1;hdr=mmap(0,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);close(fd);if(hdr==MAP_FAILED)return -1;
 *hdr=(struct header){.magic=UINT64_C(0x4c564f4253455256),.version=1,.record_bytes=sizeof(*records),.capacity=CAP};records=(void *)(hdr+1);
 qemu_plugin_register_vcpu_init_cb(id,init_cpu);qemu_plugin_register_vcpu_tb_trans_cb(id,translate);return 0;
}
