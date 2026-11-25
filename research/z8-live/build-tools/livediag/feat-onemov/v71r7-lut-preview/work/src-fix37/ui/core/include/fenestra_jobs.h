#ifndef FENESTRA_JOBS_H
#define FENESTRA_JOBS_H
#include "fenestra.h"
/* core.jobs/v1 prototype: one background EL0 thread in the module's own address
 * space, with blocking calls for large card IO and large memory. Main-entry
 * rules are unchanged (100 ms per invocation, copied exchange).
 *
 * Module main entry -> copied FX_BIND_CALL on "core.jobs": QUERY/START/STATUS/CANCEL.
 * Job thread -> svc #0x4653 (fx_job_call below). A native call blocks this
 * thread only; every other module and the native UI keep running. */
#define FX_JOB_ABI 1u
enum {FX_JOB_QUERY=1u,FX_JOB_START,FX_JOB_STATUS,FX_JOB_CANCEL};
enum {FX_JOB_IDLE,FX_JOB_QUEUED,FX_JOB_RUNNING,FX_JOB_DONE,FX_JOB_CANCELLED,FX_JOB_FAILED};
#define FX_JOB_MAX_BUFFERS 8u
#define FX_JOB_MAX_FILES 16u
#define FX_JOB_BIG_MAX (256u*1024u*1024u)
#define FX_JOB_BIG_BUDGET (UINT64_C(512)*1024u*1024u)
#define FX_JOB_FLOOR (32u*1024u*1024u)
#define FX_JOB_IO_CHUNK (1024u*1024u)
#define FX_JOB_SOFT_STOP_MS 1000u
typedef struct {
    uint32_t abi,bytes;uint64_t entry,argument;
    uint32_t total_ms,watchdog_ms; /* zero total: 30 min; zero watchdog: 10 s */
    uint32_t run_ms,sleep_ticks;   /* zero run_ms: never yield voluntarily (test only) */
    uint32_t priority,reserved;    /* zero: FX_JOB_PRIORITY */
} FxJobStart;
typedef struct {
    uint32_t abi,bytes,state,error;
    uint64_t result,progress,elapsed_ms,resumes,syscalls,sleeps,read_bytes,write_bytes;
    uint64_t memory_bytes,memory_peak,native_error,cancel;
} FxJobStatus;
/* Read-only to EL0. */
typedef struct {
    uint32_t abi,bytes;uint64_t argument;
    volatile uint32_t cancel,photos; /* cancel: 0 run, 1 stop soon. photos: grant present. */
    struct FxJobShared *shared;
} FxJobApi;
typedef struct {char name[40];uint64_t bytes;uint32_t attributes,reserved;} FxJobEntry;
/* Read/write to EL0, read by the core as untrusted data. */
typedef struct FxJobShared {
    volatile uint64_t heartbeat,progress;
    char path[128],path2[128];
    FxJobEntry entry;
} FxJobShared;
_Static_assert(sizeof(FxJobShared)<=4096u,"job shared page");
enum {FX_SYS_ALLOC=1u,FX_SYS_FREE,FX_SYS_OPEN,FX_SYS_READ_AT,FX_SYS_WRITE_AT,FX_SYS_CLOSE,
      FX_SYS_REMOVE,FX_SYS_RENAME,FX_SYS_LIST_OPEN,FX_SYS_LIST_NEXT,FX_SYS_LIST_CLOSE,
      FX_SYS_TIME,FX_SYS_SLEEP,FX_SYS_TEST_INJECT};
enum {FX_JOB_OPEN_READ,FX_JOB_OPEN_CREATE};
/* Return: >=0 success value, otherwise -(int64_t)FxResult. *second carries a
 * size (OPEN) or a native error code (failed native call). */
static inline int64_t fx_job_call(uint64_t op,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t *second) {
    register uint64_t x0 __asm__("x0")=op;
    register uint64_t x1 __asm__("x1")=a;
    register uint64_t x2 __asm__("x2")=b;
    register uint64_t x3 __asm__("x3")=c;
    register uint64_t x4 __asm__("x4")=d;
    __asm__ volatile("svc #0x4653":"+r"(x0),"+r"(x1):"r"(x2),"r"(x3),"r"(x4):"memory");
    if(second)*second=x1;
    return (int64_t)x0;
}
#endif
