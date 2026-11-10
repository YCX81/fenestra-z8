#include "runtime.h"
#include <stddef.h>
const unsigned fr_layout[]={sizeof(struct live_runtime),offsetof(struct live_runtime,recorder),sizeof(struct fr_state),offsetof(struct fr_state,tx),offsetof(struct live_runtime,card_epoch)};
