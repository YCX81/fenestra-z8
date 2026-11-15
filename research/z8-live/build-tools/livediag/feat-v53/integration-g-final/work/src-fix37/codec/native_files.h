#ifndef LIVE_NATIVE_FILES_H
#define LIVE_NATIVE_FILES_H
#include "live_transaction.h"
/* Lifetime covers the whole transaction. Native callbacks require task context
   and original firmware mappings, not IRQ or an EL0/gate address space. */
typedef struct {
 void *user;
 int (*cancelled)(void *);
 void (*yield)(void *);
 void (*encode_event)(void *,unsigned);
 uint32_t last_native_error, dir_stage;
 char dir_path[128];
} live_native_file_context;
void live_native_file_ops(live_file_ops *,live_native_file_context *);
#endif
