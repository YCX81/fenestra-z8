#ifndef PB_RUNTIME_H
#define PB_RUNTIME_H
#include <stdint.h>
#include <stdatomic.h>
#include "native_ui.h"
#include "preview.h"
#include "sync.h"
#include "native_sync.h"
#define PB_MAGIC 0x50425549u
typedef struct PbRuntime {
    uint32_t magic,bytes;
    _Atomic uint32_t key,media_epoch,shutdown,key_epoch,start_request;
    _Atomic uint32_t journal_lock;
    _Atomic uint64_t gate;
    uint32_t started,error,polls,current_id,cache_valid,blocked_key_epoch;
    uint32_t lookup_count,lookup_error,frames,playing,last_frame,restores;
    uint32_t key_presses,delete_success,protect_success,journal_drops;
    uint8_t previous_keys[128];
    LiveManifest manifest;
    char native_path[64],directory[64];
    uint64_t photo_size,start_us,preview_size;
    int32_t preview_fd;
    LpPreview preview;
    LiveSyncJournal journal;
    LiveNativeSync sync;
    void *settings;
    PbNativeDisplay display;
    FxDemoScene scene;
    FxSceneCommand commands[8];
    uint8_t i420[101760],pixels[271384];
    void *stack;
    uint8_t task[32];
    uint32_t gc_attempted;
    int32_t gc_result;
    uint32_t cached_id;
    _Atomic uint32_t render_request,ui_leaving;
    uint32_t ui_polls,ui_presented,ui_detached,ui_render_error,rendered_frames;
    uint32_t consumed_rec,after_leave_requests;
    uint32_t ui_state_calls,ui_state_detached;
} PbRuntime;
PbRuntime *pb_runtime_get(void);
uint32_t pb_get_key(void);
void pb_set_key(uint32_t key);
uint32_t pb_key_notify(const uint8_t *);
uint64_t pb_key_decode_wrap(const uint8_t *,uintptr_t,uintptr_t);
uint32_t pb_ui_poll_wrap(void *);
uint32_t pb_ui_wait_wrap(void);
void pb_ui_leave(void);
void pb_ui_state_notify(void);
void pb_boot(void);
void pb_system_notify(const void *);
uint32_t pb_delete_wrap(uint32_t,uint32_t);
uint32_t pb_protect_wrap(void *,uint32_t);
#endif
