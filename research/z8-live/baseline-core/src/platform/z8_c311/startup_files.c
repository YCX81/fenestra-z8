#include "fenestra_startup_files.h"
#define FN(a,t) ((t)(uintptr_t)(a))
/* C3.11 wrappers independently exercised against the pinned original MAIN.
 * Separate paths and record identity preserve C110 settings on a shared card. */
const FxStartupFileProfile fx_z8_startup_file_profile={
    FN(0x402dd4f8,int32_t (*)(const char *,uint32_t,uint32_t)),
    FN(0x402dd6c8,uint32_t (*)(int32_t,void *,uint32_t,uint32_t *)),
    FN(0x402dd78c,uint32_t (*)(int32_t,const void *,uint32_t,uint32_t *,void *)),
    FN(0x402dd558,uint32_t (*)(int32_t)),
    FN(0x402dda4c,uint32_t (*)(const char *)),
    {"A:\\FENESTRA\\boot-z8.flag","A:\\FENESTRA\\startup-z8.0","A:\\FENESTRA\\startup-z8.1"},
    UINT32_C(0x80000010)
};
