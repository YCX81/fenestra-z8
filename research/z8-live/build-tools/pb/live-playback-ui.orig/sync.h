#ifndef LIVE_SYNC_H
#define LIVE_SYNC_H
#include <stddef.h>
#include <stdint.h>
#define LIVE_SYNC_CAPACITY 8u
#define LIVE_SYNC_PATH 64u
#define LIVE_SYNC_MANIFEST_MAX 256u
typedef struct {
    char native_path[LIVE_SYNC_PATH];
    char uuid[37];
    uint64_t native_size;
    uint64_t volume_generation;
} LiveManifest;
/* Parse v3's exact four-field MANIFEST. Reject duplicate/unknown fields,
 * NULs, unsafe paths, missing identities, numeric overflow and extra bytes. */
int live_manifest_parse(const void *,size_t,LiveManifest *);
int live_sync_directory(char out[LIVE_SYNC_PATH],const char *native_path);
enum { LIVE_SYNC_DELETE=1, LIVE_SYNC_PROTECT=2, LIVE_SYNC_GC=3 };
typedef struct { LiveManifest identity; uint8_t operation, phase; } LiveSyncEntry;
typedef struct {
    LiveSyncEntry entries[LIVE_SYNC_CAPACITY];
    uint32_t head,count,overflow;
} LiveSyncJournal;
typedef struct {
    void *ctx;
    /* Return 0 only for a mounted, fully readable volume with verified epoch.
     * On media change the caller increments epoch and flushes caches/journal.
     * A/B are volume names, never physical slot identities. */
    int (*volume_ready)(void *,char drive,uint64_t generation);
    int (*read_manifest)(void *,const char *,void *,size_t,size_t *);
    /* Atomically scan ALL native same-folder/prefix/stem formats, including
     * JPG and NEF. Error != absence. Must report actual current protection.
     * Serialize this scan + mutations with native media mutation ownership. */
    int (*native_group)(void *,const char *native_path,unsigned *members,unsigned *protected_members);
    /* Mutation callbacks must reject symlinks/reparse points/alias escapes;
     * remove absent file succeeds (idempotent). No recursive deletion. */
    int (*remove_file)(void *,const char *);
    int (*remove_directory)(void *,const char *);
    int (*set_readonly)(void *,const char *,int readonly);
    void (*yield)(void *);
} LiveSyncOps;
/* Enqueue performs bounded memory work only. Call after native success; path
 * and manifest identity must already have been cached before native delete.
 * External mutex serializes enqueue/step. In-memory journal is not durable. */
int live_sync_enqueue(LiveSyncJournal *,unsigned operation,const LiveManifest *);
/* 0 idle; 1 consumed; 2 deferred (I/O/media failure, retain journal); -1 bad API.
 * One entry per worker call, with yields between individual sidecar operations.
 * GC uses the same checks as delete, and is only queued at readable-volume boot. */
int live_sync_step(LiveSyncJournal *,const LiveSyncOps *);
#endif
