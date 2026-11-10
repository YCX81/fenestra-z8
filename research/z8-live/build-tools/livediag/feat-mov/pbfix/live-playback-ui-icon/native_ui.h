#ifndef LP_NATIVE_UI_H
#define LP_NATIVE_UI_H
#include "fenestra_native_display.h"
typedef struct {
    FxNativeDisplay display; /* First: native object callbacks receive this address. */
    uint32_t images_created,images_destroyed,paths_created,paths_destroyed;
    uint32_t paints_created,paints_destroyed,unlinks,skipped_owner;
} PbNativeDisplay;
_Static_assert(offsetof(PbNativeDisplay,display)==0,"native display callback ABI");
extern const FxNativeDisplayProfile pb_native_display_profile;
FxNativeView pb_native_current_view(void);
uint32_t pb_native_display_global_ready(void);
FxResult pb_native_display_attach_global(FxNativeDisplay *,const FxDemoScene *);
/* Private global-root candidate. All caller scene/data storage must remain
 * immutable until gate disabled + detach returns; native UI owner serializes calls.
 * Parent must establish full-frame still mode before attach. */
FxResult pb_display_attach(PbNativeDisplay *,const FxDemoScene *,_Atomic uint64_t *gate);
FxResult pb_display_present(PbNativeDisplay *,const FxDemoScene *);
void pb_display_detach(PbNativeDisplay *);
uint32_t pb_display_ready(void);
uint32_t pb_display_is_current(const PbNativeDisplay *);
FxNativeDisplay *pb_display_state(PbNativeDisplay *);
#endif
