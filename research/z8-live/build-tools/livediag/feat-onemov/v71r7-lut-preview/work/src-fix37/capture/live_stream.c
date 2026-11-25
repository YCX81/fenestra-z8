#include "live_stream.h"
#include "live_alias.h"
#include "live_scale_neon.h"
#include "../codec/preview_writer.h"
#ifdef LIVE_HOST_TEST
#define FILMSIM_LIVE_MODE_OFF        0
#define FILMSIM_LIVE_MODE_GRAIN_ONLY 1
#define FILMSIM_LIVE_MODE_FULL_CPU2  2
#ifdef LIVE_HOST_FILMSIM_MOCK
extern int filmsim_runtime_live_mode(void);
extern int filmsim_runtime_live_active(void);
extern uint32_t filmsim_runtime_live_is_busy(void);
extern int filmsim_runtime_live_begin_submit_nv12(unsigned w, unsigned h,
                                                  uint8_t **y_out, uint8_t **uv_out,
                                                  unsigned *y_stride_out, unsigned *uv_stride_out);
extern void filmsim_runtime_live_commit_submit_nv12(int slot, unsigned w, unsigned h,
                                                    uint64_t source_us, int commit);
extern int filmsim_runtime_live_acquire_nv12(const uint8_t **y_out, const uint8_t **uv_out,
                                             unsigned *w_out, unsigned *h_out,
                                             uint64_t *gen_out, const int8_t **grain_tile_out);
extern const int8_t *filmsim_runtime_live_acquire_grain_tile(void);
extern void filmsim_runtime_live_release(void);
extern int filmsim_runtime_live_layer0_feed_active(void);
#else
static inline int filmsim_runtime_live_mode(void) { return 0; }
static inline uint32_t filmsim_runtime_live_is_busy(void) { return 0u; }
static inline int filmsim_runtime_live_begin_submit_nv12(unsigned w, unsigned h,
                                                         uint8_t **y_out, uint8_t **uv_out,
                                                         unsigned *y_stride_out, unsigned *uv_stride_out) {
 (void)w;(void)h;(void)y_out;(void)uv_out;(void)y_stride_out;(void)uv_stride_out; return -1;
}
static inline void filmsim_runtime_live_commit_submit_nv12(int slot, unsigned w, unsigned h,
                                                           uint64_t source_us, int commit) {
 (void)slot;(void)w;(void)h;(void)source_us;(void)commit;
}
static inline int filmsim_runtime_live_acquire_nv12(const uint8_t **y_out, const uint8_t **uv_out,
                                                    unsigned *w_out, unsigned *h_out,
                                                    uint64_t *gen_out, const int8_t **grain_tile_out) {
 (void)y_out;(void)uv_out;(void)w_out;(void)h_out;(void)gen_out;(void)grain_tile_out; return 0;
}
static inline const int8_t *filmsim_runtime_live_acquire_grain_tile(void) { return 0; }
static inline void filmsim_runtime_live_release(void) {}
static inline int filmsim_runtime_live_layer0_feed_active(void) { return 0; }
#endif
#else
#include "filmsim_runtime.h"
#endif
#include <string.h>

/* V71: Batched dc ivac without per-row dsb sy (eliminates 31 barriers/frame in fr_post_check). */
#if !defined(__aarch64__) || defined(__APPLE__) || defined(__linux__) || defined(LIVE_HOST_TEST)
static inline void fr_dc_ivac_nb(uintptr_t p, size_t n) { (void)p; (void)n; }
static inline void fr_dsb_sy(void) {}
#else
static inline void fr_dc_ivac_nb(uintptr_t p, size_t n) {
 if (!n) return;
 unsigned l = la_line();
 uintptr_t e = p + n;
 p &= ~(uintptr_t)(l - 1u);
 for (; p < e; p += l) __asm__ volatile("dc ivac, %0" :: "r"(p) : "memory");
}
static inline void fr_dsb_sy(void) { __asm__ volatile("dsb sy" ::: "memory"); }
#endif

/* Frozen V70/V71 Wave5 Direct-NV12 & 1-Deep Async API prototypes (live_codec.h).
 * Safe to repeat identically if live_codec.h already declares them. */
int live_encoder_nv12_begin(live_encoder *e, unsigned *slot_out,
                            uint8_t **y_out, uint8_t **uv_out,
                            unsigned *y_stride_out, unsigned *uv_stride_out,
                            unsigned *coded_h_out, int *cached_out);
int live_encoder_submit_slot(live_encoder *e, unsigned slot, int already_cleaned,
                             uint64_t pts, uint64_t *last_idr,
                             unsigned bitrate, unsigned interval);
int live_encoder_poll_au(live_encoder *e, int block, uint64_t *pts_out,
                         const uint8_t **out, size_t *bytes, unsigned *idr);
int live_encoder_flush(live_encoder *e, uint64_t *pts_out,
                       const uint8_t **out, size_t *bytes, unsigned *idr);
int live_encoder_last_valid_slot(const live_encoder *e);
int live_encoder_slot_ptrs(const live_encoder *e, unsigned slot,
                           const uint8_t **y_out, const uint8_t **uv_out,
                           unsigned *y_stride_out, unsigned *uv_stride_out);
#if !defined(LIVE_HOST_TEST) || defined(LIVE_HOST_W5_PARK_MOCK)
int live_encoder_is_parked(const live_encoder *e);
#else
static inline int live_encoder_is_parked(const live_encoder *e) { (void)e; return 0; }
#endif

#define LOAD(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#define STORE(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
/* Put cold scalar fallbacks & V70 cadence helpers in .filmsim (78.6 KiB free)
 * so live_primary stays well below PortraitPB 0x401FCD00.
 * Keep live_stream_step() (calling filmsim_runtime_live_*) in .text! */
#ifndef LIVE_HOST_TEST
#define LIVE_V70_SEC __attribute__((section(".filmsim"),noinline))
#else
#define LIVE_V70_SEC
#endif
/* Keep 8 preview slots at 2.5 fps (~8.4 MiB) for the center NCDT thumbnail;
 * LIVE_PREVIEW_SLOTS (50) stays fixed in live_stream.h to preserve Wave5 ABI. */
#define LIVE_PREVIEW_RING_SLOTS 8u
#define LIVE_PREVIEW_RING_STEP_US 400000u
#define LV_ALIAS_SLOTS 8u
struct lv_alias_slot { uintptr_t y, uv; size_t yn, un; uintptr_t ya, ua; };
typedef uint64_t __attribute__((may_alias)) la_u64a;
typedef uint32_t __attribute__((may_alias)) la_u32a;

#ifdef LIVE_HOST_TEST
static struct live_flip_obs host_flip_obs_default;
static struct live_flip_obs *host_flip_obs_ptr = 0;
static live_host_observe_fn host_observer_cb = 0;
void live_host_bind_flip_obs(struct live_flip_obs *obs) { host_flip_obs_ptr = obs; }
struct live_flip_obs *live_host_get_flip_obs(void) {
 return host_flip_obs_ptr ? host_flip_obs_ptr : &host_flip_obs_default;
}
void live_host_set_observer(live_host_observe_fn fn) { host_observer_cb = fn; }
#endif

static struct live_flip_obs *stream_flip_obs(struct live_stream *s) {
#ifdef LIVE_HOST_TEST
 (void)s;
 return live_host_get_flip_obs();
#else
 return la_stream_flip_obs(s);
#endif
}

static uintptr_t align16(uintptr_t a){return (a+15)&~(uintptr_t)15;}

void live_stream_init(struct live_stream *s) {
 memset(s,0,sizeof(*s));s->fr_magic=0x31524646u;s->fr_rows=16;s->fr_drop=1;s->dcim_copy=0 /* v37: one MOV per shot */;s->bitrate=16000000u;s->idr_interval_us=LIVE_IDR_US;
 /* V70: tick-driven argmin |flip_us - tick| sampler removes sample_us=28000u gate. */
 s->tune=(struct live_tune){0x314e544cu,1,-1,1,LA_WINDOW_US,0u,LA_MAX_GAP_US,1,3000000u,1,1000000u,1,1,1,1,3,400u /* v37 */};
#ifdef LIVE_HOST_TEST
 memset(live_host_get_flip_obs(),0,sizeof(struct live_flip_obs));
#endif
}

/* Source rows sit in an uncached live-view buffer: one ldp moves 16 bytes, so a 64-byte
 * burst costs 4 bus reads instead of 64 byte loads. Falls back to bytes when unaligned. */
static LIVE_V70_SEC void burst_copy(uint8_t *d,const uint8_t *s,size_t n) {
#ifdef __aarch64__
 if((((uintptr_t)d|(uintptr_t)s)&15)==0){
  while(n>=64){
   __asm__ volatile("ldp x9,x10,[%1]\n ldp x11,x12,[%1,#16]\n ldp x13,x14,[%1,#32]\n ldp x15,x16,[%1,#48]\n"
                    "stp x9,x10,[%0]\n stp x11,x12,[%0,#16]\n stp x13,x14,[%0,#32]\n stp x15,x16,[%0,#48]\n"
                    ::"r"(d),"r"(s):"x9","x10","x11","x12","x13","x14","x15","x16","memory");
   d+=64;s+=64;n-=64;}
 }
#endif
 while(n--)*d++=*s++;
}

static LIVE_V70_SEC int convert_ex(uint8_t *dst,const struct la_source *s,int cached) {
 if(!dst||!s||!s->y||!s->uv||!s->width||!s->height||((s->width|s->height)&1)||
 s->width>4096||s->height>4096||s->y_stride<s->width||s->uv_stride<s->width||
 s->y_stride>8192||s->uv_stride>8192)return 0;
 unsigned w=s->width,h=s->height;uint8_t *u=dst+(size_t)w*h,*v=u+(size_t)w*h/4;
 uint8_t tmp[4096] __attribute__((aligned(16)));
 for(unsigned y=0;y<h;y++)burst_copy(dst+(size_t)y*w,s->y+(size_t)y*s->y_stride,w);
 unsigned cw=w/2u,ch=h/2u;
 int fast_uv=0;
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
 fast_uv=(w&7u)==0u&&(((uintptr_t)u|(uintptr_t)v)&3u)==0u;
#endif
 int direct_uv=cached&&(((uintptr_t)s->uv|s->uv_stride)&7u)==0u;
 for(unsigned y=0;y<ch;y++){
  const uint8_t *sr=s->uv+(size_t)y*s->uv_stride;
  const uint8_t *row=direct_uv?sr:(burst_copy(tmp,sr,w),tmp);
  uint8_t *du=u+(size_t)y*cw,*dv=v+(size_t)y*cw;
  unsigned x=0;
  if(fast_uv){
   const la_u64a *q=(const la_u64a *)(const void *)row;
   la_u32a *u32=(la_u32a *)du,*v32=(la_u32a *)dv;
   unsigned n4=cw>>2;
   for(unsigned i=0;i<n4;i++){
    uint64_t p=q[i];
    uint64_t up=p&UINT64_C(0x00ff00ff00ff00ff);
    up=(up|(up>>8))&UINT64_C(0x0000ffff0000ffff);
    u32[i]=(uint32_t)(up|(up>>16));
    uint64_t vp=(p>>8)&UINT64_C(0x00ff00ff00ff00ff);
    vp=(vp|(vp>>8))&UINT64_C(0x0000ffff0000ffff);
    v32[i]=(uint32_t)(vp|(vp>>16));
   }
   x=n4<<2;
  }
  for(;x<cw;x++){du[x]=row[2u*x];dv[x]=row[2u*x+1u];}
 }
 return 1;
}
int live_stream_convert(uint8_t *dst,const struct la_source *s) {return convert_ex(dst,s,0);}

/* v63+: target 1280x852 for 3:2 Live View (1024x682, 1024x680, 1280x852); fit other
 * aspect ratios proportionally inside 1280x852 so LCD<->EVF switches never reset. */
static void live_fit(unsigned w,unsigned h,unsigned *fw,unsigned *fh){
 if(!w||!h){*fw=0;*fh=0;return;}
 uint64_t lhs=(uint64_t)h*3u,rhs=(uint64_t)w*2u;
 uint64_t diff=lhs>rhs?lhs-rhs:rhs-lhs;
 if(diff<=12u){*fw=1280u;*fh=852u;return;}
 unsigned tw=1280u,th=(unsigned)(((uint64_t)h*1280u/w+1u)&~1u);
 if(th>852u){th=852u;tw=(unsigned)(((uint64_t)w*852u/h+1u)&~1u);}
 if(tw<2u)tw=2u;
 if(th<2u)th=2u;
 *fw=tw;*fh=th;
}

static unsigned cache_slot(int32_t idx[2],unsigned row,int32_t keep,int *miss){
 if(idx[0]==(int32_t)row){*miss=0;return 0u;}
 if(idx[1]==(int32_t)row){*miss=0;return 1u;}
 unsigned slot=(keep>=0&&idx[0]==keep)?1u:0u;
 idx[slot]=(int32_t)row;*miss=1;return slot;
}

static LIVE_V70_SEC void hscale_luma(uint8_t *dst,const uint8_t *src,unsigned sw,unsigned dw,uint8_t *sbuf,int cached){
 const uint8_t *p=src;
 if(!cached||((uintptr_t)src&3u)!=0u){burst_copy(sbuf,src,sw);p=sbuf;}
 if(sw==1024u&&dw==1280u){
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  const la_u32a *wp=(const la_u32a *)(const void *)p;
  uint32_t cur=wp[0];
  for(unsigned g=0;g<256u;g++){
   uint32_t nxt=g<255u?wp[g+1u]:0u;
   unsigned a0=cur&255u,a1=(cur>>8)&255u,a2=(cur>>16)&255u,a3=cur>>24;
   unsigned a4=g<255u?(nxt&255u):a3;
   uint8_t *o=dst+5u*g;
   o[0]=(uint8_t)a0;
   o[1]=(uint8_t)((3u*a0+13u*a1+8u)>>4);
   o[2]=(uint8_t)((3u*a1+5u*a2+4u)>>3);
   o[3]=(uint8_t)((5u*a2+3u*a3+4u)>>3);
   o[4]=(uint8_t)((13u*a3+3u*a4+8u)>>4);
   cur=nxt;
  }
#else
  for(unsigned g=0;g<256u;g++){
   unsigned a0=p[4u*g],a1=p[4u*g+1u],a2=p[4u*g+2u],a3=p[4u*g+3u];
   unsigned a4=g<255u?p[4u*g+4u]:a3;
   dst[5u*g+0u]=(uint8_t)a0;
   dst[5u*g+1u]=(uint8_t)((3u*a0+13u*a1+8u)>>4);
   dst[5u*g+2u]=(uint8_t)((3u*a1+5u*a2+4u)>>3);
   dst[5u*g+3u]=(uint8_t)((5u*a2+3u*a3+4u)>>3);
   dst[5u*g+4u]=(uint8_t)((13u*a3+3u*a4+8u)>>4);
  }
#endif
  return;
 }
 unsigned den=dw>1u?dw-1u:1u;
 uint32_t step=(uint32_t)((((uint64_t)(sw-1u)<<16)+(den>>1))/den),fx=0,max_fx=(sw-1u)<<16;
 for(unsigned x=0;x<dw;x++){
  uint32_t cf=fx>max_fx?max_fx:fx;
  unsigned ix=cf>>16,wx=(cf>>8)&255u,ix1=ix+1u<sw?ix+1u:ix;
  dst[x]=(uint8_t)(((256u-wx)*p[ix]+wx*p[ix1]+128u)>>8);
  fx+=step;
 }
}

static LIVE_V70_SEC void hscale_uv(uint8_t *ud,uint8_t *vd,const uint8_t *src_uv,unsigned sw_c,unsigned dw_c,uint8_t *sbuf,int cached){
 const uint8_t *p=src_uv;
 if(!cached||((uintptr_t)src_uv&7u)!=0u){burst_copy(sbuf,src_uv,(size_t)sw_c*2u);p=sbuf;}
 if(sw_c==512u&&dw_c==640u){
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  const la_u64a *qp=(const la_u64a *)(const void *)p;
  uint64_t cur=qp[0];
  for(unsigned g=0;g<128u;g++){
   uint64_t nxt=g<127u?qp[g+1u]:0u;
   unsigned u0=(unsigned)(cur&255u),v0=(unsigned)((cur>>8)&255u);
   unsigned u1=(unsigned)((cur>>16)&255u),v1=(unsigned)((cur>>24)&255u);
   unsigned u2=(unsigned)((cur>>32)&255u),v2=(unsigned)((cur>>40)&255u);
   unsigned u3=(unsigned)((cur>>48)&255u),v3=(unsigned)(cur>>56);
   unsigned u4=g<127u?(unsigned)(nxt&255u):u3,v4=g<127u?(unsigned)((nxt>>8)&255u):v3;
   uint8_t *uo=ud+5u*g,*vo=vd+5u*g;
   uo[0]=(uint8_t)u0;uo[1]=(uint8_t)((3u*u0+13u*u1+8u)>>4);uo[2]=(uint8_t)((3u*u1+5u*u2+4u)>>3);
   uo[3]=(uint8_t)((5u*u2+3u*u3+4u)>>3);uo[4]=(uint8_t)((13u*u3+3u*u4+8u)>>4);
   vo[0]=(uint8_t)v0;vo[1]=(uint8_t)((3u*v0+13u*v1+8u)>>4);vo[2]=(uint8_t)((3u*v1+5u*v2+4u)>>3);
   vo[3]=(uint8_t)((5u*v2+3u*v3+4u)>>3);vo[4]=(uint8_t)((13u*v3+3u*v4+8u)>>4);
   cur=nxt;
  }
#else
  for(unsigned g=0;g<128u;g++){
   unsigned b=8u*g;
   unsigned u0=p[b],v0=p[b+1u],u1=p[b+2u],v1=p[b+3u];
   unsigned u2=p[b+4u],v2=p[b+5u],u3=p[b+6u],v3=p[b+7u];
   unsigned u4=g<127u?p[b+8u]:u3,v4=g<127u?p[b+9u]:v3;
   ud[5u*g+0u]=(uint8_t)u0;
   ud[5u*g+1u]=(uint8_t)((3u*u0+13u*u1+8u)>>4);
   ud[5u*g+2u]=(uint8_t)((3u*u1+5u*u2+4u)>>3);
   ud[5u*g+3u]=(uint8_t)((5u*u2+3u*u3+4u)>>3);
   ud[5u*g+4u]=(uint8_t)((13u*u3+3u*u4+8u)>>4);
   vd[5u*g+0u]=(uint8_t)v0;
   vd[5u*g+1u]=(uint8_t)((3u*v0+13u*v1+8u)>>4);
   vd[5u*g+2u]=(uint8_t)((3u*v1+5u*v2+4u)>>3);
   vd[5u*g+3u]=(uint8_t)((5u*v2+3u*v3+4u)>>3);
   vd[5u*g+4u]=(uint8_t)((13u*v3+3u*v4+8u)>>4);
  }
#endif
  return;
 }
 unsigned den=dw_c>1u?dw_c-1u:1u;
 uint32_t step=(uint32_t)((((uint64_t)(sw_c-1u)<<16)+(den>>1))/den),fx=0,max_fx=(sw_c-1u)<<16;
 for(unsigned x=0;x<dw_c;x++){
  uint32_t cf=fx>max_fx?max_fx:fx;
  unsigned ix=cf>>16,wx=(cf>>8)&255u,w0=256u-wx,ix1=ix+1u<sw_c?ix+1u:ix;
  ud[x]=(uint8_t)((w0*p[2u*ix]+wx*p[2u*ix1]+128u)>>8);
  vd[x]=(uint8_t)((w0*p[2u*ix+1u]+wx*p[2u*ix1+1u]+128u)>>8);
  fx+=step;
 }
}

static LIVE_V70_SEC void vblend_row(uint8_t *dst,const uint8_t *r0,const uint8_t *r1,unsigned n,unsigned wy){
 if(wy==0u){burst_copy(dst,r0,n);return;}
 unsigned w0=16u-wy,x=0;
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
 if((((uintptr_t)dst|(uintptr_t)r0|(uintptr_t)r1)&7u)==0u){
  const la_u64a *p0=(const la_u64a *)r0,*p1=(const la_u64a *)r1;
  la_u64a *pd=(la_u64a *)dst;
  unsigned words=n>>3;
  if(wy==8u){
   uint64_t fe=UINT64_C(0xfefefefefefefefe);
   for(unsigned i=0;i<words;i++){
    uint64_t a=p0[i],b=p1[i];
    pd[i]=(a|b)-(((a^b)&fe)>>1);
   }
  }else{
   uint64_t m=UINT64_C(0x00ff00ff00ff00ff),rnd=UINT64_C(0x0008000800080008);
   for(unsigned i=0;i<words;i++){
    uint64_t a=p0[i],b=p1[i];
    uint64_t lo=((a&m)*w0+(b&m)*wy+rnd)>>4;
    uint64_t hi=(((a>>8)&m)*w0+((b>>8)&m)*wy+rnd)>>4;
    pd[i]=(lo&m)|((hi&m)<<8);
   }
  }
  x=words<<3;
 }
#endif
 for(;x<n;x++)dst[x]=(uint8_t)((w0*r0[x]+wy*r1[x]+8u)>>4);
}

static LIVE_V70_SEC int convert_scaled_ex(uint8_t *dst,unsigned dw,unsigned dh,const struct la_source *s,int cached) {
 if(!dst||!s||!s->y||!s->uv||!s->width||!s->height||((s->width|s->height|dw|dh)&1)||!dw||!dh||dw>4096||dh>4096||
 s->width>4096||s->height>4096||s->y_stride<s->width||s->uv_stride<s->width||
 s->y_stride>8192||s->uv_stride>8192)return 0;
#ifndef LIVE_HOST_TEST
 if(dw>2048u)return 0;
#endif
 unsigned w=s->width,h=s->height,cw=dw/2u,ch=dh/2u,sw_c=w/2u,sh_c=h/2u;
 uint8_t *u=dst+(size_t)dw*dh,*v=u+(size_t)dw*dh/4u;
 uint64_t hbuf[2][256] __attribute__((aligned(16)));
 uint8_t sbuf[4096] __attribute__((aligned(16)));
 if(dw<=2048u){
  uint8_t *lh[2]={(uint8_t *)hbuf[0],(uint8_t *)hbuf[1]};
  int32_t row_idx[2]={-1,-1};
  uint32_t y_num=(h-1u)<<4,y_den=dh>1u?dh-1u:1u;
  uint32_t y_qs=y_num/y_den,y_rs=y_num%y_den,fy=0u,y_rem=0u;
  for(unsigned y=0;y<dh;y++){
   unsigned iy=fy>>4,wy=fy&15u,iy1=iy+1u<h?iy+1u:iy;
   fy+=y_qs;y_rem+=y_rs;if(y_rem>=y_den){y_rem-=y_den;fy++;}
   int miss=0;
   unsigned s0=cache_slot(row_idx,iy,wy?(int32_t)iy1:-1,&miss);
   if(miss)hscale_luma(lh[s0],s->y+(size_t)iy*s->y_stride,w,dw,sbuf,cached);
   if(wy==0u){vblend_row(dst+(size_t)y*dw,lh[s0],lh[s0],dw,0u);continue;}
   unsigned s1=s0;
   if(iy1!=iy){
    s1=cache_slot(row_idx,iy1,(int32_t)iy,&miss);
    if(miss)hscale_luma(lh[s1],s->y+(size_t)iy1*s->y_stride,w,dw,sbuf,cached);
   }
   vblend_row(dst+(size_t)y*dw,lh[s0],lh[s1],dw,wy);
  }
  uint8_t *uh[2]={(uint8_t *)hbuf[0],(uint8_t *)hbuf[0]+1024u};
  uint8_t *vh[2]={(uint8_t *)hbuf[1],(uint8_t *)hbuf[1]+1024u};
  int32_t crow_idx[2]={-1,-1};
  uint32_t cy_num=(sh_c-1u)<<4,cy_den=ch>1u?ch-1u:1u;
  uint32_t cy_qs=cy_num/cy_den,cy_rs=cy_num%cy_den,cfy=0u,cy_rem=0u;
  for(unsigned y=0;y<ch;y++){
   unsigned iy=cfy>>4,wy=cfy&15u,iy1=iy+1u<sh_c?iy+1u:iy;
   cfy+=cy_qs;cy_rem+=cy_rs;if(cy_rem>=cy_den){cy_rem-=cy_den;cfy++;}
   int miss=0;
   unsigned s0=cache_slot(crow_idx,iy,wy?(int32_t)iy1:-1,&miss);
   if(miss)hscale_uv(uh[s0],vh[s0],s->uv+(size_t)iy*s->uv_stride,sw_c,cw,sbuf,cached);
   if(wy==0u){
    vblend_row(u+(size_t)y*cw,uh[s0],uh[s0],cw,0u);
    vblend_row(v+(size_t)y*cw,vh[s0],vh[s0],cw,0u);
    continue;
   }
   unsigned s1=s0;
   if(iy1!=iy){
    s1=cache_slot(crow_idx,iy1,(int32_t)iy,&miss);
    if(miss)hscale_uv(uh[s1],vh[s1],s->uv+(size_t)iy1*s->uv_stride,sw_c,cw,sbuf,cached);
   }
   vblend_row(u+(size_t)y*cw,uh[s0],uh[s1],cw,wy);
   vblend_row(v+(size_t)y*cw,vh[s0],vh[s1],cw,wy);
  }
  return 1;
 }
#ifdef LIVE_HOST_TEST
 uint8_t *srow[2]={sbuf,(uint8_t *)hbuf};
 int32_t row_idx[2]={-1,-1};
 uint64_t x_num=(uint64_t)(w-1u)<<8,y_num=(uint64_t)(h-1u)<<4;
 unsigned x_den=dw>1u?dw-1u:1u,y_den=dh>1u?dh-1u:1u;
 for(unsigned y=0;y<dh;y++){
  uint32_t fy=(uint32_t)(((uint64_t)y*y_num)/y_den);
  unsigned iy=fy>>4,wy=fy&15u,w0=16u-wy,iy1=iy+1u<h?iy+1u:iy;
  int miss=0;
  unsigned s0=cache_slot(row_idx,iy,wy?(int32_t)iy1:-1,&miss);
  if(miss)burst_copy(srow[s0],s->y+(size_t)iy*s->y_stride,w);
  unsigned s1=s0;
  if(wy&&iy1!=iy){
   s1=cache_slot(row_idx,iy1,(int32_t)iy,&miss);
   if(miss)burst_copy(srow[s1],s->y+(size_t)iy1*s->y_stride,w);
  }
  const uint8_t *r0=srow[s0],*r1=srow[s1];
  uint8_t *dy=dst+(size_t)y*dw;
  for(unsigned x=0;x<dw;x++){
   uint64_t fx=((uint64_t)x*x_num)/x_den;
   unsigned ix=(unsigned)(fx>>8),wx=(unsigned)(fx&255u),ix1=ix+1u<w?ix+1u:ix,wx0=256u-wx;
   unsigned p0=(wx0*r0[ix]+wx*r0[ix1]+128u)>>8;
   unsigned p1=wy?(wx0*r1[ix]+wx*r1[ix1]+128u)>>8:p0;
   dy[x]=(uint8_t)((w0*p0+wy*p1+8u)>>4);
  }
 }
 int32_t crow_idx[2]={-1,-1};
 uint64_t cx_num=(uint64_t)(sw_c-1u)<<8,cy_num=(uint64_t)(sh_c-1u)<<4;
 unsigned cx_den=cw>1u?cw-1u:1u,cy_den=ch>1u?ch-1u:1u;
 for(unsigned y=0;y<ch;y++){
  uint32_t fy=(uint32_t)(((uint64_t)y*cy_num)/cy_den);
  unsigned iy=fy>>4,wy=fy&15u,w0=16u-wy,iy1=iy+1u<sh_c?iy+1u:iy;
  int miss=0;
  unsigned s0=cache_slot(crow_idx,iy,wy?(int32_t)iy1:-1,&miss);
  if(miss)burst_copy(srow[s0],s->uv+(size_t)iy*s->uv_stride,w);
  unsigned s1=s0;
  if(wy&&iy1!=iy){
   s1=cache_slot(crow_idx,iy1,(int32_t)iy,&miss);
   if(miss)burst_copy(srow[s1],s->uv+(size_t)iy1*s->uv_stride,w);
  }
  const uint8_t *r0=srow[s0],*r1=srow[s1];
  uint8_t *du=u+(size_t)y*cw,*dv=v+(size_t)y*cw;
  for(unsigned x=0;x<cw;x++){
   uint64_t fx=((uint64_t)x*cx_num)/cx_den;
   unsigned ix=(unsigned)(fx>>8),wx=(unsigned)(fx&255u),ix1=ix+1u<sw_c?ix+1u:ix,wx0=256u-wx;
   unsigned u0=(wx0*r0[2u*ix]+wx*r0[2u*ix1]+128u)>>8;
   unsigned v0=(wx0*r0[2u*ix+1u]+wx*r0[2u*ix1+1u]+128u)>>8;
   unsigned u1=wy?(wx0*r1[2u*ix]+wx*r1[2u*ix1]+128u)>>8:u0;
   unsigned v1=wy?(wx0*r1[2u*ix+1u]+wx*r1[2u*ix1+1u]+128u)>>8:v0;
   du[x]=(uint8_t)((w0*u0+wy*u1+8u)>>4);
   dv[x]=(uint8_t)((w0*v0+wy*v1+8u)>>4);
  }
 }
 return 1;
#else
 return 0;
#endif
}
int live_stream_convert_scaled(uint8_t *dst,unsigned dw,unsigned dh,const struct la_source *s) {return convert_scaled_ex(dst,dw,dh,s,0);}

/* Integer microseconds on a 30000/1001 grid; absolute index avoids drift. */
static __attribute__((noinline)) uint64_t cfr_time(const struct live_stream *s,uint64_t n){return s->cfr_epoch+n*1001000u/30u;}

/* ===================== V70 Component 1: Flip-Observed 6-Buffer Ring ===================== */
LIVE_V70_SEC
int live_flip_obs_should_poll(const struct live_flip_obs *obs,uint64_t irq_seq,uint64_t now) {
 if(!obs || !irq_seq || (irq_seq&1u))return 0;
 if(irq_seq!=obs->obs_seq)return 1;
 if(obs->obs_pending_flip){
  if(now>=obs->obs_pending_since &&
     (now-obs->obs_pending_since<=LIVE_FLIP_PENDING_MAX_US ||
      obs->last_poll_us<obs->obs_pending_since+LIVE_FLIP_PENDING_MAX_US)){
   if(!obs->last_poll_us || now<obs->last_poll_us || now-obs->last_poll_us>=LIVE_FLIP_REPOLL_MIN_US)
    return 1;
  }
 }
 return 0;
}

LIVE_V70_SEC
void live_flip_obs_reset(struct live_flip_obs *obs) {
 if(!obs)return;
 /* Preserve lifetime diagnostic counters (flip_obs_count, interp_pushes,
  * direct_nv12_ok, direct_nv12_retrig, async_harvests); reset active ring state. */
 memset(obs->ring,0,sizeof(obs->ring));
 obs->obs_pending_since=0;
 obs->last_poll_us=0;
 obs->last_s4=0;
 obs->last_s4_uv=0;
 obs->last_obs_us=0;
 obs->flip_seq_head=0;
 obs->last_kept_flip_seq=0;
 obs->last_irq_stamp=0;
 obs->ring_head=0;
 obs->ring_count=0;
 obs->obs_pending_flip=0;
 obs->last_w=0;obs->last_h=0;obs->last_ys=0;obs->last_uvs=0;obs->last_epoch=0;obs->flip_dt_us=0;
 /* V71.3 P0-2c: Preserve fs_last_encoded_gen & fs_last_mode across picker_reset/EV_SOFT_CANCEL
  * so 220 ms shutter blackout uses 0-copy retrig instead of re-running upscale+grain 4x on
  * the stale pre-blackout front slot. cfr_reset()/release() clear them on full stream reset. */
 obs->fs_pad=0;
}

static LIVE_V70_SEC void v70_flip_push(struct live_flip_obs *obs,uint64_t y,uint64_t uv,
 uint64_t flip_us,uint32_t w,uint32_t h,uint32_t ys,uint32_t uvs,uint32_t epoch,uint32_t flags) {
 if(!obs || !y || !uv || !w || !h)return;
 uint64_t seq=++obs->flip_seq_head;
 uint32_t idx=obs->ring_head%LIVE_FLIP_RING_CAP;
 obs->ring[idx]=(struct live_flip_entry){y,uv,flip_us,seq,w,h,ys,uvs,epoch,flags};
 obs->ring_head=(idx+1u)%LIVE_FLIP_RING_CAP;
 if(obs->ring_count<LIVE_FLIP_RING_CAP)obs->ring_count++;
 obs->flip_obs_count++;
}

LIVE_V70_SEC
void live_flip_obs_ingest_snapshot(struct la_capture *c,struct live_stream *s,
 struct live_flip_obs *obs,uint64_t seq,uint64_t stamp,uint64_t now,uint32_t epoch,
 uintptr_t cur_y,uintptr_t cur_uv,uint32_t width,uint32_t height,
 uint32_t y_stride,uint32_t uv_stride) {
 if(!c || !s || !obs || !cur_y || !cur_uv || !width || !height)return;
 struct live_diag *D=&s->diag;
 uint64_t prev_obs_seq=obs->obs_seq;
 uint64_t prev_poll_us=obs->last_poll_us;
 uint32_t prev_pending=obs->obs_pending_flip;
 int new_irq=(seq!=prev_obs_seq);
 if(new_irq){
  if(c->last_generation && seq>c->last_generation+2u){
   uint64_t n=(seq-c->last_generation)/2u-1u;
   c->dropped+=n;s->backlog_dropped+=n;
  }
  c->last_generation=seq;
  D->new_seq++;
  if(stamp && now>=stamp){
   D->last_age_us=now-stamp;
   if(D->last_age_us>D->max_age_us)D->max_age_us=D->last_age_us;
  }
 }
 obs->obs_seq=seq;
 obs->last_poll_us=now;
 obs->last_irq_stamp=stamp;
 /* Maintain pool membership table from observed s[3+ch] addresses.
  * On 8-slot saturation reset cleanly so a silent pool switch never wedges nbufs at 8. */
 {
  unsigned k;
  for(k=0;k<D->nbufs && D->bufs[k]!=cur_y;k++){}
  if(k==D->nbufs){
   if(D->nbufs>=8u){
    D->nbufs=0;s->trans_n=0;s->last_y=0;
    live_flip_obs_reset(obs);
    obs->obs_seq=seq;obs->last_poll_us=now;obs->last_irq_stamp=stamp;
   }
   D->bufs[D->nbufs++]=cur_y;
  }
 }
 /* First snapshot after boot/reset: record DMA's current active write buffer. */
 if(!obs->last_s4){
  uint64_t init_ts=(stamp && stamp<=now && now-stamp<40000u)?stamp:now;
  obs->last_s4=cur_y;obs->last_s4_uv=cur_uv;obs->last_obs_us=init_ts;
  obs->last_w=width;obs->last_h=height;obs->last_ys=y_stride;obs->last_uvs=uv_stride;
  obs->last_epoch=epoch;
  s->last_y=cur_y;s->last_gen=seq>>1;
  if(s->tune.picker<2u){
   v70_flip_push(obs,cur_y,cur_uv,init_ts,width,height,y_stride,uv_stride,epoch,LIVE_FLIP_FLAG_BOOT);
   obs->obs_pending_flip=0u;
  }else{
   obs->obs_pending_flip=1u;
   obs->obs_pending_since=(stamp && stamp<=now)?stamp:now;
  }
  return;
 }
 /* No flip yet: keep pending-flip re-poll alive up to 12 ms after this C0 IRQ. */
 if(cur_y==obs->last_s4){
  if(new_irq){
   obs->obs_pending_flip=1u;
   obs->obs_pending_since=(stamp && stamp<=now)?stamp:now;
  }else if(obs->obs_pending_flip && now>=obs->obs_pending_since &&
           now-obs->obs_pending_since>LIVE_FLIP_PENDING_MAX_US){
   obs->obs_pending_flip=0u;
  }
  return;
 }
 /* Actual buffer flip detected (cur_y != obs->last_s4)!
  * Learn trans[cur_y] = last_s4 only when guaranteed at most 1 flip occurred since prev_poll_us.
  * V71 tightened 120 Hz EVF window: whenever smoothed flip_dt_us < 13715 us (e.g. 120 Hz ~8.34 ms),
  * cap single-step learning to 7/8 * flip_dt (~7.30 ms at 120 Hz, floor 3000 us, cap 7800 us when
  * flip_dt < 10000 us) so an 8.5..12 ms 2-flip gap cannot corrupt trans[], while 60 Hz stays 12000 us. */
 uint32_t step_win=LIVE_FLIP_PENDING_MAX_US;
 if(obs->flip_dt_us && obs->flip_dt_us<13715u){
  uint32_t adaptive=(obs->flip_dt_us*7u)>>3;
  if(adaptive<3000u)adaptive=3000u;
  if(obs->flip_dt_us<10000u && adaptive>7800u)adaptive=7800u;
  if(adaptive>LIVE_FLIP_PENDING_MAX_US)adaptive=LIVE_FLIP_PENDING_MAX_US;
  step_win=adaptive;
 }
 int single_step=(seq==prev_obs_seq) ||
                 (!prev_pending && prev_obs_seq && seq==prev_obs_seq+2u) ||
                 (prev_poll_us && now>=prev_poll_us && now-prev_poll_us<=step_win);
 if(single_step){
  unsigned k;
  for(k=0;k<s->trans_n && s->trans[k].to!=cur_y;k++){}
  if(k<8u){
   s->trans[k].from=obs->last_s4;
   s->trans[k].to=cur_y;
   if(k==s->trans_n)s->trans_n++;
  }
 }
 uint64_t prev=0,prev_uv=0;
 if(s->tune.picker<2u){
  prev=cur_y;prev_uv=cur_uv;
 }else{
  for(unsigned k=0;k<s->trans_n;k++){
   if(s->trans[k].to==cur_y){prev=s->trans[k].from;break;}
  }
  if(!prev && s->tune.picker==3u){
   uint64_t best=0,mx=0;
   for(unsigned k=0;k<D->nbufs;k++){
    uint64_t b=D->bufs[k];
    if(b>mx)mx=b;
    if(b<cur_y && b>best)best=b;
   }
   prev=best?best:(mx&&mx!=cur_y?mx:0u);
  }
  if(prev){s->picked_prev++;}
  else{prev=obs->last_s4;s->picked_fallback++;}
  if(prev==obs->last_s4){
   prev_uv=obs->last_s4_uv;
  }else{
   int64_t uv_delta=(int64_t)cur_uv-(int64_t)cur_y;
   prev_uv=(uint64_t)((int64_t)prev+uv_delta);
  }
 }
 /* V71.3 P0-2a: Hardware-calibrated flip timestamp from C0 ISR stamp (same CNTPCT_EL0
  * clock domain as now_us()) with strict sanity guard; falls back safely to now on
  * anomalous/stale clocks. When prev_pending && new_irq && single-flip, attribute
  * the delayed flip to the previous IRQ's obs_pending_since timestamp. */
 uint64_t flip_ts=now;
 if(stamp && stamp<=now && now-stamp<40000u){
  flip_ts=stamp;
 }
 if(prev_pending && new_irq && prev==obs->last_s4 && now>stamp){
  uint64_t ps=obs->obs_pending_since;
  if(ps && ps<flip_ts && now>=ps && now-ps<40000u && (obs->flip_seq_head==0u || ps>obs->last_obs_us)){
   flip_ts=ps;
  }else if(prev_poll_us && stamp>prev_poll_us){
   flip_ts=prev_poll_us+((stamp-prev_poll_us)>>1);
  }
 }
 if(obs->last_obs_us && flip_ts<obs->last_obs_us){
  flip_ts=(now>=obs->last_obs_us)?now:obs->last_obs_us;
 }
 /* Preemption-safe intermediate recovery: if a 10 Hz CPU0 preemption spanned 2 or 3 flips
  * (dt < 45 ms, prev != last_s4), walk trans[] back to last_s4 and push the untouched
  * intermediate buffers with evenly interpolated hardware-calibrated timestamps before prev! */
 uint64_t dt_flip=(flip_ts>=obs->last_obs_us)?(flip_ts-obs->last_obs_us):0u;
 if(s->tune.picker>=2u && prev!=obs->last_s4 && obs->last_s4 && obs->last_s4_uv &&
    dt_flip>0u && dt_flip<45000u &&
    obs->last_w==width && obs->last_h==height && obs->last_epoch==epoch){
  uint64_t prev2=0,prev3=0;
  for(unsigned k=0;k<s->trans_n;k++){if(s->trans[k].to==prev){prev2=s->trans[k].from;break;}}
  if(prev2 && prev2!=obs->last_s4 && prev2!=prev && prev2!=cur_y){
   for(unsigned k=0;k<s->trans_n;k++){if(s->trans[k].to==prev2){prev3=s->trans[k].from;break;}}
  }
  if(prev3==obs->last_s4){
   /* 3 flips spanned (e.g. 120 Hz EVF during 18 ms preemption): last_s4 -> prev2 -> prev */
   int64_t uv_delta=(int64_t)cur_uv-(int64_t)cur_y;
   uint64_t prev2_uv=(uint64_t)((int64_t)prev2+uv_delta);
   v70_flip_push(obs,obs->last_s4,obs->last_s4_uv,obs->last_obs_us+dt_flip/3u,
                 obs->last_w,obs->last_h,obs->last_ys,obs->last_uvs,obs->last_epoch,
                 LIVE_FLIP_FLAG_INTERP);
   v70_flip_push(obs,prev2,prev2_uv,obs->last_obs_us+(dt_flip*2u)/3u,
                 width,height,y_stride,uv_stride,epoch,LIVE_FLIP_FLAG_INTERP);
   obs->interp_pushes+=2u;
  }else if(prev2==obs->last_s4){
   /* Exact 2-flip span: last_s4 -> prev; never push 4..5-flip-old last_s4 as head - 1! */
   v70_flip_push(obs,obs->last_s4,obs->last_s4_uv,obs->last_obs_us+(dt_flip>>1),
                 obs->last_w,obs->last_h,obs->last_ys,obs->last_uvs,obs->last_epoch,
                 LIVE_FLIP_FLAG_INTERP);
   obs->interp_pushes++;
  }
 }
 if(obs->flip_seq_head>=1u && single_step && prev==obs->last_s4 && flip_ts>obs->last_obs_us){
  uint64_t dt_obs=flip_ts-obs->last_obs_us;
  if(dt_obs>=3500u && dt_obs<=50000u){
   uint32_t d32=(uint32_t)dt_obs;
   obs->flip_dt_us=obs->flip_dt_us?((obs->flip_dt_us*3u+d32)>>2):d32;
  }
 }
 v70_flip_push(obs,prev,prev_uv,flip_ts,width,height,y_stride,uv_stride,epoch,0u);
 obs->last_s4=cur_y;obs->last_s4_uv=cur_uv;obs->last_obs_us=flip_ts;
 obs->last_w=width;obs->last_h=height;obs->last_ys=y_stride;obs->last_uvs=uv_stride;
 obs->last_epoch=epoch;
 s->last_y=cur_y;s->last_gen=seq>>1;
 if(new_irq && stamp && now>=stamp && now-stamp<10000u &&
    ((prev_pending && now>stamp) || (prev_obs_seq && seq>prev_obs_seq+2u))){
  obs->obs_pending_flip=1u;
  obs->obs_pending_since=stamp;
 }else{
  obs->obs_pending_flip=0u;
 }
}

/* V71.3 P0-2b: 60Hz (stride=2) / 120Hz (stride=4) stride-lock guard.
 * When the target flip (last_kept + stride) has not yet landed in obs->ring
 * and no safe candidate is already >= 28 ms past cfr_source_us, hold briefly
 * up to 8 ms (60Hz) / 6 ms (120Hz) instead of greedily grabbing an intermediate
 * odd flip and causing 1:3 (16.7 / 50.0 ms) sawtooth jitter. */
static LIVE_V70_SEC int v70_should_stride_wait(
 const struct live_stream *s,const struct live_flip_obs *obs,
 uint64_t tick,uint32_t cur_epoch,uint64_t now) {
 if(!s || !obs || s->cfr_have<2u || !obs->last_kept_flip_seq || !obs->flip_dt_us)return 0;
 uint32_t stride=0u,wait_us=0u;
 if(obs->flip_dt_us>=6500u && obs->flip_dt_us<=10500u){
  stride=4u;wait_us=6000u;
 }else if(obs->flip_dt_us>=13500u && obs->flip_dt_us<=20000u){
  stride=2u;wait_us=8000u;
 }else{
  return 0;
 }
 if(obs->flip_seq_head>=obs->last_kept_flip_seq+stride)return 0;
 if(now>=tick+wait_us)return 0;
 uint32_t nb=s->diag.nbufs;
 uint32_t eff_nb=nb<2u?2u:(nb>LIVE_FLIP_RING_CAP?LIVE_FLIP_RING_CAP:nb);
 uint32_t safe_age=(eff_nb>=3u)?(eff_nb-2u):1u;
 for(unsigned i=0;i<obs->ring_count;i++){
  const struct live_flip_entry *h=&obs->ring[i];
  if(!h->y || !h->uv || (h->flags&LIVE_FLIP_FLAG_CONSUMED))continue;
  if(h->flip_seq<=obs->last_kept_flip_seq)continue;
  if(h->lease_epoch!=cur_epoch)continue;
  if(s->tune.picker>=2u && obs->last_s4 && h->y==obs->last_s4)continue;
  if(obs->flip_seq_head<h->flip_seq || (obs->flip_seq_head-h->flip_seq)>=safe_age)continue;
  if(s->tune.stale_us && now>h->flip_us && now-h->flip_us>s->tune.stale_us)continue;
  if(s->cfr_source_us && h->flip_us>=s->cfr_source_us+28000u)return 0;
 }
 return 1;
}

/* Pick the safe untorn flip_ring entry minimizing |h.flip_us - tick|. */
static LIVE_V70_SEC struct live_flip_entry *v70_pick_candidate(
 const struct live_stream *s,struct live_flip_obs *obs,
 uint64_t tick,uint32_t cur_epoch,uint64_t now) {
 if(!obs || !obs->ring_count)return 0;
 if(v70_should_stride_wait(s,obs,tick,cur_epoch,now))return 0;
 uint32_t nb=s->diag.nbufs;
 uint32_t eff_nb=nb<2u?2u:(nb>LIVE_FLIP_RING_CAP?LIVE_FLIP_RING_CAP:nb);
 uint32_t safe_age=(eff_nb>=3u)?(eff_nb-2u):1u;
 struct live_flip_entry *best=0;
 uint64_t best_dist=UINT64_MAX;
 for(unsigned i=0;i<obs->ring_count;i++){
  struct live_flip_entry *h=&obs->ring[i];
  if(!h->y || !h->uv || (h->flags&LIVE_FLIP_FLAG_CONSUMED))continue;
  if(h->flip_seq<=obs->last_kept_flip_seq)continue;
  if(h->lease_epoch!=cur_epoch)continue;
  if(s->tune.picker>=2u && obs->last_s4 && h->y==obs->last_s4)continue;
  if(obs->flip_seq_head<h->flip_seq || (obs->flip_seq_head-h->flip_seq)>=safe_age)continue;
  if(s->tune.stale_us && now>h->flip_us && now-h->flip_us>s->tune.stale_us)continue;
  /* V71: once CFR is running (cfr_have!=0), never steal a future flip (> half a 30fps frame
   * after tick) that belongs to tick+1; on startup (cfr_have==0) no prior frame exists to repeat. */
  if(s->cfr_have && h->flip_us>tick && h->flip_us-tick>16667u)continue;
  uint64_t dist=(h->flip_us>=tick)?(h->flip_us-tick):(tick-h->flip_us);
  if(!best || dist<best_dist || (dist==best_dist && h->flip_seq>best->flip_seq)){
   best=h;best_dist=dist;
  }
 }
 return best;
}

/* V71.3 Mode-2 decoupled CPU2 feeder fallback: pick the safe untorn flip
 * (age < safe_age, ignoring flip_us > tick + 16667u) minimizing |h.flip_us - tick|
 * (closest-to-tick, tie-break newer flip_seq) so CPU2 never starves across a lag/grace
 * tick without jumping to max flip_seq and causing big-step/small-step sawtooth! */
static LIVE_V70_SEC struct live_flip_entry *v70_pick_newest_safe(
 const struct live_stream *s,struct live_flip_obs *obs,
 uint64_t tick,uint32_t cur_epoch,uint64_t now) {
 if(!obs || !obs->ring_count)return 0;
 if(v70_should_stride_wait(s,obs,tick,cur_epoch,now))return 0;
 uint32_t nb=s->diag.nbufs;
 uint32_t eff_nb=nb<2u?2u:(nb>LIVE_FLIP_RING_CAP?LIVE_FLIP_RING_CAP:nb);
 uint32_t safe_age=(eff_nb>=3u)?(eff_nb-2u):1u;
 struct live_flip_entry *best=0;
 uint64_t best_dist=UINT64_MAX;
 for(unsigned i=0;i<obs->ring_count;i++){
  struct live_flip_entry *h=&obs->ring[i];
  if(!h->y || !h->uv || (h->flags&LIVE_FLIP_FLAG_CONSUMED))continue;
  if(h->flip_seq<=obs->last_kept_flip_seq)continue;
  if(h->lease_epoch!=cur_epoch)continue;
  if(s->tune.picker>=2u && obs->last_s4 && h->y==obs->last_s4)continue;
  if(obs->flip_seq_head<h->flip_seq || (obs->flip_seq_head-h->flip_seq)>=safe_age)continue;
  if(s->tune.stale_us && now>h->flip_us && now-h->flip_us>s->tune.stale_us)continue;
  uint64_t dist=(h->flip_us>=tick)?(h->flip_us-tick):(tick-h->flip_us);
  if(!best || dist<best_dist || (dist==best_dist && h->flip_seq>best->flip_seq)){
   best=h;best_dist=dist;
  }
 }
 return best;
}

static LIVE_V70_SEC const struct live_flip_entry *v70_peek_first_flip(
 const struct live_stream *s,const struct live_flip_obs *obs,uint32_t cur_epoch,uint64_t now) {
 if(!obs || !obs->ring_count)return 0;
 const struct live_flip_entry *best=0;
 for(unsigned i=0;i<obs->ring_count;i++){
  const struct live_flip_entry *h=&obs->ring[i];
  if(!h->y || !h->uv || (h->flags&LIVE_FLIP_FLAG_CONSUMED))continue;
  if(h->flip_seq<=obs->last_kept_flip_seq)continue;
  if(h->lease_epoch!=cur_epoch)continue;
  if(s->tune.picker>=2u && obs->last_s4 && h->y==obs->last_s4)continue;
  if(s->tune.stale_us && now>h->flip_us && now-h->flip_us>s->tune.stale_us)continue;
  if(!best || h->flip_seq>best->flip_seq)best=h;
 }
 return best;
}

/* ===================== V70 Async Wave5 Harvest & Flush Helpers ===================== */
static LIVE_V70_SEC int v70_push_au(struct la_capture *c,struct live_stream *s,
 uint64_t pts,const uint8_t *au,size_t n,unsigned idr,uint64_t now) {
 if(!au || !n || n>UINT32_MAX || au_ring_push(&s->ring,au,n,pts,idr)){
  s->error=LA_OVERFLOW;STORE(&c->phase,LA_FAILED);return -1;
 }
 s->encoded++;s->last_pts=pts;if(!s->first_pts)s->first_pts=pts;
 s->ring_fill=s->ring.used;if(s->ring_fill>s->ring_peak)s->ring_peak=s->ring_fill;
 c->count=s->ring.count;
 if(pts>s->first_pts && s->cfr_index>1u)
  s->effective_fps_milli=(s->cfr_index-1u)*UINT64_C(1000000000)/(pts-s->first_pts);
 s->low_fps=(now>pts+100000u);
 return 0;
}

/* Poll 1-deep async Wave5 encoder. Returns 1 harvested, 0 idle/pending,
 * LIVE_ENCODER_BUSY on preempt, <0 on fatal error. */
static LIVE_V70_SEC int v70_harvest_au(struct la_capture *c,struct live_stream *s,
 struct live_flip_obs *obs,int block,uint64_t now,uint64_t (*clock_us)(void)) {
 if(!c->descriptor)return 0;
 uint64_t pts=0;const uint8_t *au=0;size_t n=0;unsigned idr=0;
 uint64_t t0=clock_us?clock_us():0u;
 int rc=live_encoder_poll_au(&s->encoder,block,&pts,&au,&n,&idr);
 uint64_t dt=clock_us?(clock_us()-t0):0u;
 if(rc==0)return 0;
 if(rc==LIVE_ENCODER_BUSY){
  s->diag.busy++;c->dropped++;s->copy_dropped++;
  if(s->cfr_index)s->cfr_index--;
  return LIVE_ENCODER_BUSY;
 }
 if(rc<0){
  s->diag.enc_error++;live_log(s,now,EV_ENC_ERR,(uint32_t)-rc,0);
  s->error=(uint32_t)-rc;STORE(&c->phase,LA_FAILED);
  return rc;
 }
 s->encode_total_us+=dt;
 if(dt>s->encode_max_us)s->encode_max_us=dt;
 if(obs)obs->async_harvests++;
 if(v70_push_au(c,s,pts,au,n,idr,now)<0)return -1;
 if(s->encoded)s->encode_avg_us=s->encode_total_us/s->encoded;
 return 1;
}

static LIVE_V70_SEC int v70_flush_encoder(struct la_capture *c,struct live_stream *s,
 struct live_flip_obs *obs,int push_to_ring,uint64_t now) {
 if(!s->encoder.hw && !s->encoder.persist)return 0;
 uint64_t pts=0;const uint8_t *au=0;size_t n=0;unsigned idr=0;
 int rc=live_encoder_flush(&s->encoder,&pts,&au,&n,&idr);
 if(rc==1){
  if(obs)obs->async_harvests++;
  if(push_to_ring){
   if(v70_push_au(c,s,pts,au,n,idr,now)<0)return -1;
  }
  return 1;
 }
 if(rc<0 && rc!=LIVE_ENCODER_BUSY){
  s->diag.enc_error++;
  if(push_to_ring){
   live_log(s,now,EV_ENC_ERR,(uint32_t)-rc,0);
   s->error=(uint32_t)-rc;STORE(&c->phase,LA_FAILED);
  }
  return rc;
 }
 return 0;
}

static void picker_reset(struct la_capture *c,struct live_stream *s,struct live_flip_obs *obs){
 s->diag.nbufs=0;s->trans_n=0;s->last_y=0;s->last_gen=0;s->last_kept_y=0;s->log_pad=0;s->preview_pad=0;
 if(obs){
  live_flip_obs_reset(obs);
  if(c){
   uint64_t q=LOAD(&c->irq_seq);
   if(!(q&1u))obs->obs_seq=q;
  }
 }
}

static void cfr_reset(struct la_capture *c,struct live_stream *s,struct live_flip_obs *obs){
 (void)v70_flush_encoder(c,s,obs,0,0);
 s->cfr_pad++; /* Also invalidates temporal source observations on release/resume. */
 s->cfr_have=0;s->cfr_epoch=0;s->cfr_index=0;s->cfr_source_us=0;
 s->last_pts=0;s->first_pts=0;
 /* V71.3 Track A: if encoder is already warm, request an immediate HEVC IDR
  * on the first frame after cfr_reset() so post-shot / PB-resume readiness
  * does not stall up to 14 P-frames (~467 ms) waiting for the next periodic GOP. */
 s->last_idr_us=s->encoded?0xFFFFFFFF00000000ULL:0u;
 picker_reset(c,s,obs);
 if(obs){obs->fs_last_encoded_gen=0u;obs->fs_last_mode=0u;obs->fs_pad=0u;}
 s->preview_head=0;s->preview_count=0;s->preview_last=0;
 au_ring_init(&s->ring,s->ring.data,s->ring.capacity);
}

static void release(struct la_capture *c,struct live_stream *s,struct live_flip_obs *obs) {
 (void)v70_flush_encoder(c,s,obs,0,0);
 if(s->cache_base){la_dc_civac(s->cache_base,s->cache_size);s->cache_base=0;s->cache_size=0;}
 if(c->descriptor)c->ops.release(c->ops.context,c->descriptor);
 c->descriptor=0;c->pixels=0;c->actual_bytes=0;c->count=0;c->stream_start=0;
 cfr_reset(c,s,obs);s->cfr_spare=0;
 s->i420=0;s->work=0;s->frozen_bytes=0;s->preview=0;s->preview_scratch=0;s->preview_head=0;s->preview_count=0;s->preview_last=0;s->last_pts=0;s->last_idr_us=0;
 au_ring_init(&s->ring,0,0);STORE(&c->press_mailbox,0);STORE(&s->job,0);STORE(&c->phase,LA_OFF);
}

/* Worker only, after the stream and mux stopped. Diagnostic history is also a
 * photo-pool allocation; it must not pin that pool for a stock playback owner. */
void live_stream_release_aux(struct la_capture *c,struct live_stream *s) {
 if(c->descriptor || LOAD(&s->job))return;
 void *d=s->fr_desc;s->fr_desc=0;s->fr=0;s->fr_head=0;
 if(d)c->ops.release(c->ops.context,d);
}

/* V71.3 Defect 3A: Atomically abort a pre-mux pending clip (job==2 -> 4) so
 * stock handoff (crumb_pad==2) releases capture.descriptor immediately without
 * racing mux_worker's CAS(2 -> 3). Returns 1 if cancelled, 0 if job was not 2. */
int live_stream_abort_pending_job(struct live_stream *s) {
 if(!s)return 0;
 uint32_t expected=2u;
 return __atomic_compare_exchange_n(&s->job,&expected,4u,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED);
}

static int allocate(struct la_capture *c,struct live_stream *s,struct live_flip_obs *obs,unsigned w,unsigned h) {
 if(!s->fr && !s->fr_desc){void *d=0;uint8_t *px=0;uint32_t got=0;
  if(c->ops.allocate(c->ops.context,LIVE_FR*(uint32_t)sizeof(struct live_fr)+64u,&d,&px,&got) && d && px &&
     got>=LIVE_FR*sizeof(struct live_fr)+64u){s->fr_desc=d;s->fr=(struct live_fr *)(((uintptr_t)px+63)&~(uintptr_t)63);
   memset(s->fr,0,LIVE_FR*sizeof(struct live_fr));s->fr_head=0;}
  else{s->fr_alloc_fail++;if(d)c->ops.release(c->ops.context,d);}}
 size_t p,t;if(live_encoder_sizes(w,h,&p,&t))return 0;
 size_t bytes=2*LIVE_BITSTREAM_BYTES+(size_t)w*h*3+p+t+sizeof(*s->work)+(LIVE_PREVIEW_RING_SLOTS+1u)*(size_t)LIVE_PREVIEW_FRAME+512u+160;
 if(bytes>UINT32_MAX)return 0;s->allocation_requested=bytes;
 if(!c->ops.allocate(c->ops.context,bytes,&c->descriptor,&c->pixels,&c->actual_bytes))return 0;
 if(!c->descriptor||!c->pixels||c->actual_bytes<bytes){release(c,s,obs);return 0;}
 if(s->tune.cached_work && !(((uintptr_t)c->pixels|c->actual_bytes)&63)){
  uintptr_t a=la_alias((uintptr_t)c->pixels,c->actual_bytes);
  if(a){la_dc_civac(a,c->actual_bytes);c->pixels=(uint8_t *)a;s->cache_base=a;s->cache_size=c->actual_bytes;s->cached_work_ok++;}
  else s->cached_work_fail++;
 }
 uintptr_t a=align16((uintptr_t)c->pixels);s->i420=(void *)a;a=align16(a+(size_t)w*h*3/2);
 s->cfr_spare=(void *)a;a=align16(a+(size_t)w*h*3/2);
 void *pm=(void *)a;a=align16(a+p);void *tm=(void *)a;a=align16(a+t);
 s->work=(void *)a;a=align16(a+sizeof(*s->work));
 au_ring_init(&s->ring,(void *)a,LIVE_BITSTREAM_BYTES);a+=LIVE_BITSTREAM_BYTES;s->frozen_bytes=(void *)a;
 a=align16(a+LIVE_BITSTREAM_BYTES);s->preview=(void *)a;a=align16(a+LIVE_PREVIEW_RING_SLOTS*(size_t)LIVE_PREVIEW_FRAME);s->preview_scratch=(void *)a;
 memset((void *)align16((uintptr_t)s->preview_scratch+LIVE_PREVIEW_FRAME),0,LV_ALIAS_SLOTS*sizeof(struct lv_alias_slot));
 s->preview_head=0;s->preview_count=0;s->preview_last=0;
 s->width=w;s->height=h;s->first_pts=0;s->last_pts=0;
 if(live_encoder_init(&s->encoder,pm,tm,w,h)){release(c,s,obs);return 0;}return 1;
}

/* Nearest-neighbour I420 downscale of the frame just encoded into one slot (FilmSim / fallback). */
static LIVE_V70_SEC void preview_push(struct live_stream *s,uint64_t pts,const uint8_t *frame) {
 if(!s->preview||!frame||(s->preview_last&&pts>=s->preview_last&&pts-s->preview_last<LIVE_PREVIEW_RING_STEP_US-8000u))return;
 unsigned w=s->width,h=s->height;const uint8_t *sy=frame,*su=sy+(size_t)w*h,*sv=su+(size_t)w*h/4;
 uint8_t *d=s->preview+(size_t)s->preview_head*LIVE_PREVIEW_FRAME,*du=d+LIVE_PREVIEW_W*LIVE_PREVIEW_H,*dv=du+LIVE_PREVIEW_W*LIVE_PREVIEW_H/4;
 if(w==1280u){
  unsigned yq=h/LIVE_PREVIEW_H,yr=h%LIVE_PREVIEW_H,syi=0u,rem=0u;
  for(unsigned y=0;y<LIVE_PREVIEW_H;y++){
   const uint8_t *rp=sy+(size_t)syi*1280u;
   syi+=yq;rem+=yr;if(rem>=LIVE_PREVIEW_H){rem-=LIVE_PREVIEW_H;syi++;}
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
   la_u32a *dp=(la_u32a *)(void *)(d+(size_t)y*1024u);
   for(unsigned k=0;k<256u;k++,rp+=5u)
    dp[k]=(uint32_t)rp[0]|((uint32_t)rp[1]<<8)|((uint32_t)rp[2]<<16)|((uint32_t)rp[3]<<24);
#else
   uint8_t *dp=d+(size_t)y*1024u;
   for(unsigned k=0;k<256u;k++,rp+=5u,dp+=4u){dp[0]=rp[0];dp[1]=rp[1];dp[2]=rp[2];dp[3]=rp[3];}
#endif
  }
  unsigned ch=h/2u,ph=LIVE_PREVIEW_H/2u,cq=ch/ph,cr=ch%ph,cyi=0u,crem=0u;
  for(unsigned y=0;y<ph;y++){
   size_t off=(size_t)cyi*640u;
   cyi+=cq;crem+=cr;if(crem>=ph){crem-=ph;cyi++;}
   const uint8_t *rpu=su+off,*rpv=sv+off;
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
   la_u32a *dpu=(la_u32a *)(void *)(du+(size_t)y*512u),*dpv=(la_u32a *)(void *)(dv+(size_t)y*512u);
   for(unsigned k=0;k<128u;k++,rpu+=5u,rpv+=5u){
    dpu[k]=(uint32_t)rpu[0]|((uint32_t)rpu[1]<<8)|((uint32_t)rpu[2]<<16)|((uint32_t)rpu[3]<<24);
    dpv[k]=(uint32_t)rpv[0]|((uint32_t)rpv[1]<<8)|((uint32_t)rpv[2]<<16)|((uint32_t)rpv[3]<<24);
   }
#else
   uint8_t *dpu=du+(size_t)y*512u,*dpv=dv+(size_t)y*512u;
   for(unsigned k=0;k<128u;k++,rpu+=5u,rpv+=5u,dpu+=4u,dpv+=4u){
    dpu[0]=rpu[0];dpu[1]=rpu[1];dpu[2]=rpu[2];dpu[3]=rpu[3];
    dpv[0]=rpv[0];dpv[1]=rpv[1];dpv[2]=rpv[2];dpv[3]=rpv[3];
   }
#endif
  }
 }else{
  for(unsigned y=0;y<LIVE_PREVIEW_H;y++){const uint8_t *row=sy+(size_t)(y*h/LIVE_PREVIEW_H)*w;
   for(unsigned x=0;x<LIVE_PREVIEW_W;x++)d[y*LIVE_PREVIEW_W+x]=row[x*w/LIVE_PREVIEW_W];}
  for(unsigned y=0;y<LIVE_PREVIEW_H/2;y++){size_t r=(size_t)(y*(h/2)/(LIVE_PREVIEW_H/2))*(w/2);
   for(unsigned x=0;x<LIVE_PREVIEW_W/2;x++){size_t c=r+x*(w/2)/(LIVE_PREVIEW_W/2);
    du[y*(LIVE_PREVIEW_W/2)+x]=su[c];dv[y*(LIVE_PREVIEW_W/2)+x]=sv[c];}}
 }
 s->preview_pts[s->preview_head]=pts;s->preview_head=(s->preview_head+1)%LIVE_PREVIEW_RING_SLOTS;
 if(s->preview_count<LIVE_PREVIEW_RING_SLOTS)s->preview_count++;
 s->preview_last=pts;
}

/* V70 fast preview push directly from the cached Wave5 NV12 slot (2.5 fps).
 * live_preview_nv12_to_i420_1024x680 outputs 1024x682 I420 matching LIVE_PREVIEW_FRAME. */
static LIVE_V70_SEC void preview_push_nv12(struct live_stream *s,uint64_t pts,unsigned slot) {
 if(!s->preview||(s->preview_last&&pts>=s->preview_last&&pts-s->preview_last<LIVE_PREVIEW_RING_STEP_US-8000u))return;
 const uint8_t *sy=0,*suv=0;unsigned ys=0,uvs=0;
 if(live_encoder_slot_ptrs(&s->encoder,slot,&sy,&suv,&ys,&uvs)!=0 || !sy || !suv)return;
 uint8_t *d=s->preview+(size_t)s->preview_head*LIVE_PREVIEW_FRAME;
 live_preview_nv12_to_i420_1024x680(d,sy,suv,s->width,s->height,ys,uvs);
 s->preview_pts[s->preview_head]=pts;s->preview_head=(s->preview_head+1u)%LIVE_PREVIEW_RING_SLOTS;
 if(s->preview_count<LIVE_PREVIEW_RING_SLOTS)s->preview_count++;
 s->preview_last=pts;
}

/* V71.3: Fast Mode-2 preview push straight from L2-hot 1024x682 LBUF[front]
 * (avoids reading back the just-flushed cold 1.66 MB Wave5 DRAM slot and skips
 * 5:4 decimation since LBUF[front] is already exact LIVE_PREVIEW_W x LIVE_PREVIEW_H). */
static LIVE_V70_SEC void preview_push_lbuf_nv12(
 struct live_stream *s,uint64_t pts,const uint8_t *fy,const uint8_t *fuv,unsigned fw,unsigned fh) {
 if(!s->preview||!fy||!fuv||fw!=LIVE_PREVIEW_W||fh!=LIVE_PREVIEW_H)return;
 if(s->preview_last&&pts>=s->preview_last&&pts-s->preview_last<LIVE_PREVIEW_RING_STEP_US-8000u)return;
 uint8_t *d=s->preview+(size_t)s->preview_head*LIVE_PREVIEW_FRAME;
 struct la_source fsrc={fy,fuv,fw,fh,fw,fw,0u,pts,0u};
 if(!convert_ex(d,&fsrc,1))return;
 s->preview_pts[s->preview_head]=pts;s->preview_head=(s->preview_head+1u)%LIVE_PREVIEW_RING_SLOTS;
 if(s->preview_count<LIVE_PREVIEW_RING_SLOTS)s->preview_count++;
 s->preview_last=pts;
}

LIVE_V70_SEC
int live_stream_preview_frame(void *stream,uint32_t index,void *out,size_t bytes) {
 struct live_stream *s=stream;
 if(!s||!s->preview||!s->preview_count||!out||bytes!=LIVE_PREVIEW_FRAME||index>=LIVE_PREVIEW_FRAMES)return -1;
 uint64_t span=s->clip_end-s->clip_start;if(!span||span>4000000u)return -1;
 /* The IDR-aligned clip is not always three seconds. Sample its actual
  * frozen interval, matching the duration recorded in the LVJ1 header. */
 uint64_t target=s->clip_start+span*index/LIVE_PREVIEW_FRAMES;
 unsigned oldest=(s->preview_head+LIVE_PREVIEW_RING_SLOTS-s->preview_count)%LIVE_PREVIEW_RING_SLOTS,pick=oldest;
 for(unsigned i=0;i<s->preview_count;i++){unsigned k=(oldest+i)%LIVE_PREVIEW_RING_SLOTS;if(s->preview_pts[k]<=target)pick=k;else break;}
 const uint8_t *src=s->preview+(size_t)pick*LIVE_PREVIEW_FRAME;unsigned o=s->preview_orient;
 if(o!=6&&o!=8&&o!=3){memcpy(out,src,LIVE_PREVIEW_FRAME);return 0;}
 /* v13: upright preview. 3: 180 degrees, full frame. 6/8: rotate 90 cw/ccw, fit CWxH centred, black bars. */
 enum{W=LIVE_PREVIEW_W,H=LIVE_PREVIEW_H,CW=(H*H/W)&~3u,X0=((W-CW)/2)&~1u};
 uint8_t *dy=out,*du=dy+W*H,*dv=du+W*H/4;const uint8_t *sy=src,*su=sy+W*H,*sv=su+W*H/4;
 for(unsigned pl=0;pl<3;pl++){unsigned w=pl?W/2:W,h=pl?H/2:H,cw=pl?CW/2:CW,x0=pl?X0/2:X0;
  const uint8_t *sp=pl==0?sy:pl==1?su:sv;uint8_t *dp=pl==0?dy:pl==1?du:dv;uint8_t fill=pl?128:16;
  for(unsigned y=0;y<h;y++){uint8_t *row=dp+(size_t)y*w;
   if(o==3){const uint8_t *sr=sp+(size_t)(h-1-y)*w;for(unsigned x=0;x<w;x++)row[x]=sr[w-1-x];continue;}
   uint32_t ryf=(uint32_t)(((uint64_t)y*w<<8)/h),mx=(w-1u)<<8,my=(h-1u)<<8; /* v20: 24.8 fixed point */
   if(ryf>mx)ryf=mx;
   for(unsigned x=0;x<w;x++){
    if(x<x0||x>=x0+cw){row[x]=fill;continue;}
    uint32_t rxf=(uint32_t)(((uint64_t)(x-x0)*h<<8)/cw);if(rxf>my)rxf=my;
    uint32_t sxf=o==6?ryf:mx-ryf,syf=o==6?my-rxf:rxf;
    unsigned ix=sxf>>8,iy=syf>>8,fx=sxf&255u,fy=syf&255u,ix1=ix+1u<w?ix+1u:ix,iy1=iy+1u<h?iy+1u:iy;
    const uint8_t *r0=sp+(size_t)iy*w,*r1=sp+(size_t)iy1*w;
    uint32_t top=r0[ix]*(256u-fx)+r0[ix1]*fx,bot=r1[ix]*(256u-fx)+r1[ix1]*fx;
    row[x]=(uint8_t)((top*(256u-fy)+bot*fy+32768u)>>16);}}}
 return 0;
}

static void freeze(struct la_capture *c,struct live_stream *s,struct live_flip_obs *obs,uint64_t now) {
 /* V70 CRITICAL: drain any 1 in-flight async Wave5 AU into s->ring BEFORE freezing
  * so the final post-roll frame is included and 3k alignment & clip_end stay exact. */
 if(v70_flush_encoder(c,s,obs,1,now)<0)return;
 unsigned start;if(au_ring_find_start(&s->ring,s->clip_start,&start))goto fail;
 if(s->ring.entries[(s->ring.head+start)%AU_RING_SLOTS].pts!=s->clip_start)goto fail;
 unsigned count=0,bytes=0;
 for(unsigned i=start;i<s->ring.count;i++){
  const struct au_entry *e=&s->ring.entries[(s->ring.head+i)%AU_RING_SLOTS];
  if(e->pts>=s->clip_end)break;
  if(count==LIVE_MAX_FRAMES || e->size>LIVE_BITSTREAM_BYTES-bytes)goto fail;
  if(au_ring_copy(&s->ring,i,s->frozen_bytes+bytes,LIVE_BITSTREAM_BYTES-bytes)<0)goto fail;
  s->frozen[count++]=(live_input_au){s->frozen_bytes+bytes,e->size,e->pts};bytes+=e->size;
 }
 if(!count||s->frozen[count-1].pts_us<s->clip_press)goto fail;
 while(count>3u&&count%3u!=0u&&s->frozen[count-2].pts_us>=s->clip_press){count--;bytes-=(unsigned)s->frozen[count].bytes;s->clip_end=s->frozen[count].pts_us;} /* v65: 3k native player alignment */
 s->frozen_count=count;s->frozen_size=bytes;s->last_duration_us=s->clip_end-s->frozen[count-1].pts_us;
 s->snapshot_first=s->frozen[0].pts_us;s->snapshot_last=s->frozen[count-1].pts_us;
 s->snapshot_encoded=s->encoded;STORE(&c->phase,LA_FROZEN);STORE(&s->job,2);return;
 fail:s->error=LA_OVERFLOW;STORE(&s->job,0);STORE(&c->phase,LA_REQUESTED);
}

/* v68: Universal 16-row pre/post source-buffer tear verification for BOTH LCD
 * (scaled=1) and EVF (scaled=0). Works even if diagnostic fr ring is absent. */
static LIVE_V70_SEC uint64_t fr_hash_row(const uint8_t *p,unsigned n){
 uint64_t h=UINT64_C(0x9e3779b97f4a7c15)^(uint64_t)n;
 unsigned i=0;
 if(((uintptr_t)p&7u)==0u){
  const la_u64a *q=(const la_u64a *)(const void *)p;
  unsigned nw=n>>3;
  for(unsigned k=0;k<nw;k++){h^=q[k];h*=UINT64_C(0x100000001b3);}
  i=nw<<3;
 }
 for(;i<n;i++){h^=p[i];h*=UINT64_C(0x100000001b3);}
 return h^(h>>33);
}
static LIVE_V70_SEC unsigned fr_pre_hash(const struct live_stream *s,const struct la_source *src,const struct la_source *rd,uint64_t pre_y[16],uint64_t pre_uv[16]){
 unsigned R=s->fr_rows>16u?16u:s->fr_rows;
 if(!R||!src->y||!src->uv||!src->width||!src->height||((src->width|src->height)&1u)||
    src->width>4096u||src->height>4096u||src->y_stride<src->width||src->uv_stride<src->width||
    src->y_stride>8192u||src->uv_stride>8192u)return 0u;
 unsigned w=src->width,h=src->height;
 for(unsigned k=0;k<R;k++){
  unsigned yy=(unsigned)(((uint64_t)k*2u+1u)*h/(2u*R));
  pre_y[k]=fr_hash_row(rd->y+(size_t)yy*src->y_stride,w);
  pre_uv[k]=fr_hash_row(rd->uv+(size_t)(yy>>1)*src->uv_stride,w);
 }
 return R;
}
static LIVE_V70_SEC void fr_post_check(struct live_stream *s,const struct la_source *src,const struct la_source *rd,unsigned R,const uint64_t pre_y[16],const uint64_t pre_uv[16],struct live_fr *f){
 unsigned w=src->width,h=src->height;
 int inv_y=(rd->y!=src->y),inv_uv=(rd->uv!=src->uv);
 /* V71: Batch all 2*R cache-line invalidates before ONE single dsb sy (eliminates 31 barriers/frame). */
 if(inv_y || inv_uv){
  for(unsigned k=0;k<R;k++){
   unsigned yy=(unsigned)(((uint64_t)k*2u+1u)*h/(2u*R));
   if(inv_y)fr_dc_ivac_nb((uintptr_t)(rd->y+(size_t)yy*src->y_stride),w);
   if(inv_uv)fr_dc_ivac_nb((uintptr_t)(rd->uv+(size_t)(yy>>1)*src->uv_stride),w);
  }
  fr_dsb_sy();
 }
 for(unsigned k=0;k<R;k++){
  unsigned yy=(unsigned)(((uint64_t)k*2u+1u)*h/(2u*R));
  const uint8_t *sr=rd->y+(size_t)yy*src->y_stride;
  if(fr_hash_row(sr,w)!=pre_y[k])f->torn_y|=(uint16_t)(1u<<k);
  const uint8_t *su=rd->uv+(size_t)(yy>>1)*src->uv_stride;
  if(fr_hash_row(su,w)!=pre_uv[k])f->torn_uv|=(uint16_t)(1u<<k);
 }
 s->fr_checked++;if(f->torn_y|f->torn_uv)s->fr_torn++;
}

/* Per-buffer cached la_alias lookup (avoids ~672 AT S1E1R page-walks per frame). */
static LIVE_V70_SEC int lv_cached_alias(struct live_stream *s,const struct la_source *src,uintptr_t *ya,uintptr_t *ua){
 size_t yn=(size_t)src->y_stride*src->height,un=(size_t)src->uv_stride*(src->height/2u);
 uintptr_t sy=(uintptr_t)src->y,suv=(uintptr_t)src->uv;
 struct lv_alias_slot *tab=s->preview_scratch?(struct lv_alias_slot *)align16((uintptr_t)s->preview_scratch+LIVE_PREVIEW_FRAME):0;
 if(tab){
  for(unsigned i=0;i<LV_ALIAS_SLOTS;i++){
   if(tab[i].y==sy&&tab[i].uv==suv&&tab[i].yn==yn&&tab[i].un==un&&tab[i].ya&&tab[i].ua){
    *ya=tab[i].ya;*ua=tab[i].ua;return 1;
   }
  }
 }
 uintptr_t ay=la_alias(sy,yn),au=la_alias(suv,un);
 if(!ay||!au)return 0;
 if(tab){
  unsigned k=0;while(k<LV_ALIAS_SLOTS&&tab[k].y)k++;
  if(k==LV_ALIAS_SLOTS)k=(unsigned)(s->cached_lv_ok%LV_ALIAS_SLOTS);
  tab[k]=(struct lv_alias_slot){sy,suv,yn,un,ay,au};
 }
 *ya=ay;*ua=au;return 1;
}

static LIVE_V70_SEC void fr_push(struct live_stream *s,struct live_fr *f,const struct live_diag *d0){
 const struct live_diag *D=&s->diag;uint8_t r=0;
 if(f->flags&4)r=15;else if(f->flags&1)r=14;else if(D->busy>d0->busy)r=13;else if(D->ok>d0->ok)r=D->accepted_late>d0->accepted_late?2:1;
 else if(D->stale>d0->stale)r=3;else if(D->interval>d0->interval)r=4;else if(D->lease_fail>d0->lease_fail)r=5;
 else if(D->alloc_retry>d0->alloc_retry)r=6;else if(D->geometry>d0->geometry)r=7;else if(D->valid1_fail>d0->valid1_fail)r=8;
 else if(D->convert_fail>d0->convert_fail)r=9;else if(D->cancelled>d0->cancelled)r=10;else if(D->seq_changed>d0->seq_changed)r=11;
 else if(D->valid2_fail>d0->valid2_fail)r=12;
 f->result=r;f->nbufs=(uint8_t)D->nbufs;
 if(!s->fr)return;uint32_t i=s->fr_head%LIVE_FR;s->fr[i]=*f;__atomic_store_n(&s->fr_head,s->fr_head+1,__ATOMIC_RELEASE);
}

#ifdef LIVE_HOST_TEST
/* Default host-test observer bridging legacy c->ops.lease_begin() tests into flip_obs. */
static void host_default_observe(struct la_capture *c,struct live_stream *s,struct live_flip_obs *obs,uint64_t now) {
 if(!c || !s || !obs || !c->ops.lease_begin)return;
 uint64_t seq=LOAD(&c->irq_seq),stamp=LOAD(&c->irq_timestamp);
 if(!seq || (seq&1u) || seq!=LOAD(&c->irq_seq) || stamp>now)return;
 if(seq==obs->obs_seq)return;
 struct live_diag *D=&s->diag;
 if(c->last_generation && seq>c->last_generation+2u){
  uint64_t n=(seq-c->last_generation)/2u-1u;
  c->dropped+=n;s->backlog_dropped+=n;
 }
 c->last_generation=seq;D->new_seq++;
 obs->obs_seq=seq;obs->last_poll_us=now;obs->last_irq_stamp=stamp;
 struct la_source src={0};
 if(!c->ops.lease_begin(c->ops.context,seq/2u,stamp,&src)){
  D->lease_fail++;c->dropped++;s->copy_dropped++;return;
 }
 if(c->ops.lease_end)c->ops.lease_end(c->ops.context,&src);
 uintptr_t sy=(uintptr_t)src.y,suv=(uintptr_t)src.uv;
 if(!sy || !suv)return;
 {
  unsigned k;
  for(k=0;k<D->nbufs && D->bufs[k]!=sy;k++){}
  if(k==D->nbufs){
   if(D->nbufs>=8u)D->nbufs=0;
   D->bufs[D->nbufs++]=sy;
  }
 }
 /* Deduplicate repeated lease_begin return of the same still-unflipped buffer. */
 if(obs->ring_count){
  uint32_t prev_idx=(obs->ring_head+LIVE_FLIP_RING_CAP-1u)%LIVE_FLIP_RING_CAP;
  if(obs->ring[prev_idx].y==sy && obs->ring[prev_idx].lease_epoch==src.lease)return;
 }
 obs->last_s4=0; /* lease_begin already returns a completed frame */
 v70_flip_push(obs,sy,suv,stamp,src.width,src.height,src.y_stride,src.uv_stride,(uint32_t)src.lease,0u);
}
#endif

static void observe_flips(struct la_capture *c,struct live_stream *s,struct live_flip_obs *obs,
 uint64_t now,uint64_t (*clock_us)(void)) {
#ifdef LIVE_HOST_TEST
 (void)clock_us;
 if(host_observer_cb)host_observer_cb(c,s,obs,now);
 else host_default_observe(c,s,obs,now);
#else
 (void)obs;
 la_native_observe_flip(c,s,now,clock_us);
#endif
}

/* V71: Scalar NV12 -> NV12 pre-scaler for rare >1280 or non-4-aligned sources in Mode 2. */
static LIVE_V70_SEC int v71_prescale_nv12_scalar(
 uint8_t *dy,uint8_t *duv,unsigned dys,unsigned duvs,unsigned dw,unsigned dh,
 const struct la_source *s,int cached) {
 if(!dy||!duv||!s||!s->y||!s->uv||!dw||!dh||(dw&3u)||(dh&1u)||
    dys<dw||duvs<dw||!s->width||!s->height||s->width>4096u||s->height>4096u)
  return 0;
 unsigned sw=s->width,sh=s->height,sw_c=sw>>1,sh_c=sh>>1;
 unsigned dw_c=dw>>1,dh_c=dh>>1;
 uint8_t sbuf[4096] __attribute__((aligned(16)));
 uint32_t x_den=dw>1u?dw-1u:1u,y_den=dh>1u?dh-1u:1u;
 for(unsigned y=0;y<dh;y++){
  unsigned iy=(unsigned)(((uint64_t)y*(sh-1u)+(y_den>>1))/y_den);
  const uint8_t *sr=s->y+(size_t)iy*s->y_stride;
  const uint8_t *p=sr;
  if(!cached){burst_copy(sbuf,sr,sw);p=sbuf;}
  uint8_t *out=dy+(size_t)y*dys;
  for(unsigned x=0;x<dw;x++){
   unsigned ix=(unsigned)(((uint64_t)x*(sw-1u)+(x_den>>1))/x_den);
   out[x]=p[ix];
  }
 }
 uint32_t cx_den=dw_c>1u?dw_c-1u:1u,cy_den=dh_c>1u?dh_c-1u:1u;
 for(unsigned y=0;y<dh_c;y++){
  unsigned iy=(unsigned)(((uint64_t)y*(sh_c-1u)+(cy_den>>1))/cy_den);
  const uint8_t *sr=s->uv+(size_t)iy*s->uv_stride;
  const uint8_t *p=sr;
  if(!cached){burst_copy(sbuf,sr,sw);p=sbuf;}
  uint8_t *out=duv+(size_t)y*duvs;
  for(unsigned x=0;x<dw_c;x++){
   unsigned ix=(unsigned)(((uint64_t)x*(sw_c-1u)+(cx_den>>1))/cx_den);
   out[2u*x]=p[2u*ix];
   out[2u*x+1u]=p[2u*ix+1u];
  }
 }
 return 1;
}

/* V71.3 Mode 2 (FULL_CPU2_NV12): Acquire CPU2's latest graded NV12 front slot and
 * submit asynchronously to Wave5. When fgen is unchanged and CPU2 is still busy
 * within LIVE_FILMSIM_CPU2_GRACE_US (22 ms), release and return 0 (no retrig, no
 * cfr_index advance) so worker exits catch-up and polls at 1 ms until CPU2 finishes;
 * past 22 ms or when CPU2 is idle (e.g. shutter blackout), emit 0-copy Wave5 retrig.
 * Returns: 1 = encoded, 0 = warmup / CPU2 grace-wait, LIVE_ENCODER_BUSY = busy, <0 = fatal. */
static LIVE_V70_SEC int v71_filmsim_encode_nv12(
 struct la_capture *c,struct live_stream *s,struct live_flip_obs *obs,
 uint64_t tick,uint64_t now,uint32_t job,int is_repeat_branch,
 uint64_t (*clock_us)(void),struct live_fr *fr_opt) {
 struct live_diag *D=&s->diag;
 const uint8_t *fy=0,*fuv=0;
 unsigned fw=0,fh=0;
 uint64_t fgen=0;
 const int8_t *gtile=0;
 uint64_t tnow=clock_us?clock_us():now;
 if(tnow<now)tnow=now;
 int prev_slot=-1;
 if(!filmsim_runtime_live_acquire_nv12(&fy,&fuv,&fw,&fh,&fgen,&gtile)){
  int fs_busy=(filmsim_runtime_live_is_busy() || filmsim_runtime_live_layer0_feed_active());
  if(!s->ring.count || s->cfr_index==0u){
   if(s->cfr_have){s->cfr_epoch=now;s->cfr_index=0;}
   if(obs && !fs_busy)obs->fs_pad=0u;
   return 0;
  }
  if(tnow<tick+LIVE_FILMSIM_CPU2_GRACE_US && fs_busy)return 0;
  prev_slot=live_encoder_last_valid_slot(&s->encoder);
  if(prev_slot>=0)goto do_retrig;
  return 0;
 }
 /* Same graded frame generation as already sitting cleaned in Wave5's last_valid_slot:
  * grace-wait up to 22 ms while CPU2 (or Layer0->Live feed) is busy, else 0-copy async Wave5 re-trigger! */
 if(obs && fgen && fgen==obs->fs_last_encoded_gen){
  if(tnow<tick+LIVE_FILMSIM_CPU2_GRACE_US &&
     (filmsim_runtime_live_is_busy() || filmsim_runtime_live_layer0_feed_active())){
   filmsim_runtime_live_release();
   return 0;
  }
  prev_slot=live_encoder_last_valid_slot(&s->encoder);
  if(prev_slot>=0){
   filmsim_runtime_live_release();
  do_retrig:
   {
    int hrc=v70_harvest_au(c,s,obs,1,now,clock_us);
    if(hrc==LIVE_ENCODER_BUSY)return LIVE_ENCODER_BUSY;
    if(hrc<0)return hrc;
    uint64_t e0=clock_us?clock_us():0u;
    int rc=live_encoder_submit_slot(&s->encoder,(unsigned)prev_slot,1,tick,&s->last_idr_us,s->bitrate,s->idr_interval_us);
    uint64_t edt=clock_us?(clock_us()-e0):0u;s->encode_last_us=edt;s->encode_total_us+=edt;
    if(edt>s->encode_max_us)s->encode_max_us=edt;
    if(s->encoded)s->encode_avg_us=s->encode_total_us/s->encoded;
    if(fr_opt)fr_opt->enc_us=(uint32_t)edt;
    if(rc==LIVE_ENCODER_BUSY){D->busy++;c->dropped++;s->copy_dropped++;return LIVE_ENCODER_BUSY;}
    if(rc){D->enc_error++;live_log(s,now,EV_ENC_ERR,(uint32_t)-rc,0);s->error=(uint32_t)-rc;STORE(&c->phase,LA_FAILED);return rc;}
    s->cfr_repeats++;if(obs)obs->direct_nv12_retrig++;s->cfr_have=2u;s->cfr_index++;
    return 1;
   }
  }
 }
 /* NEW graded NV12 frame: NEON scale/copy + fused 512x256 grain directly into Wave5 NV12 slot! */
 int fs_can_scale=(fw==1024u && (fh==682u || fh==680u) && s->width==1280u && s->height==852u);
 int fs_can_copy=(fw==s->width && fh==s->height);
 if(fs_can_scale || fs_can_copy){
  unsigned slot=0,dys=0,duvs=0,coded_h=0;uint8_t *dy=0,*duv=0;int slot_cached=0;
  int brc=live_encoder_nv12_begin(&s->encoder,&slot,&dy,&duv,&dys,&duvs,&coded_h,&slot_cached);
  if(brc==LIVE_ENCODER_BUSY){filmsim_runtime_live_release();D->busy++;c->dropped++;s->copy_dropped++;return LIVE_ENCODER_BUSY;}
  if(brc){
   filmsim_runtime_live_release();
   D->enc_error++;live_log(s,now,EV_ENC_ERR,(uint32_t)-brc,0);
   s->error=(uint32_t)-brc;STORE(&c->phase,LA_FAILED);
   return brc;
  }
  uint64_t u0=clock_us?clock_us():0u;
  int valid;
  if(fs_can_scale){
   valid=gtile
    ? live_scale_1024_to_1280_nv12_neon_grain(dy,duv,dys,duvs,coded_h,fy,fuv,fh,fw,fw,1,slot_cached,gtile)
    : live_scale_1024_to_1280_nv12_neon(dy,duv,dys,duvs,coded_h,fy,fuv,fh,fw,fw,1,slot_cached);
  }else{
   valid=gtile
    ? live_copy_nv12_neon_grain(dy,duv,s->width,s->height,dys,duvs,coded_h,fy,fuv,fw,fw,1,slot_cached,gtile)
    : live_copy_nv12_neon(dy,duv,s->width,s->height,dys,duvs,coded_h,fy,fuv,fw,fw,1,slot_cached);
  }
  uint64_t u_dt=clock_us?(clock_us()-u0):0u;
  D->sum_convert_us+=u_dt;if(!D->n_convert)D->n_convert=1u;
  if(fr_opt){
   fr_opt->conv_us+=(uint32_t)u_dt;
   D->last_convert_us=fr_opt->conv_us;
  }else{
   D->last_convert_us=u_dt;
  }
  if(D->last_convert_us>D->max_convert_us)D->max_convert_us=D->last_convert_us;
  int prev_from_lbuf=0;
  if(valid && job<=1u && fw==LIVE_PREVIEW_W && fh==LIVE_PREVIEW_H){
   preview_push_lbuf_nv12(s,tick,fy,fuv,fw,fh);
   prev_from_lbuf=1;
  }
  filmsim_runtime_live_release();
  if(!valid){D->convert_fail++;c->dropped++;s->copy_dropped++;return 0;}
  int hrc=v70_harvest_au(c,s,obs,1,now,clock_us);
  if(hrc==LIVE_ENCODER_BUSY)return LIVE_ENCODER_BUSY;
  if(hrc<0)return hrc;
  uint64_t e0=clock_us?clock_us():0u;
  int rc=live_encoder_submit_slot(&s->encoder,slot,slot_cached,tick,&s->last_idr_us,s->bitrate,s->idr_interval_us);
  uint64_t edt=clock_us?(clock_us()-e0):0u;s->encode_last_us=edt;s->encode_total_us+=edt;
  if(edt>s->encode_max_us)s->encode_max_us=edt;
  if(s->encoded)s->encode_avg_us=s->encode_total_us/s->encoded;
  if(fr_opt)fr_opt->enc_us=(uint32_t)edt;
  if(rc==LIVE_ENCODER_BUSY){D->busy++;c->dropped++;s->copy_dropped++;return LIVE_ENCODER_BUSY;}
  if(rc){D->enc_error++;live_log(s,now,EV_ENC_ERR,(uint32_t)-rc,0);s->error=(uint32_t)-rc;STORE(&c->phase,LA_FAILED);return rc;}
  if(obs){obs->fs_last_encoded_gen=fgen;obs->direct_nv12_ok++;}
  if(is_repeat_branch)s->cfr_repeats++;
  s->cfr_have=2u;s->cfr_index++;
  if(job<=1u && !prev_from_lbuf)preview_push_nv12(s,tick,slot);
  return 1;
 }
 /* Rare non-standard aspect fallback: convert graded NV12 -> planar I420 and encode sync. */
 uint64_t u0=clock_us?clock_us():0u;
 struct la_source fsrc={fy,fuv,fw,fh,fw,fw,fgen,now,0u};
 int cv_ok=convert_scaled_ex(s->i420,s->width,s->height,&fsrc,1);
 uint64_t u_dt=clock_us?(clock_us()-u0):0u;
 D->sum_convert_us+=u_dt;if(!D->n_convert)D->n_convert=1u;
 if(fr_opt){
  fr_opt->conv_us+=(uint32_t)u_dt;
  D->last_convert_us=fr_opt->conv_us;
 }else{
  D->last_convert_us=u_dt;
 }
 if(D->last_convert_us>D->max_convert_us)D->max_convert_us=D->last_convert_us;
 filmsim_runtime_live_release();
 if(!cv_ok){D->convert_fail++;c->dropped++;s->copy_dropped++;return 0;}
 int hrc=v70_harvest_au(c,s,obs,1,now,clock_us);
 if(hrc==LIVE_ENCODER_BUSY)return LIVE_ENCODER_BUSY;
 if(hrc<0)return hrc;
 uint64_t e0=clock_us?clock_us():0u;const uint8_t *au=0;size_t n=0;unsigned idr=0;
 int rc=live_encoder_frame_timed(&s->encoder,s->i420,tick,&s->last_idr_us,s->bitrate,s->idr_interval_us,&au,&n,&idr);
 uint64_t edt=clock_us?(clock_us()-e0):0u;s->encode_last_us=edt;
 if(fr_opt)fr_opt->enc_us=(uint32_t)edt;
 if(rc==LIVE_ENCODER_BUSY){D->busy++;c->dropped++;s->copy_dropped++;return LIVE_ENCODER_BUSY;}
 if(rc){D->enc_error++;live_log(s,now,EV_ENC_ERR,(uint32_t)-rc,0);s->error=(uint32_t)-rc;STORE(&c->phase,LA_FAILED);return rc;}
 if(is_repeat_branch)s->cfr_repeats++;
 s->encode_total_us+=edt;if(edt>s->encode_max_us)s->encode_max_us=edt;
 s->cfr_index++;
 if(v70_push_au(c,s,tick,au,n,idr,now)<0)return -1;
 if(s->encoded)s->encode_avg_us=s->encode_total_us/s->encoded;
 if(obs)obs->fs_last_encoded_gen=fgen;
 s->cfr_have=2u;
 if(job<=1u)preview_push(s,tick,s->i420);
 return 1;
}

/* Kept in .text for stage1 FilmSim & V71 Native-NV12 hook verification. */
void live_stream_step(struct la_capture *c,struct live_stream *s,uint64_t now,int eligible,uint64_t (*clock_us)(void)) {
 struct live_fr fr;int fr_on=0;struct live_diag d0;
 struct live_flip_obs *obs=stream_flip_obs(s);
 uint32_t epoch=LOAD(&c->cancel_epoch),job=LOAD(&s->job);
 s->diag.magic=0x3147444cu;s->diag.version=10;s->diag.steps++;if(!eligible)s->diag.ineligible++;
 if(eligible && LOAD(&c->requested) && epoch!=c->worker_epoch && s->tune.soft_cancel) {
  /* C0 start/stop/hold and the still pool cancel at every shutter: the live-view source is
   * void, but encoded AUs are our own memory. Full picker_reset prevents stale pre-blackout
   * trans[] picks and mixed-pool nbufs inflation after C0 restarts. */
  c->worker_epoch=epoch;s->diag.soft_cancel++;live_log(s,now,EV_SOFT_CANCEL,epoch,job);
  picker_reset(c,s,obs);
 }
 if(!eligible || !LOAD(&c->requested) || epoch!=c->worker_epoch) {
  if(!eligible && epoch==c->worker_epoch)epoch=__atomic_add_fetch(&c->cancel_epoch,1,__ATOMIC_ACQ_REL);
  /* Mux reads frozen storage: cancellation first, release after its ACK. */
  if(job==2||job==3)return;
  if(c->descriptor||job)live_log(s,now,EV_RELEASE,eligible|LOAD(&c->requested)<<1,job);
  release(c,s,obs);job=0;c->worker_epoch=epoch;
  if(!eligible||!LOAD(&c->requested))return;
 }
 if(job==4){cfr_reset(c,s,obs);STORE(&s->job,0);job=0;STORE(&c->phase,LA_REQUESTED);}
 if(c->phase==LA_FAILED)return;
 if(!job && s->cfr_have && now>s->cfr_source_us+LIVE_RESUME_GAP_US){
  live_log(s,now,EV_RESUME,s->ring.count,now-s->cfr_source_us);cfr_reset(c,s,obs);
 }
 /* V70 Component 2: Non-blocking harvest of any completed 1-deep async Wave5 AU
  * right at step entry so au_ring is maximally fresh before press/freeze checks. */
 if(c->descriptor && job<=1u){
  int hrc=v70_harvest_au(c,s,obs,0,now,clock_us);
  if(hrc<0 && hrc!=LIVE_ENCODER_BUSY)return;
 }
 uint64_t press=__atomic_exchange_n(&c->press_mailbox,0,__ATOMIC_ACQ_REL);
 if(press){
  unsigned start;
  uint32_t W=s->tune.window_us;
  int found=!job && press>=W && !au_ring_find_start(&s->ring,press-W,&start);
  if(found)live_log(s,now,EV_ACCEPT,start,press);
  if(!job && !found && s->tune.short_start){
   /* No IDR 1.5 s back (slow encode or a young ring): start at the earliest IDR before the press. */
   for(unsigned i=0;i<s->ring.count;i++){const struct au_entry *e=&s->ring.entries[(s->ring.head+i)%AU_RING_SLOTS];
    if(e->pts>press)break;if(e->idr){start=i;found=1;s->diag.short_start++;live_log(s,now,EV_SHORT,i,press);break;}}
  }
  if(found){
   s->clip_start=s->ring.entries[(s->ring.head+start)%AU_RING_SLOTS].pts;
   s->clip_press=press;
   /* Audio freezes this exact same window. At most one frame is added at the
    * post-roll edge; no shortened last video sample changes native timing. */
   uint64_t ticks=((press+W-s->cfr_epoch)*30u+1000999u)/1001000u;
   s->clip_end=cfr_time(s,ticks);c->press_us=press;c->accepted_presses++;
   job=1;STORE(&s->job,1);STORE(&c->phase,LA_POST);
  }else{s->ignored_presses++;c->rejected_presses++;live_log(s,now,EV_IGNORED,s->ring.count|job<<16,press);}
 }
 if(job==2||job==3){
  /* v47: mux shares this CPU; consume C0 sequence without polling MMIO or encoding. */
  uint64_t seq=LOAD(&c->irq_seq),stamp=LOAD(&c->irq_timestamp);
  if(!(seq&1u) && seq==LOAD(&c->irq_seq) && seq!=c->last_generation){
   if(c->last_generation==0||s->last_pts==0||stamp>s->last_pts)c->last_generation=seq;
  }
  return;
 }
 /* V70 Component 1: Observe C0 buffer flips on EVERY worker step (task context ONLY,
  * NEVER blocked by stamp <= tick + 20000u or sample_us = 28000u). */
 observe_flips(c,s,obs,now,clock_us);

 /* Phase-lock cfr_epoch = first_flip_us + 2000us on startup / cfr_reset. */
 if(!s->cfr_have){
  if(s->cfr_epoch && now>s->cfr_epoch+LIVE_RESUME_GAP_US)s->cfr_epoch=0;
  if(!s->cfr_epoch){
   const struct live_flip_entry *first=v70_peek_first_flip(s,obs,epoch,now);
   if(first){
    s->cfr_epoch=(first->flip_us?first->flip_us:now)+2000u;
    s->cfr_index=0;
   }
  }
 }

 /* V70/V71 Component 2: Tick-Driven Closest-Flip CFR Sampler (argmin |h.flip_us - tick|)
  * + Direct NEON NV12 (Mode 0/1 w/ Fused Grain) + Native-NV12 CPU2 Triple-Buffer (Mode 2)
  * + 1-Deep Async Wave5 Pipeline. */
 uint64_t tick=cfr_time(s,s->cfr_index);
 int submitted=0;
 int fs_mode=filmsim_runtime_live_mode();
 if(obs && (uint32_t)fs_mode!=obs->fs_last_mode){
  obs->fs_last_encoded_gen=0u;
  obs->fs_pad=0u;
  obs->fs_last_mode=(uint32_t)fs_mode;
 }
 uint32_t fs_tick_tag=(uint32_t)s->cfr_index+1u;
 if(!fs_tick_tag)fs_tick_tag=1u;
 int fs_m2_submitted=(fs_mode==FILMSIM_LIVE_MODE_FULL_CPU2 && obs && obs->fs_pad==fs_tick_tag);
 struct live_diag *D=&s->diag;
 if(s->cfr_epoch && tick<=now && (!job || tick<s->clip_end) && !la_cancelled(c)){
  struct live_flip_entry *cand=0;
  if(!fs_m2_submitted){
   while((cand=v70_pick_candidate(s,obs,tick,epoch,now))!=0 ||
         (fs_mode==FILMSIM_LIVE_MODE_FULL_CPU2 &&
          (cand=v70_pick_newest_safe(s,obs,tick,epoch,now))!=0)){
   if(!s->cfr_have && s->cfr_index==0u && cand->flip_us>tick && cand->flip_us-tick>16667u && cand->flip_us<=now){
    s->cfr_epoch=cand->flip_us;tick=cand->flip_us;
   }
   uint64_t seq_before=LOAD(&c->irq_seq);
   fr_on=1;d0=*D;memset(&fr,0,sizeof fr);fr.t=now;fr.seq=seq_before;fr.picker=(uint8_t)s->tune.picker;
   fr.y=cand->y;
   {uint32_t sh=s->snap_head;if(sh){fr.s4=s->snap[(sh-1u)%16u].s4;fr.s20=s->snap[(sh-1u)%16u].s20;}}
   D->y=cand->y;D->uv=cand->uv;D->width=cand->width;D->height=cand->height;
   D->y_stride=cand->y_stride;D->uv_stride=cand->uv_stride;D->version2=3;

   unsigned fit_w,fit_h;live_fit(cand->width,cand->height,&fit_w,&fit_h);
   if(!c->descriptor){
    /* V71 -24 Quiet-Park: never call allocate()/live_encoder_init() while Wave5 is parked. */
    if(live_encoder_is_parked(&s->encoder)){
     D->enc_error++;live_log(s,now,EV_ENC_ERR,24u,0);
     s->error=24u;STORE(&c->phase,LA_FAILED);return;
    }
    if(!allocate(c,s,obs,fit_w,fit_h)){
     c->reason=LA_ALLOC;D->alloc_retry++;if(D->alloc_retry<64u)live_log(s,now,EV_ALLOC,0,0);c->dropped++;goto done;
    }
   }
   int scaled=(cand->width!=s->width || cand->height!=s->height);
   if(scaled && (fit_w!=s->width || (fit_h>s->height?fit_h-s->height:s->height-fit_h)>8u)){
    D->geometry++;D->geometry_skip++;s->geometry_changes++;picker_reset(c,s,obs);c->dropped++;
    if(!D->geometry_since||job)live_log(s,now,EV_GEOMETRY,(cand->width<<16)|cand->height,job);
    if(!job){
     if(!D->geometry_since)D->geometry_since=now?now:1u;
     else if(now-D->geometry_since>s->tune.geometry_reset_us){D->geometry_since=0;release(c,s,obs);return;}
    }
    goto done;
   }
   D->geometry_since=0;

   struct la_source source={
    (const uint8_t *)(uintptr_t)cand->y,(const uint8_t *)(uintptr_t)cand->uv,
    cand->width,cand->height,cand->y_stride,cand->uv_stride,
    cand->flip_seq,cand->flip_us,cand->lease_epoch
   };
   struct la_source src2=source;
   int fs_l0_feed=(fs_mode==FILMSIM_LIVE_MODE_FULL_CPU2 && filmsim_runtime_live_layer0_feed_active());
   if(s->tune.cached_lv && !fs_l0_feed){
    uintptr_t ya=0,ua=0;
    if(lv_cached_alias(s,&source,&ya,&ua)){
     fr_dc_ivac_nb(ya,(size_t)source.y_stride*source.height);
     fr_dc_ivac_nb(ua,(size_t)source.uv_stride*(source.height/2u));
     fr_dsb_sy();
     src2.y=(const uint8_t *)ya;src2.uv=(const uint8_t *)ua;s->cached_lv_ok++;
    }else s->cached_lv_fail++;
   }
   int lv_cached=(src2.y!=source.y);
   int can_direct_scale=(scaled && source.width==1024u && (source.height==682u || source.height==680u) &&
                         s->width==1280u && s->height==852u);
   int can_direct_nv12=(fs_mode!=FILMSIM_LIVE_MODE_FULL_CPU2 && (!scaled || can_direct_scale));

   live_encoder_set_flags(&s->encoder,(s->tune.cached_slot?2u:0u)|(s->tune.cached_bs?4u:0u));

   if(can_direct_nv12){
    /* Mode 0 (Normal Live) & Mode 1 (Grain-Only CPU0): Direct-NV12 async path directly
     * into Wave5's free NV12 slot with optional fused 512x256 twisted-torus grain! */
    unsigned slot=0,dys=0,duvs=0,coded_h=0;uint8_t *dy=0,*duv=0;int slot_cached=0;
    int brc=live_encoder_nv12_begin(&s->encoder,&slot,&dy,&duv,&dys,&duvs,&coded_h,&slot_cached);
    if(brc==LIVE_ENCODER_BUSY){D->busy++;c->dropped++;s->copy_dropped++;goto done;}
    if(brc){
     D->enc_error++;live_log(s,now,EV_ENC_ERR,(uint32_t)-brc,0);
     s->error=(uint32_t)-brc;STORE(&c->phase,LA_FAILED);return;
    }
    const int8_t *gtile=(fs_mode==FILMSIM_LIVE_MODE_GRAIN_ONLY)
     ? filmsim_runtime_live_acquire_grain_tile() : 0;
    uint64_t pre_y[16],pre_uv[16];
    unsigned fr_r=fr_pre_hash(s,&source,&src2,pre_y,pre_uv);
    uint64_t t2=clock_us();
    int valid;
    if(scaled){
     valid=gtile
      ? live_scale_1024_to_1280_nv12_neon_grain(dy,duv,dys,duvs,coded_h,src2.y,src2.uv,
                                                src2.height,src2.y_stride,src2.uv_stride,lv_cached,slot_cached,gtile)
      : live_scale_1024_to_1280_nv12_neon(dy,duv,dys,duvs,coded_h,src2.y,src2.uv,
                                          src2.height,src2.y_stride,src2.uv_stride,lv_cached,slot_cached);
    }else{
     valid=gtile
      ? live_copy_nv12_neon_grain(dy,duv,s->width,s->height,dys,duvs,coded_h,
                                  src2.y,src2.uv,src2.y_stride,src2.uv_stride,lv_cached,slot_cached,gtile)
      : live_copy_nv12_neon(dy,duv,s->width,s->height,dys,duvs,coded_h,
                            src2.y,src2.uv,src2.y_stride,src2.uv_stride,lv_cached,slot_cached);
    }
    if(gtile)filmsim_runtime_live_release();
    uint64_t t3=clock_us();
    D->last_convert_us=t3-t2;if(D->last_convert_us>D->max_convert_us)D->max_convert_us=D->last_convert_us;
    D->sum_convert_us+=t3-t2;D->n_convert++;
    uint64_t after=LOAD(&c->irq_seq);
    D->last_frames_during_copy=(after>seq_before)?(after-seq_before)/2u:0u;
    if(D->last_frames_during_copy>D->max_frames_during_copy)D->max_frames_during_copy=D->last_frames_during_copy;
    fr.after=after;fr.conv_us=(uint32_t)(t3-t2);
    if(valid && fr_r)fr_post_check(s,&source,&src2,fr_r,pre_y,pre_uv,&fr);
    if(!valid){D->convert_fail++;cand->flags|=LIVE_FLIP_FLAG_CONSUMED;c->dropped++;s->copy_dropped++;fr_push(s,&fr,&d0);fr_on=0;continue;}
    if(la_cancelled(c)){D->cancelled++;c->dropped++;s->copy_dropped++;goto done;}
    if(s->fr_drop && (fr.torn_y|fr.torn_uv)){
     s->fr_dropped++;fr.flags|=1u;cand->flags|=LIVE_FLIP_FLAG_CONSUMED;c->dropped++;s->copy_dropped++;
     fr_push(s,&fr,&d0);fr_on=0;
     continue; /* Try next safe untorn flip_ring entry! */
    }
    if(D->last_frames_during_copy>0u)D->accepted_late++;
    /* Drain any still-in-flight Wave5 frame synchronously before submitting the new slot. */
    int hrc=v70_harvest_au(c,s,obs,1,now,clock_us);
    if(hrc==LIVE_ENCODER_BUSY)goto done;
    if(hrc<0)return;
    uint64_t e0=clock_us();
    int rc=live_encoder_submit_slot(&s->encoder,slot,slot_cached,tick,&s->last_idr_us,s->bitrate,s->idr_interval_us);
    uint64_t edt=clock_us()-e0;s->encode_last_us=edt;s->encode_total_us+=edt;
    if(edt>s->encode_max_us)s->encode_max_us=edt;
    if(s->encoded)s->encode_avg_us=s->encode_total_us/s->encoded;
    fr.enc_us=(uint32_t)edt;
    if(rc==LIVE_ENCODER_BUSY){D->busy++;c->dropped++;s->copy_dropped++;goto done;}
    if(rc){
     D->enc_error++;live_log(s,now,EV_ENC_ERR,(uint32_t)-rc,0);
     s->error=(uint32_t)-rc;STORE(&c->phase,LA_FAILED);return;
    }
    cand->flags|=LIVE_FLIP_FLAG_CONSUMED;
    obs->last_kept_flip_seq=cand->flip_seq;
    obs->direct_nv12_ok++;
    s->last_kept_y=cand->y;s->cfr_source_us=cand->flip_us;s->cfr_have=2u;s->cfr_index++;
    D->ok++;c->copied++;
    if(job<=1u)preview_push_nv12(s,tick,slot);
    submitted=1;
    break;
   }

   if(fs_mode==FILMSIM_LIVE_MODE_FULL_CPU2){
    if(fs_l0_feed){
     /* V71.7-r7 Single-Pass Layer0->Live feed: Layer 0 already pushed native LUT+HDF
      * NV12 (1280x852 EVF or 1024x682 LCD) directly into LBUF as GRADED_READY.
      * Skip CPU0 downscale/copy and CPU2 duplicate LUT/HDF completely! */
     cand->flags|=LIVE_FLIP_FLAG_CONSUMED;
     obs->last_kept_flip_seq=cand->flip_seq;
     obs->fs_pad=fs_tick_tag;
     s->last_kept_y=cand->y;s->cfr_source_us=cand->flip_us;
     D->ok++;c->copied++;
    }else{
     /* V71r2 Mode 2 (FULL_CPU2_NV12): On EVF 1280x852, NEON 5:4 downscale directly into
      * 1024x682 LBUF[wslot] so CPU2 ALWAYS hits fast z8_hdf_live_nv12_1024() (LIVE_256_YC)
      * and cuts HDF + Fuji17^3 pixels & LBUF writes by 36%; CPU0 upscales + fuses crisp
      * 1280x852 grain into Wave5 via live_scale_1024_to_1280_nv12_neon[_grain]! */
     int evf_downscale=(source.width==1280u && source.height==852u &&
                        source.y_stride==source.uv_stride && source.y_stride>=1280u);
     int native_nv12=!evf_downscale && ((source.width&3u)==0u && (source.height&1u)==0u &&
                      source.width<=1280u && source.height<=864u);
     unsigned sub_w=evf_downscale?1024u:(native_nv12?source.width:(fit_w&~3u));
     unsigned sub_h=evf_downscale?682u:(native_nv12?source.height:(fit_h&~1u));
     if(sub_w<4u)sub_w=4u;
     if(sub_h<2u)sub_h=2u;
     uint8_t *ly=0,*luv=0;unsigned lys=0,luvs=0;
     int wslot=filmsim_runtime_live_begin_submit_nv12(sub_w,sub_h,&ly,&luv,&lys,&luvs);
     if(wslot>=0){
      uint64_t pre_y[16],pre_uv[16];
      unsigned fr_r=fr_pre_hash(s,&source,&src2,pre_y,pre_uv);
      uint64_t t2=clock_us();
      int valid;
      if(evf_downscale && lys==luvs && lys>=1024u){
       live_downscale_1280_to_1024_nv12_neon(src2.y,src2.uv,src2.y_stride,ly,luv,lys);
       valid=1;
      }else if(native_nv12){
       valid=live_copy_nv12_neon(ly,luv,sub_w,sub_h,lys,luvs,sub_h,
                                 src2.y,src2.uv,src2.y_stride,src2.uv_stride,lv_cached,0);
      }else{
       valid=v71_prescale_nv12_scalar(ly,luv,lys,luvs,sub_w,sub_h,&src2,lv_cached);
      }
      uint64_t t3=clock_us();
      D->last_convert_us=t3-t2;if(D->last_convert_us>D->max_convert_us)D->max_convert_us=D->last_convert_us;
      D->sum_convert_us+=t3-t2;D->n_convert++;
      uint64_t after=LOAD(&c->irq_seq);
      D->last_frames_during_copy=(after>seq_before)?(after-seq_before)/2u:0u;
      if(D->last_frames_during_copy>D->max_frames_during_copy)D->max_frames_during_copy=D->last_frames_during_copy;
      fr.after=after;fr.conv_us=(uint32_t)(t3-t2);
      if(valid && fr_r)fr_post_check(s,&source,&src2,fr_r,pre_y,pre_uv,&fr);
      if(!valid){
       filmsim_runtime_live_commit_submit_nv12(wslot,sub_w,sub_h,cand->flip_us,0);
       D->convert_fail++;cand->flags|=LIVE_FLIP_FLAG_CONSUMED;c->dropped++;s->copy_dropped++;
       fr_push(s,&fr,&d0);fr_on=0;continue;
      }
      if(la_cancelled(c)){
       filmsim_runtime_live_commit_submit_nv12(wslot,sub_w,sub_h,cand->flip_us,0);
       D->cancelled++;c->dropped++;s->copy_dropped++;goto done;
      }
      if(s->fr_drop && (fr.torn_y|fr.torn_uv)){
       filmsim_runtime_live_commit_submit_nv12(wslot,sub_w,sub_h,cand->flip_us,0);
       s->fr_dropped++;fr.flags|=1u;cand->flags|=LIVE_FLIP_FLAG_CONSUMED;c->dropped++;s->copy_dropped++;
       fr_push(s,&fr,&d0);fr_on=0;continue;
      }
      if(D->last_frames_during_copy>0u)D->accepted_late++;
      filmsim_runtime_live_commit_submit_nv12(wslot,sub_w,sub_h,cand->flip_us,1);
      cand->flags|=LIVE_FLIP_FLAG_CONSUMED;
      obs->last_kept_flip_seq=cand->flip_seq;
      obs->fs_pad=fs_tick_tag;
      s->last_kept_y=cand->y;s->cfr_source_us=cand->flip_us;
      D->ok++;c->copied++;
      {uint64_t ot=clock_us?clock_us():now;if(ot<now)ot=now;observe_flips(c,s,obs,ot,clock_us);}
     }
    }
    int frc=v71_filmsim_encode_nv12(c,s,obs,tick,now,job,0,clock_us,&fr);
    if(frc==1){uint64_t ot=clock_us?clock_us():now;if(ot<now)ot=now;observe_flips(c,s,obs,ot,clock_us);}
    if(frc==LIVE_ENCODER_BUSY){if(!s->cfr_have)s->cfr_have=1u;goto done;}
    if(frc<0)return;
    if(frc==0 && (!s->ring.count || s->cfr_index==0u)){
     s->cfr_have=1u;s->cfr_epoch=now;s->cfr_index=0;
    }
    submitted=1;
    break;
   }

   /* Rare unusual non-1024/non-1280 aspect fallback in Mode 0/1 -> planar I420. */
   uint8_t *kept=s->i420;
   s->i420=s->cfr_spare;
   uint64_t pre_y[16],pre_uv[16];
   unsigned fr_r=fr_pre_hash(s,&source,&src2,pre_y,pre_uv);
   uint64_t t2=clock_us();
   int valid=can_direct_scale
    ? live_scale_1024_to_1280_i420_neon(s->i420,src2.y,src2.uv,src2.height,src2.y_stride,src2.uv_stride,lv_cached)
    : (scaled ? convert_scaled_ex(s->i420,s->width,s->height,&src2,lv_cached) : convert_ex(s->i420,&src2,lv_cached));
   uint64_t t3=clock_us();
   D->last_convert_us=t3-t2;if(D->last_convert_us>D->max_convert_us)D->max_convert_us=D->last_convert_us;
   D->sum_convert_us+=t3-t2;D->n_convert++;
   uint64_t after=LOAD(&c->irq_seq);
   D->last_frames_during_copy=(after>seq_before)?(after-seq_before)/2u:0u;
   if(D->last_frames_during_copy>D->max_frames_during_copy)D->max_frames_during_copy=D->last_frames_during_copy;
   fr.after=after;fr.conv_us=(uint32_t)(t3-t2);
   if(valid && fr_r)fr_post_check(s,&source,&src2,fr_r,pre_y,pre_uv,&fr);
   if(!valid){s->i420=kept;D->convert_fail++;cand->flags|=LIVE_FLIP_FLAG_CONSUMED;c->dropped++;s->copy_dropped++;fr_push(s,&fr,&d0);fr_on=0;continue;}
   if(la_cancelled(c)){s->i420=kept;D->cancelled++;c->dropped++;s->copy_dropped++;goto done;}
   if(s->fr_drop && (fr.torn_y|fr.torn_uv)){
    s->i420=kept;s->fr_dropped++;fr.flags|=1u;cand->flags|=LIVE_FLIP_FLAG_CONSUMED;c->dropped++;s->copy_dropped++;
    fr_push(s,&fr,&d0);fr_on=0;
    continue;
   }
   if(D->last_frames_during_copy>0u)D->accepted_late++;
   s->cfr_spare=kept;
   cand->flags|=LIVE_FLIP_FLAG_CONSUMED;
   obs->last_kept_flip_seq=cand->flip_seq;
   s->last_kept_y=cand->y;s->cfr_source_us=cand->flip_us;
   D->ok++;c->copied++;
   int hrc=v70_harvest_au(c,s,obs,1,now,clock_us);
   if(hrc==LIVE_ENCODER_BUSY){s->cfr_have=1u;goto done;}
   if(hrc<0)return;
   uint64_t e0=clock_us();const uint8_t *au=0;size_t n=0;unsigned idr=0;
   int rc=live_encoder_frame_timed(&s->encoder,s->i420,tick,&s->last_idr_us,s->bitrate,s->idr_interval_us,&au,&n,&idr);
   uint64_t edt=clock_us()-e0;s->encode_last_us=edt;fr.enc_us=(uint32_t)edt;
   if(rc==LIVE_ENCODER_BUSY){D->busy++;c->dropped++;s->copy_dropped++;s->cfr_have=1u;goto done;}
   if(rc){D->enc_error++;live_log(s,now,EV_ENC_ERR,(uint32_t)-rc,0);s->error=(uint32_t)-rc;STORE(&c->phase,LA_FAILED);return;}
   s->encode_total_us+=edt;if(edt>s->encode_max_us)s->encode_max_us=edt;
   s->cfr_index++;
   if(v70_push_au(c,s,tick,au,n,idr,now)<0)return;
   if(s->encoded)s->encode_avg_us=s->encode_total_us/s->encoded;
   s->cfr_have=2u;
   if(job<=1u)preview_push(s,tick,s->i420);
   submitted=1;
   break;
  }
  }

  /* V70/V71.3 Component 3: Mode-2 CPU2 22 ms Grace Poll + Non-Cascading 28 ms Repeat Grace. */
  if(!submitted && tick<=now && (!job || tick<s->clip_end) && !la_cancelled(c) &&
     (fs_m2_submitted ||
      (s->cfr_have && (s->cfr_have==1u || now>=tick+LIVE_CFR_REPEAT_GRACE_US)))){
   int is_repeat=(!fs_m2_submitted && s->cfr_have==2u);
   live_encoder_set_flags(&s->encoder,(s->tune.cached_slot?2u:0u)|(s->tune.cached_bs?4u:0u));
   if(fs_mode==FILMSIM_LIVE_MODE_FULL_CPU2){
    int frc=v71_filmsim_encode_nv12(c,s,obs,tick,now,job,is_repeat,clock_us,0);
    if(frc==1){uint64_t ot=clock_us?clock_us():now;if(ot<now)ot=now;observe_flips(c,s,obs,ot,clock_us);}
    if(frc<0 && frc!=LIVE_ENCODER_BUSY)return;
    goto done;
   }
   int hrc=v70_harvest_au(c,s,obs,1,now,clock_us);
   if(hrc==LIVE_ENCODER_BUSY)goto done;
   if(hrc<0)return;
   if(is_repeat){
    int prev_slot=live_encoder_last_valid_slot(&s->encoder);
    if(prev_slot>=0){
     /* 0-copy, 0-dc cvac async re-trigger of previous Wave5 NV12 slot (Mode 0 & Mode 1)! */
     uint64_t e0=clock_us();
     int rc=live_encoder_submit_slot(&s->encoder,(unsigned)prev_slot,1,tick,&s->last_idr_us,s->bitrate,s->idr_interval_us);
     uint64_t edt=clock_us()-e0;s->encode_last_us=edt;s->encode_total_us+=edt;
     if(edt>s->encode_max_us)s->encode_max_us=edt;
     if(s->encoded)s->encode_avg_us=s->encode_total_us/s->encoded;
     if(rc==LIVE_ENCODER_BUSY){D->busy++;c->dropped++;s->copy_dropped++;goto done;}
     if(rc){D->enc_error++;live_log(s,now,EV_ENC_ERR,(uint32_t)-rc,0);s->error=(uint32_t)-rc;STORE(&c->phase,LA_FAILED);return;}
     s->cfr_repeats++;obs->direct_nv12_retrig++;s->cfr_index++;
     goto done;
    }
   }
   /* Fallback when no valid NV12 slot yet (unusual non-3:2 planar I420 path). */
   if(!s->i420)goto done;
   uint64_t e0=clock_us();const uint8_t *au=0;size_t n=0;unsigned idr=0;
   int rc=live_encoder_frame_timed(&s->encoder,s->i420,tick,&s->last_idr_us,s->bitrate,s->idr_interval_us,&au,&n,&idr);
   uint64_t edt=clock_us()-e0;s->encode_last_us=edt;
   if(rc==LIVE_ENCODER_BUSY){D->busy++;c->dropped++;s->copy_dropped++;goto done;}
   if(rc){D->enc_error++;live_log(s,now,EV_ENC_ERR,(uint32_t)-rc,0);s->error=(uint32_t)-rc;STORE(&c->phase,LA_FAILED);return;}
   if(is_repeat)s->cfr_repeats++;
   s->encode_total_us+=edt;if(edt>s->encode_max_us)s->encode_max_us=edt;
   s->cfr_index++;
   if(v70_push_au(c,s,tick,au,n,idr,now)<0)return;
   if(s->encoded)s->encode_avg_us=s->encode_total_us/s->encoded;
   s->cfr_have=2u;
   if(job<=1u)preview_push(s,tick,s->i420);
  }
 }
 done:
 if(fr_on)fr_push(s,&fr,&d0);
 if(job==1 && now>=s->clip_end && cfr_time(s,s->cfr_index)>=s->clip_end){
  freeze(c,s,obs,now);
  live_log(s,now,LOAD(&s->job)==2?EV_FREEZE:EV_FREEZE_FAIL,s->frozen_count,s->frozen_size|(uint64_t)s->error<<32);
 }
 else if(job==1 && now>s->clip_end+LIVE_RESUME_GAP_US){
  /* A persistent busy encoder must not produce a truncated, retimed clip. */
  s->error=LA_OVERFLOW;STORE(&s->job,0);cfr_reset(c,s,obs);STORE(&c->phase,LA_REQUESTED);
 }
 else if(!job){unsigned start;
  int fresh=s->cfr_have && s->last_pts && now>=s->cfr_source_us && now>=s->last_pts &&
            now-s->cfr_source_us<=s->tune.ready_fresh_us && now-s->last_pts<=s->tune.ready_fresh_us;
  if(now>=s->tune.window_us && fresh &&
    !au_ring_find_start(&s->ring,now-s->tune.window_us,&start)){if(c->phase!=LA_READY)live_log(s,now,EV_READY,s->ring.count,0);STORE(&c->phase,LA_READY);}
  else if(s->tune.short_start && now>=LIVE_WARM_US && fresh && !au_ring_find_start(&s->ring,now-LIVE_WARM_US,&start)){
   if(c->phase!=LA_WARM)live_log(s,now,EV_WARM,s->ring.count,0);STORE(&c->phase,LA_WARM);}
  else STORE(&c->phase,LA_REQUESTED);
 }
}

uint32_t live_stream_progress(const struct la_capture *c,const struct live_stream *s,uint64_t now) {
 uint32_t p=LOAD(&c->phase);
 if(p==LA_READY||p==LA_WARM||(p>=LA_POST&&p<=LA_ENCODING))return 12u;
 uint64_t goal=s->tune.short_start?LIVE_WARM_US:s->tune.window_us;
 if(!s->last_pts||now<s->last_pts||now-s->last_pts>s->tune.ready_fresh_us||!goal)return 0u;
 for(unsigned i=0;i<s->ring.count;i++){const struct au_entry *e=&s->ring.entries[(s->ring.head+i)%AU_RING_SLOTS];
  if(e->idr){if(now<=e->pts)return 0u;uint64_t n=(now-e->pts)*12u/goal;return n>11u?11u:(uint32_t)n;}}
 return 0u;
}
