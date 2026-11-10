#!/usr/bin/env python3
"""Live v5: use the cacheable alias (VA + 0x400000000, MAIR 0xff, same PA; checked page by page
with AT S1E1R) instead of the uncached mapping (MAIR 0x44) for CPU work on Live buffers.
Hardware 2026-11-05: live-view buffers 0xa1f5f9800.. and the Wave5/pool blocks 0x8a...... are
mapped 0x44; their +0x400000000 aliases map the same PA with 0xff.
Switches in struct live_tune (reserved[0..3], default 1, USB-writable):
  cached_lv    read live-view frames through the alias after DC IVAC
  cached_work  Live's own pool allocation (i420, AU ring, preview, workspace) through the alias;
               DC CIVAC before it is released
  cached_slot  write the Wave5 input slot through the alias, DC CVAC before TRIGGER
  cached_bs    read the Wave5 bitstream through the alias after DC IVAC
AU/wrap staging moves from the uncached Wave5 block into the encoder scratch (Live's allocation).
Native file writes clean their source first; reads clean+invalidate before and invalidate after.
Usage: cache_patch.py SRC_DIR (after diag/fix1/fix2/fix3/tune)"""
import sys
from pathlib import Path
src = Path(sys.argv[1])
def sub(path, old, new, count=1):
    p = src / path; t = p.read_text()
    assert t.count(old) == count, (path, old[:80], t.count(old))
    p.write_text(t.replace(old, new))

(src / 'capture/live_alias.h').write_text(r'''#ifndef LIVE_ALIAS_H
#define LIVE_ALIAS_H
#include <stdint.h>
#include <stddef.h>
#ifndef __aarch64__
/* host replay build: no alias, cache maintenance is a no-op */
static inline uintptr_t la_alias(uintptr_t va,size_t n){(void)va;(void)n;return 0;}
static inline void la_dc_ivac(uintptr_t p,size_t n){(void)p;(void)n;}
static inline void la_dc_cvac(uintptr_t p,size_t n){(void)p;(void)n;}
static inline void la_dc_civac(uintptr_t p,size_t n){(void)p;(void)n;}
#else
/* PAR_EL1 of an EL1 read translation; interrupts masked and PAR restored (Fenestra pattern). */
static inline uint64_t la_par(uintptr_t va) {
 uint64_t daif,old,par;
 __asm__ volatile("mrs %0,daif\n msr daifset,#15\n mrs %1,par_el1\n at s1e1r,%3\n isb\n mrs %2,par_el1\n msr par_el1,%1\n msr daif,%0"
                  :"=&r"(daif),"=&r"(old),"=&r"(par):"r"(va):"memory");
 return par;
}
/* VA+0x400000000 when every page of [va,va+n) maps the same PA there with MAIR 0xff; else 0. */
static inline uintptr_t la_alias(uintptr_t va,size_t n) {
 if(!n||va<UINT64_C(0x800000000)||va+n>UINT64_C(0xc00000000))return 0;
 uintptr_t d=UINT64_C(0x400000000);
 for(uintptr_t p=va&~(uintptr_t)4095;p<va+n;p+=4096){
  uint64_t a=la_par(p),b=la_par(p+d);
  if((a|b)&1)return 0;
  if(((a^b)&UINT64_C(0x0000fffffffff000))!=0)return 0;
  if((b>>56)!=0xffu)return 0;
 }
 return va+d;
}
static inline unsigned la_line(void){uint64_t ctr;__asm__ volatile("mrs %0,ctr_el0":"=r"(ctr));return 4u<<((ctr>>16)&15);}
#define LA_DC(name,op) static inline void name(uintptr_t p,size_t n){ \
 if(!n)return;unsigned l=la_line();uintptr_t e=p+n;p&=~(uintptr_t)(l-1); \
 for(;p<e;p+=l)__asm__ volatile("dc " op ", %0"::"r"(p):"memory"); \
 __asm__ volatile("dsb sy":::"memory");}
LA_DC(la_dc_ivac,"ivac")
LA_DC(la_dc_cvac,"cvac")
LA_DC(la_dc_civac,"civac")
#endif
#endif
''')

# tune switches in reserved slots
sub('capture/live_stream.h', ' uint32_t ready_fresh_us;   /* READY needs an encoded frame at most this old */\n uint32_t reserved[5];',
    ' uint32_t ready_fresh_us;   /* READY needs an encoded frame at most this old */\n'
    ' uint32_t cached_lv,cached_work,cached_slot,cached_bs; /* cacheable-alias switches, default 1 */\n uint32_t reserved[1];')
sub('capture/live_stream.h', ' uint32_t log_head,log_pad;\n struct live_ev log[LIVE_LOG];\n};',
    ' uint32_t log_head,log_pad;\n struct live_ev log[LIVE_LOG];\n'
    ' uintptr_t cache_base;size_t cache_size;\n uint64_t cached_lv_ok,cached_lv_fail,cached_work_ok,cached_work_fail;\n};')
sub('capture/live_stream.c', '1,3000000u,1,250000u,{0,0,0,0,0}};', '1,3000000u,1,250000u,1,1,1,1,{0}};')
sub('capture/live_stream.c', '#include "live_stream.h"', '#include "live_stream.h"\n#include "live_alias.h"')
# release / allocate
sub('capture/live_stream.c', ' if(c->descriptor)c->ops.release(c->ops.context,c->descriptor);',
    ' if(s->cache_base){la_dc_civac(s->cache_base,s->cache_size);s->cache_base=0;s->cache_size=0;}\n'
    ' if(c->descriptor)c->ops.release(c->ops.context,c->descriptor);')
sub('capture/live_stream.c', ' if(!c->descriptor||!c->pixels||c->actual_bytes<bytes){release(c,s);return 0;}',
    ''' if(!c->descriptor||!c->pixels||c->actual_bytes<bytes){release(c,s);return 0;}
 if(s->tune.cached_work && !(((uintptr_t)c->pixels|c->actual_bytes)&63)){
  uintptr_t a=la_alias((uintptr_t)c->pixels,c->actual_bytes);
  if(a){la_dc_civac(a,c->actual_bytes);c->pixels=(uint8_t *)a;s->cache_base=a;s->cache_size=c->actual_bytes;s->cached_work_ok++;}
  else s->cached_work_fail++;
 }''')
# live-view read
sub('capture/live_stream.c', '   uint64_t t2=clock_us();valid=live_stream_convert(s->i420,&source);uint64_t t3=clock_us();',
    '''   struct la_source src2=source;
   if(s->tune.cached_lv){
    uintptr_t ya=la_alias((uintptr_t)source.y,(size_t)source.y_stride*source.height),
              ua=la_alias((uintptr_t)source.uv,(size_t)source.uv_stride*(source.height/2));
    if(ya&&ua){la_dc_ivac(ya,(size_t)source.y_stride*source.height);la_dc_ivac(ua,(size_t)source.uv_stride*(source.height/2));
     src2.y=(const uint8_t *)ya;src2.uv=(const uint8_t *)ua;s->cached_lv_ok++;}
    else s->cached_lv_fail++;
   }
   uint64_t t2=clock_us();valid=live_stream_convert(s->i420,&src2);uint64_t t3=clock_us();''')
# encoder flags each frame
sub('codec/live_codec.h', 'int live_encoder_sizes(unsigned w,unsigned h,size_t *persist,size_t *scratch);',
    'int live_encoder_sizes(unsigned w,unsigned h,size_t *persist,size_t *scratch);\nvoid live_encoder_set_flags(live_encoder *,unsigned);')
sub('capture/live_stream.c', '  c->copied++;uint64_t begin=clock_us();',
    '  live_encoder_set_flags(&s->encoder,(s->tune.cached_slot?2u:0u)|(s->tune.cached_bs?4u:0u));\n  c->copied++;uint64_t begin=clock_us();')
W = 'codec/live_w5enc.c'
sub(W, '#include "live_h264_idr.h"', '#include "live_h264_idr.h"\n#include "../capture/live_alias.h"')
sub(W, '    uint64_t t_in, t_wait, t_out, t_parse, s_in, s_wait, s_out, s_parse, n_timed;   /* diag, us */\n};',
    '    uint64_t t_in, t_wait, t_out, t_parse, s_in, s_wait, s_out, s_parse, n_timed;   /* diag, us */\n'
    '    uint32_t flags, alias_slot_ok, alias_slot_fail, alias_bs_ok, alias_bs_fail, staging_scratch;\n};')
sub(W, '    *persist = 16; *scratch = 16;   /* everything lives in the permanent Wave5 block */',
    '    *persist = 16; *scratch = 2 * (size_t)W5_AU_CAP + 64;   /* AU/wrap staging in Live\'s (cacheable) allocation */')
sub(W, 'int live_encoder_sizes(unsigned w, unsigned h, size_t *persist, size_t *scratch) {',
    'void live_encoder_set_flags(live_encoder *e, unsigned f) { if (e && e->hw && e->hw->magic == W5_MAGIC) e->hw->flags = f; }\n'
    'int live_encoder_sizes(unsigned w, unsigned h, size_t *persist, size_t *scratch) {')
# staging buffers
sub(W, '''    uint64_t t_in0 = now_us();''', '''    uint8_t *stage_au = w->au, *stage_wrap = w->wrap;
    if (e->scratch) { stage_au = (uint8_t *)(((uintptr_t)e->scratch + 63) & ~(uintptr_t)63); stage_wrap = stage_au + W5_AU_CAP; w->staging_scratch = 1; }
    uint64_t t_in0 = now_us();''')
sub(W, '''    uint8_t *y = (uint8_t *)(uintptr_t)w->slot[slot], *c = y + (size_t)width * ((height + 31) & ~31u);''',
'''    size_t slot_span = (size_t)width * ((height + 31) & ~31u) + (size_t)width * height / 2;
    uint8_t *y = (uint8_t *)(uintptr_t)w->slot[slot]; int slot_cached = 0;
    if (w->flags & 2u) {
        uintptr_t a = la_alias((uintptr_t)y, slot_span);
        if (a) { y = (uint8_t *)a; slot_cached = 1; w->alias_slot_ok++; } else w->alias_slot_fail++;
    }
    uint8_t *c = y + (size_t)width * ((height + 31) & ~31u);''')
sub(W, '''    uint64_t t_in_end = now_us(); w->t_in = t_in_end - t_in0; w->s_in += w->t_in;''',
'''    if (slot_cached) la_dc_cvac((uintptr_t)y, slot_span);   /* the VPU reads PA memory */
    uint64_t t_in_end = now_us(); w->t_in = t_in_end - t_in0; w->s_in += w->t_in;''')
sub(W, '''                w5_burst(w->wrap, (const uint8_t *)(uintptr_t)a1, n1);
                w5_burst(w->wrap + n1, (const uint8_t *)(uintptr_t)a2, n2);
                src = w->wrap;''', '''                uintptr_t c1 = (w->flags & 4u) ? la_alias(a1, n1) : 0, c2 = (w->flags & 4u) ? la_alias(a2, n2) : 0;
                if (c1 && c2) { la_dc_ivac(c1, n1); la_dc_ivac(c2, n2); a1 = c1; a2 = c2; w->alias_bs_ok++; }
                else if (w->flags & 4u) w->alias_bs_fail++;
                w5_burst(stage_wrap, (const uint8_t *)(uintptr_t)a1, n1);
                w5_burst(stage_wrap + n1, (const uint8_t *)(uintptr_t)a2, n2);
                src = stage_wrap;''')
sub(W, '''                uint8_t *dst = w->wrap + (addr & 15);
                w5_burst(dst, src, size);''', '''                uintptr_t ca = (w->flags & 4u) ? la_alias(addr, size) : 0;
                if (ca) { la_dc_ivac(ca, size); src = (const uint8_t *)ca; w->alias_bs_ok++; }
                else if (w->flags & 4u) w->alias_bs_fail++;
                uint8_t *dst = stage_wrap + (addr & 15);
                w5_burst(dst, src, size);''')
sub(W, '            if (live_h264_idr_au(&w->idr, src, size, w->au, W5_AU_CAP, &n, idr)) return -23;',
       '            if (live_h264_idr_au(&w->idr, src, size, stage_au, W5_AU_CAP, &n, idr)) return -23;')
sub(W, '            *out = w->au; *bytes = n;', '            *out = stage_au; *bytes = n;')
# native file I/O cache maintenance
F = 'codec/native_files.c'
sub(F, '#define OPEN ', '#include "../capture/live_alias.h"\n#define OPEN ')
sub(F, ' uint32_t r=READ(fd,p,(uint32_t)n,&got);*actual=got;return result(c,r);',
       ' la_dc_civac((uintptr_t)p,n);uint32_t r=READ(fd,p,(uint32_t)n,&got);la_dc_ivac((uintptr_t)p,n);*actual=got;return result(c,r);')
sub(F, ' uint32_t r=WRITE(fd,p,(uint32_t)n,&got,0);*actual=got;return result(c,r);',
       ' la_dc_cvac((uintptr_t)p,n);uint32_t r=WRITE(fd,p,(uint32_t)n,&got,0);*actual=got;return result(c,r);')
sub('capture/live_stream.c', 's->diag.version=4;', 's->diag.version=5;')
print('cache patch applied to', src)
