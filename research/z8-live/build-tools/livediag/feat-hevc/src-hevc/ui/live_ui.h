#ifndef LIVE_UI_H
#define LIVE_UI_H
#include <stdint.h>
/* Runtime-provided API. Set is nonblocking and must not allocate or do IO. */
#define LIVE_REQUESTED 1u
#define LIVE_READY 2u
#define LIVE_BUSY 4u
#define LIVE_ERROR 8u
void live_requested_set(uint32_t requested);
uint32_t live_status(void);
void live_runtime_boot(void);
/* Called by UI system wrapper before forwarding shutdown/card-change events. */
void live_runtime_event(uint32_t event);
void live_runtime_ui_event(uint32_t event);
int live_runtime_flag_park(int park);
/* UI owns immutable lifetime heap root, parent owns engine it publishes here.
 * get is a bounded, lock-free, allocation-free IRQ-safe load sequence. */
void *live_ui_engine_get(void);
void live_ui_engine_set(void *engine);
/* Parent low-priority worker calls periodically, never IRQ/GUI callback. */
void live_ui_poll(void);
uint32_t live_ui_save_error(void);
#endif
