#include "fenestra_native_linux.h"
#define FN(a,t) ((t)(uintptr_t)(a))
#include "ptp.h"
extern FxLinuxBridge *fx_z8_menu_linux(void);
const FxNativeLinuxProfile fx_native_linux_profile={
    fx_z8_menu_linux,fx_z8_menu_ptp,
    FN(UINT64_C(0xa02e920c4),const volatile uint8_t *),
    FN(UINT64_C(0xa02e93860),const volatile uint64_t *),
    FN(UINT64_C(0xa02e93868),const volatile uint32_t *),
    FN(UINT64_C(0xa02e93530),_Atomic uintptr_t *),
    FN(UINT64_C(0xa02e91010),const volatile uintptr_t *),
    UINT64_C(0xa0f000000),UINT64_C(0xa0f7fefc0),0x7ff000u,"FXL3",
    FN(UINT64_C(0x40c9da40),uint32_t (*)(uint32_t,const uint8_t *)),
    FN(UINT64_C(0x40c9e48c),uint8_t *(*)(uint32_t)),
    FN(UINT64_C(0x40c9e63c),void (*)(void *)),
    FN(UINT64_C(0x40c9e794),int32_t (*)(const void *)),
    FN(UINT64_C(0x41278778),int32_t (*)(void *)),
    FN(UINT64_C(0x41278838),int32_t (*)(void *)),
    FN(UINT64_C(0x41278210),int32_t (*)(uintptr_t,const void *,uint32_t)),
    FN(UINT64_C(0x41277808),int32_t (*)(uintptr_t)),
    4u,
    1u, /* Original offset-free includes the pool base at offset zero. */
};
