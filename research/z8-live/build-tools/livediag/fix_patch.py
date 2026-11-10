#!/usr/bin/env python3
"""Live fix v1 on top of diag_patch.py: (1) read the uncached NV12 source with 64-byte ldp bursts
instead of byte loads; (2) learn how many distinct live-view buffers the hardware rotates and,
when it is at least 3, accept a copy during which at most N-2 newer frames completed.
Diag struct gains: bufs[8], nbufs, accepted_late, fast_rows. Usage: fix_patch.py SRC_DIR (after diag_patch)."""
import sys
from pathlib import Path
src = Path(sys.argv[1])
def sub(path, old, new, count=1):
    p = src / path; t = p.read_text()
    assert t.count(old) == count, (path, old[:70], t.count(old))
    p.write_text(t.replace(old, new))

sub('capture/live_stream.h', ' uint64_t snap_y_after,snap_uv_after;\n};',
    ' uint64_t snap_y_after,snap_uv_after;\n uint64_t bufs[8];uint32_t nbufs,version2;uint64_t accepted_late,fast_rows,slow_rows;\n};')

conv_old = ''' unsigned w=s->width,h=s->height;uint8_t *u=dst+(size_t)w*h,*v=u+(size_t)w*h/4;
 for(unsigned y=0;y<h;y++)memcpy(dst+(size_t)y*w,s->y+(size_t)y*s->y_stride,w);
 for(unsigned y=0;y<h/2;y++){const uint8_t *row=s->uv+(size_t)y*s->uv_stride;
  for(unsigned x=0;x<w/2;x++){u[(size_t)y*w/2+x]=row[2*x];v[(size_t)y*w/2+x]=row[2*x+1];}}
 return 1;'''
conv_new = ''' unsigned w=s->width,h=s->height;uint8_t *u=dst+(size_t)w*h,*v=u+(size_t)w*h/4;
 uint8_t tmp[4096] __attribute__((aligned(16)));
 for(unsigned y=0;y<h;y++)burst_copy(dst+(size_t)y*w,s->y+(size_t)y*s->y_stride,w);
 for(unsigned y=0;y<h/2;y++){burst_copy(tmp,s->uv+(size_t)y*s->uv_stride,w);
  for(unsigned x=0;x<w/2;x++){u[(size_t)y*w/2+x]=tmp[2*x];v[(size_t)y*w/2+x]=tmp[2*x+1];}}
 return 1;'''
sub('capture/live_stream.c', conv_old, conv_new)
sub('capture/live_stream.c', 'int live_stream_convert(uint8_t *dst,const struct la_source *s) {',
'''/* Source rows sit in an uncached live-view buffer: one ldp moves 16 bytes, so a 64-byte
 * burst costs 4 bus reads instead of 64 byte loads. Falls back to bytes when unaligned. */
static uint64_t burst_fast,burst_slow;
static void burst_copy(uint8_t *d,const uint8_t *s,size_t n) {
 if((((uintptr_t)d|(uintptr_t)s)&15)==0){
  burst_fast++;
  while(n>=64){
   __asm__ volatile("ldp x9,x10,[%1]\\n ldp x11,x12,[%1,#16]\\n ldp x13,x14,[%1,#32]\\n ldp x15,x16,[%1,#48]\\n"
                    "stp x9,x10,[%0]\\n stp x11,x12,[%0,#16]\\n stp x13,x14,[%0,#32]\\n stp x15,x16,[%0,#48]\\n"
                    ::"r"(d),"r"(s):"x9","x10","x11","x12","x13","x14","x15","x16","memory");
   d+=64;s+=64;n-=64;}
 } else burst_slow++;
 while(n--)*d++=*s++;
}
int live_stream_convert(uint8_t *dst,const struct la_source *s) {''')
# static counters are not allowed in RX payload: replace with no-ops
sub('capture/live_stream.c', 'static uint64_t burst_fast,burst_slow;\n', '')
sub('capture/live_stream.c', '  burst_fast++;\n', '')
sub('capture/live_stream.c', ' } else burst_slow++;\n', ' }\n')

# buffer learning + relaxed acceptance
sub('capture/live_stream.c', '''  D->y_stride=source.y_stride;D->uv_stride=source.uv_stride;''',
'''  D->y_stride=source.y_stride;D->uv_stride=source.uv_stride;D->version2=2;
  {unsigned k;for(k=0;k<D->nbufs && D->bufs[k]!=D->y;k++){}
   if(k==D->nbufs && D->nbufs<8)D->bufs[D->nbufs++]=D->y;}''')
sub('capture/live_stream.c', '''   else if(!c->ops.lease_valid(c->ops.context,&source)){valid=0;D->valid2_fail++;}
   else if(LOAD(&c->irq_seq)!=seq){valid=0;D->seq_changed++;}
   else if(la_cancelled(c)){valid=0;D->cancelled++;}''',
'''   else if(la_cancelled(c)){valid=0;D->cancelled++;}
   else{
    uint64_t now_seq=LOAD(&c->irq_seq);uint64_t passed=now_seq>seq?(now_seq-seq+1)/2:0;
    int same=c->ops.lease_valid(c->ops.context,&source) && now_seq==seq;
    /* With N>=3 rotating buffers, the one just copied is rewritten only after N-1 newer
     * frames; accept when at most N-2 completed during the copy. */
    int late=!same && !(now_seq&1) && D->nbufs>=3 && passed<=D->nbufs-2 &&
             LOAD(&c->cancel_epoch)==source.lease;
    if(same){}
    else if(late){D->accepted_late++;}
    else if(now_seq!=seq){valid=0;D->seq_changed++;}
    else{valid=0;D->valid2_fail++;}
   }''')
sub('capture/live_stream.c', 's->diag.magic=0x3147444cu;s->diag.version=1;', 's->diag.magic=0x3147444cu;s->diag.version=2;')
print('fix patch applied to', src)
