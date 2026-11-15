#include <stddef.h>
#include "runtime.h"
const unsigned offsets[]={sizeof(struct live_runtime),
offsetof(struct live_runtime,shutdown),sizeof(((struct live_runtime*)0)->shutdown),
offsetof(struct live_runtime,card_epoch),sizeof(((struct live_runtime*)0)->card_epoch),
offsetof(struct live_runtime,requested),sizeof(((struct live_runtime*)0)->requested)};
