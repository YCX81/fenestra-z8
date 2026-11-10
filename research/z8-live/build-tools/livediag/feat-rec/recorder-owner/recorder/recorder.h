#ifndef W5_RECORDER_H
#define W5_RECORDER_H
#include <stdint.h>
#define FR_BYTES 2048u
#define FR_MAGIC 0x32524657u
#define FR_DONE 0x80000000u
struct fr_tx { uint32_t event, phase, error, written, close_rc, serial; };
struct fr_state {
 /* USB writes arm only. A new positive token requests one prepare attempt. */
 uint32_t arm, attempted, ready, claimed, lock, generation, seen, missed;
 uint32_t client, kbps, size, epoch, seq, pending, adapter_gen, reserved;
 uint64_t session, arena, movrs_sp, adapter_sp;
 struct fr_tx tx[4];
 uint8_t io[FR_BYTES] __attribute__((aligned(16)));
};
struct live_runtime;
struct fr_state *fr_context(void);
uint32_t fr_enabled(uint32_t,const uint64_t *);
void fr_prepare(struct live_runtime *);
void fr_probe(uint32_t event,const uint64_t *regs);
void fr_encoder_snapshot(uint32_t *out);
#endif
