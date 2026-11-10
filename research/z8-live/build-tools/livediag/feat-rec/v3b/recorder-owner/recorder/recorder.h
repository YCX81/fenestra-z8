#ifndef W5_RECORDER_H
#define W5_RECORDER_H
#include <stdint.h>
#define FR_SLOTS 6u
#define FR_BYTES (FR_SLOTS*512u)
#define FR_MAGIC 0x33524657u
#define FR_VERSION 3u
#define FR_ENTRY 15u
#define FR_REJECT 16u
/* EPOCH is informational; all other bits reject association. */
#define FR_EPOCH 1u
#define FR_SHUTDOWN 2u
#define FR_ARM 4u
#define FR_NOT_READY 8u
#define FR_CLAIMED 16u
#define FR_MODE 32u
#define FR_LOCK 64u
#define FR_RACE 128u
#define FR_AFTER 256u /* rejection observed after hand-back returned */
#define FR_DONE 0x80000000u
struct fr_tx { uint32_t event, phase, error, written, close_rc, serial; };
struct fr_state {
 /* USB writes arm only. A new positive token requests one prepare attempt. */
 uint32_t arm, attempted, ready, claimed, lock, generation, seen, missed;
 uint32_t client, kbps, size, epoch, seq, pending, adapter_gen, file_owned;
 uint64_t session, arena, movrs_sp, adapter_sp;
 uint32_t entry_claimed, reject_claimed;
 struct fr_tx tx[FR_SLOTS];
 uint8_t io[FR_BYTES] __attribute__((aligned(16)));
};
struct live_runtime;
struct fr_state *fr_context(void);
uint32_t fr_enabled(uint32_t,const uint64_t *);
void fr_before(const uint64_t *regs);
void fr_prepare(struct live_runtime *);
void fr_probe(uint32_t event,const uint64_t *regs);
void fr_encoder_snapshot(uint32_t *out);
#endif
