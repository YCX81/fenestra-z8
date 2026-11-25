#ifndef LIVE_UI_H
#define LIVE_UI_H
#include <stdint.h>
#include <stdatomic.h>
/* Runtime-provided API. Set is nonblocking and must not allocate or do IO. */
#define LIVE_REQUESTED 1u
#define LIVE_READY 2u
#define LIVE_BUSY 4u
#define LIVE_ERROR 8u
#define LIVE_WARM 16u            /* v46: a press now gives a shorter clip */
#define LIVE_PROGRESS_SHIFT 8u   /* v46: bits 8..11 = pre-roll twelfths, 12 = ready */
void live_requested_set(uint32_t requested);
uint32_t live_status(void);
/* v39: shutter-sound mute while Live is requested (runtime-owned, nonblocking). */
void live_shutter_mute_set(uint32_t mute);
uint32_t live_shutter_mute_get(void);
void live_runtime_boot(void);
/* Called by UI system wrapper before forwarding irreversible suspend (ev=16) or card-change (ev=24). */
void live_runtime_event(uint32_t event);
void live_runtime_ui_event(uint32_t event);
/* park=1: flush LIVELOG and park/remove BOOT-Z8.FLAG (ev=7).
 * park=0: immediately restore parked BOOT-Z8.FLAG (ev=8/ev=15). */
int live_runtime_flag_park(int park);
/* UI owns immutable lifetime heap root, parent owns engine it publishes here.
 * get is a bounded, lock-free, allocation-free IRQ-safe load sequence. */
void *live_ui_engine_get(void);
void live_ui_engine_set(void *engine);
/* Parent low-priority worker calls periodically, never IRQ/GUI callback. */
void live_ui_poll(void);
uint32_t live_ui_save_error(void);
struct live_runtime;
void live_ui_evf_flush(struct live_runtime *);
_Atomic(void *) *live_ui_filmsim_context_slot(void);
/* V71.7-r4: 1 iff livephoto.wlm is loaded & Fenestra phase==2 (0x401ccde4). */
int livephoto_is_module_loaded(void);
#include "live_ipc.h"
#endif
