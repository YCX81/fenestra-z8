#!/usr/bin/env python3
"""v9: Live flight recorder (per-frame record ring, readable over USB) + on-camera tear check.
Every new live-view generation the worker sees gets one 64-byte record in a 4096-entry ring
(~2.3 min at 30 fps) allocated once from the native pool and never released while powered:
  t, seq, irq_seq after the copy, picked Y address, snapshot words s4/s20, convert/encode us,
  result (kept / why dropped), picker mode, buffer count, and torn_y/torn_uv masks.
Tear check (stream.fr_rows sample rows, default 16, 0 = off, max 16): right after the copy the
same rows of the source buffer are read again and compared with the copy. A differing row means
the hardware rewrote that buffer within ~ms of the copy, i.e. the copied frame is torn there.
Usage: flight_patch.py SRC_DIR (after picker_patch/test_patch, i.e. on a copy of src-fix8)"""
import sys
from pathlib import Path
src = Path(sys.argv[1])


def sub(path, old, new, count=1):
    p = src / path; t = p.read_text()
    assert t.count(old) == count, (path, old[:80], t.count(old))
    p.write_text(t.replace(old, new))


H = 'capture/live_stream.h'
sub(H, 'struct live_ev {uint64_t t;uint32_t type,a;uint64_t b;};',
    'struct live_ev {uint64_t t;uint32_t type,a;uint64_t b;};\n'
    '/* v9 flight record, 64 bytes; result codes in flight_patch.py / livectl.py */\n'
    'struct live_fr {uint64_t t,seq,after,y,s4,s20;uint32_t conv_us,enc_us;uint16_t torn_y,torn_uv;uint8_t result,picker,nbufs,flags;};\n'
    '#define LIVE_FR 4096u')
sub(H, ' uint64_t last_y,last_gen;struct {uint64_t from,to;} trans[8];uint64_t picked_prev,picked_fallback;\n};',
    ' uint64_t last_y,last_gen;struct {uint64_t from,to;} trans[8];uint64_t picked_prev,picked_fallback;\n'
    ' uint32_t fr_magic,fr_rows;uint32_t fr_head,fr_alloc_fail;struct live_fr *fr;void *fr_desc;uint64_t fr_torn,fr_checked;\n};')

C = 'capture/live_stream.c'
sub(C, 'void live_stream_init(struct live_stream *s) {\n memset(s,0,sizeof(*s));',
    'void live_stream_init(struct live_stream *s) {\n memset(s,0,sizeof(*s));s->fr_magic=0x31524646u;s->fr_rows=16;')
# Allocate the ring once (pool may be gated during a still: retried on every allocate()).
sub(C, 'static int allocate(struct la_capture *c,struct live_stream *s,unsigned w,unsigned h) {\n size_t p,t;',
    'static int allocate(struct la_capture *c,struct live_stream *s,unsigned w,unsigned h) {\n'
    ' if(!s->fr && !s->fr_desc){void *d=0;uint8_t *px=0;uint32_t got=0;\n'
    '  if(c->ops.allocate(c->ops.context,LIVE_FR*(uint32_t)sizeof(struct live_fr)+64u,&d,&px,&got) && d && px &&\n'
    '     got>=LIVE_FR*sizeof(struct live_fr)+64u){s->fr_desc=d;s->fr=(struct live_fr *)(((uintptr_t)px+63)&~(uintptr_t)63);\n'
    '   memset(s->fr,0,LIVE_FR*sizeof(struct live_fr));s->fr_head=0;}\n'
    '  else{s->fr_alloc_fail++;if(d)c->ops.release(c->ops.context,d);}}\n'
    ' size_t p,t;')
sub(C, 'void live_stream_step(struct la_capture *c,struct live_stream *s,uint64_t now,int eligible,uint64_t (*clock_us)(void)) {\n',
    '''static int fr_row_diff(const uint8_t *a,const uint8_t *b,unsigned n){
 if(!(((uintptr_t)a|(uintptr_t)b)&7)){const uint64_t *x=(const uint64_t *)a,*y=(const uint64_t *)b;
  for(unsigned i=0;i<n/8;i++)if(x[i]!=y[i])return 1;return 0;}
 for(unsigned i=0;i<n;i++)if(a[i]!=b[i])return 1;return 0;}
/* Re-read sample rows of the source right after the copy; compare with what was copied. */
static void fr_sample(struct live_stream *s,const struct la_source *src,const struct la_source *rd,struct live_fr *f){
 unsigned R=s->fr_rows>16?16:s->fr_rows,w=src->width,h=src->height;
 const uint8_t *u=s->i420+(size_t)w*h,*v=u+(size_t)w*h/4;
 for(unsigned k=0;k<R;k++){unsigned yy=(unsigned)(((uint64_t)k*2+1)*h/(2*R));
  const uint8_t *sr=rd->y+(size_t)yy*src->y_stride;
  if(rd->y!=src->y)la_dc_ivac((uintptr_t)sr,w);
  if(fr_row_diff(sr,s->i420+(size_t)yy*w,w))f->torn_y|=(uint16_t)(1u<<k);
  unsigned uy=yy/2;const uint8_t *su=rd->uv+(size_t)uy*src->uv_stride;
  if(rd->uv!=src->uv)la_dc_ivac((uintptr_t)su,w);
  for(unsigned x=0;x<w/2;x++)if(su[2*x]!=u[(size_t)uy*w/2+x]||su[2*x+1]!=v[(size_t)uy*w/2+x]){f->torn_uv|=(uint16_t)(1u<<k);break;}}
 s->fr_checked++;if(f->torn_y|f->torn_uv)s->fr_torn++;}
static void fr_push(struct live_stream *s,struct live_fr *f,const struct live_diag *d0){
 const struct live_diag *D=&s->diag;uint8_t r=0;
 if(D->busy>d0->busy)r=13;else if(D->ok>d0->ok)r=D->accepted_late>d0->accepted_late?2:1;
 else if(D->stale>d0->stale)r=3;else if(D->interval>d0->interval)r=4;else if(D->lease_fail>d0->lease_fail)r=5;
 else if(D->alloc_retry>d0->alloc_retry)r=6;else if(D->geometry>d0->geometry)r=7;else if(D->valid1_fail>d0->valid1_fail)r=8;
 else if(D->convert_fail>d0->convert_fail)r=9;else if(D->cancelled>d0->cancelled)r=10;else if(D->seq_changed>d0->seq_changed)r=11;
 else if(D->valid2_fail>d0->valid2_fail)r=12;
 f->result=r;f->nbufs=(uint8_t)D->nbufs;
 if(!s->fr)return;uint32_t i=s->fr_head%LIVE_FR;s->fr[i]=*f;__atomic_store_n(&s->fr_head,s->fr_head+1,__ATOMIC_RELEASE);}
void live_stream_step(struct la_capture *c,struct live_stream *s,uint64_t now,int eligible,uint64_t (*clock_us)(void)) {
 struct live_fr fr;int fr_on=0;struct live_diag d0;
''')
sub(C, '  struct live_diag *D=&s->diag;D->new_seq++;',
    '  struct live_diag *D=&s->diag;D->new_seq++;\n'
    '  fr_on=1;d0=*D;memset(&fr,0,sizeof fr);fr.t=now;fr.seq=seq;fr.picker=(uint8_t)s->tune.picker;')
sub(C, '  D->y_stride=source.y_stride;D->uv_stride=source.uv_stride;D->version2=2;',
    '  D->y_stride=source.y_stride;D->uv_stride=source.uv_stride;D->version2=2;\n'
    '  fr.y=D->y;{uint32_t sh=s->snap_head;if(sh){fr.s4=s->snap[(sh-1)%16u].s4;fr.s20=s->snap[(sh-1)%16u].s20;}}')
sub(C, '   uint64_t after=LOAD(&c->irq_seq);D->last_frames_during_copy=after>seq?(after-seq)/2:0;',
    '   uint64_t after=LOAD(&c->irq_seq);D->last_frames_during_copy=after>seq?(after-seq)/2:0;\n'
    '   fr.after=after;fr.conv_us=(uint32_t)(t3-t2);if(valid&&s->fr_rows&&s->fr)fr_sample(s,&source,&src2,&fr);')
sub(C, '  uint64_t elapsed=clock_us()-begin;s->encode_last_us=elapsed;',
    '  uint64_t elapsed=clock_us()-begin;s->encode_last_us=elapsed;fr.enc_us=(uint32_t)elapsed;')
sub(C, ' finish:\n', ' finish:\n if(fr_on)fr_push(s,&fr,&d0);\n')
print('flight patch applied to', src)
