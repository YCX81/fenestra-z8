#ifndef HELD_SOUND_PRIVATE_H
#define HELD_SOUND_PRIVATE_H
#include <stdint.h>
/* PRIVATE dispatcher-thread prototype, not an installed hook.
 * Only an envelope intercepted before native dispatch/ownership transfer may
 * enter state1. The caller must run on the original serialized SFX dispatcher.
 * Live's worker must never invoke this against a concurrently running task.
 */
struct held_sound { uint64_t message; uint32_t state, generation; };
/* 1: both held start and cancel completed, 0: no matching held ownership,
 * -1: state/shape/owner mismatch; leave all native allocations untouched.
 */
int held_sound_cancel(struct held_sound *, void *cancel_message);
#endif
