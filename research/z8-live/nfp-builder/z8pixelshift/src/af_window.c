/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Immutable raw rows are kept until both neighbours and the asymmetric NX
   first-slot guard have been consumed. Corrections are written to a separate
   output row, never back into the ring. No frame-sized allocation. */
#include "af_window.h"
#include "afrow.h"
#ifdef PSM_FIRMWARE
#define AF_BATCH ((void (*)(const uint16_t *,uint16_t *,uint32_t))(uintptr_t)0x409D8000u)
#else
#define AF_BATCH ps8_af_one_batch
#endif

size_t ps8_af_window_bytes(const psm_plan *p)
{
    if (!p || p->width > 2u*PSM_MAX_SENSOR || (p->scale != 1 && p->scale != 2)) return 0;
    uint32_t lanes = 2u*p->scale;
    return (size_t)(2u*p->scale+1u)*p->width*8u + ((p->width+lanes-1u)/lanes)*16u;
}

int ps8_af_window_init(ps8_af_window *w,const psm_plan *p,void *memory,size_t bytes)
{
    if (!w || !p || !memory || ((uintptr_t)memory & 7u) || !ps8_af_window_bytes(p) ||
        bytes < ps8_af_window_bytes(p) || (p->frame_count != 4 && p->frame_count != 16)) return PSM_BAD_PLAN;
    /* NX's first source slot uses slot + 3 at the SAME sensor coordinate.
       The supported four-frame square puts that sample one output scale below.
       Reject other frame orders instead of silently applying an invalid guard. */
    for (uint32_t i=0;i<p->frame_count;i+=4) {
        const psm_frame *a=&p->frames[i],*d=&p->frames[i+3];
        if (d->dx2 != a->dx2 || d->dy2 != a->dy2+2) return PSM_BAD_LATTICE;
    }
    uint16_t *at=memory;
    w->slots=2u*p->scale+1u;w->width=p->width;
    for (uint32_t i=0;i<w->slots;++i) {
        w->rgb[i]=at;at+=3u*p->width;w->green[i]=at;at+=p->width;w->row_id[i]=UINT32_MAX;
    }
    w->features=at;w->values=at+7u*((p->width+2u*p->scale-1u)/(2u*p->scale));
    return PSM_OK;
}

static int blue_origin(const psm_plan *p,uint32_t y,uint32_t x,uint32_t *slot,uint32_t *source_y)
{
    for (uint32_t i=0;i<p->frame_count;++i) {
        const psm_frame *f=&p->frames[i];
        int32_t sy=p->y0+(int32_t)y-f->dy2*(int32_t)p->scale/2;
        int32_t sx=p->x0+(int32_t)x-f->dx2*(int32_t)p->scale/2;
        if (sy<0 || sx<0 || sy%(int32_t)p->scale || sx%(int32_t)p->scale) continue;
        sy/=(int32_t)p->scale;sx/=(int32_t)p->scale;
        if ((uint32_t)sy>=p->sensor_height || (uint32_t)sx>=p->sensor_width) continue;
        if (f->cfa[((uint32_t)sy&1u)*2u+((uint32_t)sx&1u)]==2) {
            *slot=i;*source_y=(uint32_t)sy;return PSM_OK;
        }
    }
    return PSM_BAD_LATTICE;
}

int ps8_af_render(ps8_af_window *w,const psm_plan *p,uint32_t row,uint16_t *out)
{
    uint32_t at=row%w->slots,period=2u*p->scale;
    if (!out || row>=p->height || w->row_id[at]!=row || w->width!=p->width) return PSM_BAD_PLAN;
    const uint16_t *current=w->rgb[at];
    for (uint32_t x=0;x<3u*p->width;++x) out[x]=current[x];
    for (uint32_t lane=0;lane<period && lane<p->width;++lane) {
        uint32_t slot,sy,other_slot,up_y,down_y;
        if (blue_origin(p,row,lane,&slot,&sy)) return PSM_BAD_LATTICE;
        if (sy<29 || sy>5489 || sy%12!=5) continue;
        if (row<p->scale || row+p->scale>=p->height) return PSM_BAD_CROP;
        uint32_t upper=row-p->scale,lower=row+p->scale;
        if (w->row_id[upper%w->slots]!=upper || w->row_id[lower%w->slots]!=lower) return PSM_BAD_PLAN;
        if (blue_origin(p,upper,lane,&other_slot,&up_y) ||
            blue_origin(p,lower,lane,&other_slot,&down_y)) return PSM_BAD_LATTICE;
        uint32_t neighbour=up_y%12!=5 ? upper : lower;
        if ((up_y%12==5) && (down_y%12==5)) return PSM_BAD_LATTICE;
        const uint16_t *near=w->rgb[neighbour%w->slots],*guard=w->rgb[lower%w->slots];
        const uint16_t *pg=w->green[at],*ng=w->green[neighbour%w->slots];
        uint32_t n=0;
        for (uint32_t x=lane;x<p->width;x+=period,++n) {
            uint16_t *f=w->features+7u*n;
            f[0]=current[3u*x+2u];f[1]=current[3u*x];f[2]=pg[x];
            f[3]=near[3u*x+2u];f[4]=near[3u*x];f[5]=ng[x];
            f[6]=slot%4u==0 ? guard[3u*x+2u] : f[0];
        }
        AF_BATCH(w->features,w->values,n);
        n=0;
        for (uint32_t x=lane;x<p->width;x+=period) out[3u*x+2u]=w->values[n++];
    }
    return PSM_OK;
}
