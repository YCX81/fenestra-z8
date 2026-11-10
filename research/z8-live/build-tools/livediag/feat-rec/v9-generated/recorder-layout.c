#include "runtime.h"
#include <stddef.h>
const unsigned fr_layout[]={sizeof(struct live_runtime),offsetof(struct live_runtime,recorder),sizeof(struct fr_state),offsetof(struct fr_state,tx),offsetof(struct live_runtime,card_epoch),offsetof(struct live_runtime,shutdown),FR_BYTES,FR_SLOTS,offsetof(struct fr_state,skip_nr),offsetof(struct fr_state,live_exit_active),0x4e523031,0x4c503036};
