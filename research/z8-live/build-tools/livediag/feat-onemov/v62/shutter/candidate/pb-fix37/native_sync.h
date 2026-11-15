#ifndef LIVE_NATIVE_SYNC_H
#define LIVE_NATIVE_SYNC_H
#include "sync.h"
typedef struct {
    int32_t (*open)(const char *,uint32_t,uint32_t);
    uint32_t (*read)(int32_t,void *,uint32_t,uint32_t *);
    uint32_t (*close)(int32_t);
    uint32_t (*getattr)(const char *,uint32_t *);
    uint32_t (*setattr)(const char *,uint32_t);
    uint32_t (*remove)(const char *);
    uint32_t (*rmdir)(const char *);
    uint32_t (*dir_open)(const char *,int32_t *);
    uint32_t (*dir_next)(int32_t,void *);
    uint32_t (*dir_close)(int32_t);
    void (*lock)(void);
    void (*unlock)(void);
} LiveNativeSyncBackend;
typedef struct {
    LiveNativeSyncBackend fs;
    uint64_t generation,gc_generation;
    uint32_t last_error,poisoned,steps,gc_ready,gc_active;
    char drive;
    char native_path[LIVE_SYNC_PATH],directory[LIVE_SYNC_PATH];
} LiveNativeSync;
/* C3.11 only. Runtime must invalidate this instance and its journal on media
 * changes. Generation=1 is an emulation session epoch, not persistent identity. */
void live_native_sync_init(LiveNativeSync *,char drive,uint64_t generation);
/* Called by a low-priority task, never GUI/draw/IRQ. It holds native DCF
 * mutation ownership across scan+mutation and releases it before returning.
 * No yield occurs while ownership is held; caller yields after every step.
 * GC is permitted only after a successful complete boot directory census. */
int live_native_sync_step(LiveSyncJournal *,LiveNativeSync *);
/* One boot-only collection before a worker starts, with an empty journal.
 * Enumerate the entire directory tree through explicit EOF and checked close;
 * depth<=8, total entries<=4096, paths<64 bytes. Any error/bound/invalid LIVE
 * manifest rejects the complete census and queues nothing. Read and validate
 * at most 8 LIVE manifests, then queue only groups with no native member.
 * No mutation occurs here; file payloads other than MANIFEST are not read.
 * Return queued count >=0, or -1 on refusal/error. Stack budget <16 KiB. */
int live_native_sync_gc_boot(LiveSyncJournal *,LiveNativeSync *);
#endif
