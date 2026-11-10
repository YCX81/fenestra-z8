#include "live_audio.h"
/* C3.11 allocator ABI verified statically; selector0 admission + memory budget
 * must be checked by integrator before enabling ANY backend. Allocation may
 * round 768000 bytes to an entire 0xaec000-byte class24 unit. */
int live_audio_native_alloc(void *ctx,uint32_t bytes,void **d,uint8_t **cpu,uint32_t *actual) {
 (void)ctx;
 *d=0;*cpu=0;*actual=0;
 if(*(volatile uint32_t *)(uintptr_t)0xa03f31a68!=0)return -1;
 *d=((void *(*)(uint32_t,uint32_t,uint32_t))(uintptr_t)0x4104d2dc)(0x4c4155,0x60,bytes);
 if(!*d)return -1;
 uintptr_t r=*(uintptr_t *)((uint8_t *)*d+8);
 if(!r || *(uint32_t *)((uint8_t *)*d+0x10)!=0x24 ||
    *(volatile uint32_t *)(uintptr_t)0xa03f31a68!=0)return -1;
 *cpu=*(uint8_t **)r;*actual=*(uint32_t *)(r+8);
 if((uintptr_t)*cpu>UINTPTR_MAX-*actual)return -1;
 return 0;
}
void live_audio_native_free(void *ctx,void *d) {
 (void)ctx;((void (*)(void *))(uintptr_t)0x4104d600)(d);
}
/* Allocation only; the photo capture backend is live_audio_z8_ops. */
void live_audio_native_ops(struct live_audio_ops *o) {
 *o=(struct live_audio_ops){0,live_audio_native_alloc,live_audio_native_free,0,0,0};
}
uint64_t live_audio_clock_us(void) {
#if defined(__aarch64__)
 uint64_t n,hz;
 __asm__ volatile("isb; mrs %0,cntpct_el0; mrs %1,cntfrq_el0":"=r"(n),"=r"(hz)::"memory");
 return hz?n/hz*1000000u+(n%hz)*1000000u/hz:0;
#else
 return 0; /* host tests supply explicit times; no invented wall-clock epoch */
#endif
}
