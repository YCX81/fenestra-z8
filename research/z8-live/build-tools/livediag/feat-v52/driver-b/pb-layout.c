#include <stddef.h>
#include "runtime.h"
const unsigned offsets[]={
sizeof(PbRuntime),
offsetof(PbRuntime,audio_stage),sizeof(((PbRuntime*)0)->audio_stage),
offsetof(PbRuntime,audio_plays),sizeof(((PbRuntime*)0)->audio_plays),
offsetof(PbRuntime,audio_errors),sizeof(((PbRuntime*)0)->audio_errors),
offsetof(PbRuntime,audio_stop),sizeof(((PbRuntime*)0)->audio_stop),
offsetof(PbRuntime,audio_waits),sizeof(((PbRuntime*)0)->audio_waits),
offsetof(PbRuntime,audio_skipped),sizeof(((PbRuntime*)0)->audio_skipped),
offsetof(PbRuntime,audio_reason),sizeof(((PbRuntime*)0)->audio_reason),
offsetof(PbRuntime,audio_source_frames),sizeof(((PbRuntime*)0)->audio_source_frames),
offsetof(PbRuntime,audio_pending),sizeof(((PbRuntime*)0)->audio_pending)
};
