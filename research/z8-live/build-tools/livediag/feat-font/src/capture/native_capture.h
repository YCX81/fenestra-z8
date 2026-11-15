#ifndef LIVE_A_NATIVE_CAPTURE_H
#define LIVE_A_NATIVE_CAPTURE_H
#include "live_capture.h"
struct la_native_geometry { uint32_t width,height,y_stride,uv_stride,nv12; };
struct la_native_task { int32_t id, native_error; uint32_t phase; };
int la_native_geometry(struct la_native_geometry *);
int la_native_geometry_k(struct la_native_geometry *,unsigned k);
int la_native_allocate(void *,uint32_t,void **,uint8_t **,uint32_t *);
void la_native_release(void *,void *);
uint64_t la_native_time_us(void);
/* Generic affinity parameter, preserving every existing Fenestra caller. */
int la_native_task_start(struct la_native_task *,void (*)(void *),void *,void *,uint32_t,uint32_t,uint32_t,uint32_t);
int la_native_cpu2_start(struct la_native_task *,void (*)(void *),void *,void *,uint32_t,uint32_t);
void la_native_yield(void);
int la_best_begin(void *,uint64_t,uint64_t,struct la_source *);
int la_best_valid(void *,const struct la_source *);
void la_best_end(void *,const struct la_source *);

int la_heap_allocate(void *,uint32_t,void **,uint8_t **,uint32_t *);
void la_heap_release(void *,void *);
#endif
