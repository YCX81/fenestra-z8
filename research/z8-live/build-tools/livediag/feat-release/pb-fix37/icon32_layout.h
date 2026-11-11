#ifndef ICON32_LAYOUT_H
#define ICON32_LAYOUT_H
#include <stdint.h>

/* The screen may rotate in the native compositor. Invalidate retained draws on
 * either the raw pose or Nikon's effective UI policy changing. Neither value
 * alone supplies the rotation of this particular graphics surface. */
static inline uint32_t icon32_orientation(void) {
    uint32_t pose=*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa03bc8004);
    uint32_t ui=((uint32_t (*)(void))(uintptr_t)UINT64_C(0x4038d59c))();
    return (pose&0xffu)|((ui&0xffu)<<8);
}

typedef struct { uint16_t x,y,w,h; } Icon32Rect;

/* Anchor fractions refer to the old scene. Destination extents are backing
 * pixels. The image renderer uses the same viewport for scene and target,
 * so both edges land on integers even on letterboxed/transposed outputs. */
static inline Icon32Rect icon32_rect(uint32_t vw,uint32_t vh,uint32_t sw,uint32_t sh,
                                     uint32_t x,uint32_t y,uint32_t w,uint32_t h) {
    if(!vw||!vh)return (Icon32Rect){0};
    if(w>vw||h>vh) {
        if(vw*h<=vh*w){h=h*vw/w;w=vw;}
        else{w=w*vh/h;h=vh;}
        if(!w)w=1u;
        if(!h)h=1u;
    }
    x=x*vw/sw;y=y*vh/sh;
    if(x>vw-w)x=vw-w;
    if(y>vh-h)y=vh-h;
    return (Icon32Rect){(uint16_t)x,(uint16_t)y,(uint16_t)w,(uint16_t)h};
}

/* With the default SVG viewBox the visible badge (circle AND wordmark)
 * is centred at x=49 in the 112-unit artwork. The native reference's A and
 * VIEW+camera rows have ink-box centres 44 and 54 on a 1024-wide viewport.
 * An explicit left anchor retains the previous --shoot-x behaviour. */
static inline Icon32Rect icon32_shoot_rect(uint32_t vw,uint32_t vh,
                                          uint32_t x,uint32_t y,uint32_t w,uint32_t h,
                                          uint32_t center_at_1024,uint32_t ink_mid2,
                                          uint32_t source_width) {
    Icon32Rect rect=icon32_rect(vw,vh,720u,480u,x,y,w,h);
    if(center_at_1024!=UINT32_MAX && rect.w && source_width) {
        uint32_t axis=(center_at_1024*vw+512u)/1024u;
        /* ink_mid2 is the sum of the measured half-open ink bounds of the
         * selected asset, so odd source widths and custom artwork still fit. */
        uint32_t ink_center=(ink_mid2*rect.w+source_width)/(2u*source_width);
        uint32_t left=axis>ink_center?axis-ink_center:0u;
        rect.x=(uint16_t)(left>vw-rect.w?vw-rect.w:left);
    }
    return rect;
}
#endif
