/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <inttypes.h>
#include <assert.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <zlib.h>
#define MiB (1024ULL*1024)
#define QEMU_CLOCK_VIRTUAL 0
#define QEMU_THREAD_JOINABLE 0
#define MEMTX_OK 0
#define MEMTXATTRS_UNSPECIFIED 0
#define W5P_UNIT_TEST 1
typedef uint64_t hwaddr;
typedef pthread_mutex_t QemuMutex;
typedef pthread_t QemuThread;
typedef struct { int pending; } QEMUTimer;
typedef int *qemu_irq;
static uint64_t clock_ns;
static int address_space_memory;
static uint8_t ram[32*MiB];
static const uint64_t ram_base=0xa00000000ULL;
typedef struct NikonZ9State {
    struct W5pState *w5p;
    bool w5p_model,wave5_busy,w5p_host_codec_diagnostic;
    uint32_t video_processing_global[4];
} NikonZ9State;
static bool dma_ram_range(NikonZ9State *s,uint64_t a,uint64_t n) { return n && a>=ram_base && a-ram_base<sizeof(ram) && n<=sizeof(ram)-(a-ram_base); }
static bool raw_pipeline_address(NikonZ9State *s,uint64_t a,uint64_t n,uint64_t *p) { return false; }
static bool graphics_spans_overlap(uint64_t a,uint64_t n,uint64_t b,uint64_t m) { return n && m && a<b+m && b<a+n; }
static int address_space_read(void *s,uint64_t a,int attr,void *buf,size_t n) { if(!dma_ram_range(NULL,a,n))return 1;memcpy(buf,ram+a-ram_base,n);return 0; }
static int address_space_write(void *s,uint64_t a,int attr,const void *buf,size_t n) { if(!dma_ram_range(NULL,a,n))return 1;memcpy(ram+a-ram_base,buf,n);return 0; }
static void stl_le_p(void *p,uint32_t v) { v=GUINT32_TO_LE(v);memcpy(p,&v,4); }
static void stw_le_p(void *p,uint16_t v) { v=GUINT16_TO_LE(v);memcpy(p,&v,2); }
static uint16_t lduw_le_p(const void *p) { uint16_t v;memcpy(&v,p,2);return GUINT16_FROM_LE(v); }
static uint16_t lduw_be_p(const void *p) { uint16_t v;memcpy(&v,p,2);return GUINT16_FROM_BE(v); }
static uint32_t ldl_le_p(const void *p) { uint32_t v;memcpy(&v,p,4);return GUINT32_FROM_LE(v); }
static uint32_t ldl_be_p(const void *p) { uint32_t v;memcpy(&v,p,4);return GUINT32_FROM_BE(v); }
static void qemu_set_irq(qemu_irq irq,int level) { *irq=level; }
static uint64_t qemu_clock_get_ns(int c) { return clock_ns; }
static void timer_mod(QEMUTimer *t,uint64_t n) { t->pending=1; }
static void qemu_mutex_lock(QemuMutex *m) { pthread_mutex_lock(m); }
static void qemu_mutex_unlock(QemuMutex *m) { pthread_mutex_unlock(m); }
static void qemu_thread_create(QemuThread *t,const char *name,void *(*fn)(void *),void *p,int mode) { assert(!pthread_create(t,NULL,fn,p)); }
static void qemu_thread_join(QemuThread *t) { assert(!pthread_join(*t,NULL)); }
#include "nikon-z8-wave5-port.h"
static NikonZ9State s;
static W5pState w;
static QEMUTimer timer;
static int irq;
static unsigned mode,WIDTH=1024,HEIGHT=704;
static void wr(unsigned o,uint32_t v) { uint64_t x=v;assert(w5p_access(&s,W5P_BASE+o,&x,4,true)); }
static uint32_t rd(unsigned o) { uint64_t x=0;assert(w5p_access(&s,W5P_BASE+o,&x,4,false));return x; }
static bool cmd(unsigned c,bool boot) { wr(0x100,c);wr(0x70,1);wr(boot?0x6c:0x38,1);return rd(0x108)!=0; }
static void ack(void) { wr(0x34,rd(0x4c));wr(0x3c,1);assert(!irq && !w.pending && !(s.video_processing_global[3]&1)); }
static void query(void) { wr(0x104,2);assert(cmd(0x4000,false));assert(rd(0x1dc)==1); }
static void wait_job(void) {
    for(int n=0;n<10000 && w.job;n++) { usleep(1000);clock_ns+=1000000;w5p_tick(&w); }
    assert(!w.job && !w.busy && !s.wave5_busy);
}

static void source(unsigned frame) {
 for(unsigned y=0;y<HEIGHT;y++)for(unsigned x=0;x<WIDTH;x++)ram[0x40000+y*WIDTH+x]=(x+y+frame)%220+16;
 memset(ram+0x180000,100+frame,WIDTH*HEIGHT/2);
 wr(0x118,0x40800000);wr(0x11c,1*MiB);wr(0x128,4);
 wr(0x144,frame);wr(0x148,0x40040000);wr(0x14c,0x40180000);wr(0x150,0);
 wr(0x154,(WIDTH<<16)|WIDTH);wr(0x158,2);wr(0x164,0x23);
}
static void boot_config(void) {
 for(unsigned n=16;n<=22;n++)assert(w5p_external_reset(&s,0x0d154248,1u<<n));
 wr(0x110,0x40000000);wr(0x114,4096);wr(0x11c,0x40010000);wr(0x120,4096);ram[0]=0xa5;
 assert(cmd(1,true));assert(w.booted && rd(4)==4);
 wr(0x48,0x8340);wr(0x104,0);assert(cmd(0x4000,false));
 wr(0x110,mode<<16);wr(0x114,0x40020000);wr(0x118,4096);wr(0x124,0);
 assert(cmd(8,false));ack();
 wr(0x104,1);wr(0x11c,1);assert(cmd(0x200,false));ack();
 wr(0x104,0);wr(0x11c,(HEIGHT<<16)|WIDTH);wr(0x124,(8<<14)|(mode==1?(153<<3):0));wr(0x150,30);wr(0x174,1001);wr(0x178,mode==1?30000:60000);wr(0x138,0);wr(0x134,0);wr(0x170,0);wr(0x130,15<<(mode==3?17:16));
 assert(cmd(0x200,false));ack();query();
 wr(0x104,0x18);assert(cmd(0x80,false));assert(w.instance[0].buffers);
}
static void lifecycle(unsigned codec) {
 mode=codec;memset(&s,0,sizeof(s));memset(&w,0,sizeof(w));memset(ram,0,sizeof(ram));
 s.w5p=&w;s.w5p_model=true;s.w5p_host_codec_diagnostic=true;w.board=&s;w.irq=&irq;w.timer=&timer;pthread_mutex_init(&w.lock,NULL);w.external_reset_held=0x7f0000;
 uint32_t first_crc=0;unsigned first_len=0;
 for(unsigned cycle=0;cycle<3;cycle++) {
  assert(!w.instance[0].codec && !w.instance[0].created && !w.instance[0].native_end_written && !w.instance[0].native_drained);
  boot_config();wr(0x118,0x40800000);wr(0x11c,1*MiB);wr(0x128,4);wr(0x164,0x3c);wr(0x144,UINT32_MAX);
  assert(cmd(0x100,false));assert(w.instance[0].codec);ack();query();unsigned bytes=rd(0x14c);assert(bytes>20);
  uint32_t crc=crc32(0,ram+0x800000,bytes);if(!cycle){first_crc=crc;first_len=bytes;}else assert(crc==first_crc && bytes==first_len);
  assert(ldl_le_p(ram+0x20100)>0);assert(w.instance[0].pictures==0);
  source(0);assert(cmd(0x100,false));wait_job();ack();query();assert(rd(0x14c)>0 && w.instance[0].pictures==1);
  source(1);wr(0x164,0xe3);assert(cmd(0x100,false));wait_job();ack();query();assert(w.instance[0].native_end_written);
  source(2);wr(0x144,UINT32_MAX);assert(cmd(0x100,false));ack();query();assert(w.instance[0].native_drained && !rd(0x14c));
  assert(cmd(0x20,false));assert(!w.instances && !w.instance[0].codec && !w.instance[0].created && !w.instance[0].native_end_written && !w.instance[0].native_drained);
  for(unsigned n=16;n<=22;n++)assert(w5p_external_reset(&s,0x0d154348,1u<<n));
  assert(!w.booted && !rd(4) && !w.interrupt_mask && !w.pending);
  printf("PASS codec=%u cycle=%u headers=%u crc=%08x destroy+reset clean\n",codec,cycle,bytes,crc);
 }
 pthread_mutex_destroy(&w.lock);
}
int main(void){lifecycle(1);lifecycle(3);return 0;}
