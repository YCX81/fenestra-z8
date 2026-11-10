#ifndef W5_RECORDER_H
#define W5_RECORDER_H
#include <stdint.h>
#define FR_SLOTS 6u
#define FR_BYTES (FR_SLOTS*512u)
#define FR_MAGIC 0x344c4657u /* WFL4 */
#define FR_VERSION 4u
#define FR_ENTRY 15u
#define FR_I 0u
#define FR_R 1u
#define FR_S 2u
#define FR_M 3u
#define FR_D 4u
#define FR_P 5u
#define FR_P_MAX_US 1600000u
#define FR_LAMP_BUSY (1u<<30)
#define FR_RETURNED (1u<<29)
struct fr_state;
void fr_progress(struct fr_state *,const uint64_t *,uint32_t,uint32_t,uint32_t,uint32_t,int32_t);
void fr_worker_tick(struct fr_state *);
void fr_checkpoint_value(struct fr_state *,const uint64_t *,uint32_t,uint32_t);
void fr_checkpoint(struct fr_state *,const uint64_t *,uint32_t);
void fr_close_begin(struct fr_state *,const uint64_t *,const void *);
void fr_close_end(struct fr_state *,const void *);
uint64_t fr_close_grace(void);
#define FR_D_MAX_US 1250000u
/* Six lock-held D frames, each bounded even for the longest wire code.
 * D has a fixed 14-bit encoding (no escape extension); all NR values fit. */
#define FR_D_TOTAL_MAX_US (6u * FR_D_MAX_US)
#define FR_CLOSE_MARGIN_US 100000u
/* EPOCH is informational; all other bits reject association. */
#define FR_EPOCH 1u
#define FR_SHUTDOWN 2u
#define FR_ARM 4u
#define FR_NOT_READY 8u
#define FR_CLAIMED 16u
#define FR_MODE 32u
#define FR_LOCK 64u
#define FR_RACE 128u
#define FR_DONE 0x80000000u
struct fr_tx { uint32_t event, phase, error, written, close_rc, serial; };
struct fr_state {
 /* Existing owner arm mechanism. Positive token requests one prepare attempt.
  * Keep v3 offsets/extent: six sectors allocated, only sector 0 written in v6. */
 uint32_t arm, attempted, ready, claimed, lock, generation, seen, missed;
 uint32_t client, kbps, size, epoch, seq, lamp_seen, lamp_failed, file_owned;
 uint64_t session, arena, movrs_sp, live_exit_active;
 uint32_t entry_claimed, skip_nr;
 struct fr_tx tx[FR_SLOTS];
 /* Preparation uses all sectors before claimed; REC uses only RAM in the
  * unused second sector. Preserve runtime/gate/NR ABI and on-disk format. */
 union {
  uint8_t io[FR_BYTES] __attribute__((aligned(16)));
  struct { uint8_t reserved[512]; uintptr_t close_owner;
   uint64_t d_start[6], d_elapsed[6];
   /* Single MOVRS writer. packed is one release-published wire snapshot;
    * worker never waits for a seqlock held by a stalled producer. */
   uint32_t packed, updates, timestamp_lo, busy, alive, delay_rc, raw_value;
   uint32_t busy_iterations, delay_returns;
   uint32_t i_done, heartbeat, reports, report_failed;
   uint64_t last_report;

  } diag;
 };
};
/* Reserved lamp_seen bits record actual suppressed calls, not requested mode. */
#define FR_NR_SHIFT 8u
uint64_t fr_live_exit(uint32_t client);
uint64_t fr_exit_scoped(struct fr_state *);
int fr_nr_begin(struct fr_state *, uintptr_t);
void fr_nr_end(struct fr_state *);
int fr_nr_allow(struct fr_state *, const uint64_t *, uint64_t);
struct live_runtime;
struct fr_state *fr_context(void);
struct fr_state *fr_before(const uint64_t *regs);
void fr_light(struct fr_state *,uint32_t stage,const uint64_t *regs);
void fr_prepare(struct live_runtime *);
void fr_probe(uint32_t event,const uint64_t *regs);
#endif
