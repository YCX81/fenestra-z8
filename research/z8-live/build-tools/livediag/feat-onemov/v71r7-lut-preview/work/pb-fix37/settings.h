#ifndef PB_SETTINGS_H
#define PB_SETTINGS_H
#include "fenestra_native_menu.h"
#include <stdint.h>
/* REC=9 is the runtime's default. Off=0 releases REC to native playback.
 * This explicit setting is used because reading Nikon f3's role is unclosed.
 * Caller owns this state; no writable statics or fixed RAM mailbox. */
#define PB_SETTINGS_PAGE UINT32_C(0x705f)
#define PB_SETTINGS_ROW UINT32_C(0x7fb4)
typedef struct {
    uint32_t installed,error;
    FxNativeMenu *core;
    FxNativeMenu choices;
} PbSettings;
/* Implemented by runtime; may return NULL until the owner is published. */
PbSettings *pb_settings_state(void);
uint32_t pb_get_key(void);
void pb_set_key(uint32_t);
int pb_settings_install(PbSettings *);
struct PbRuntime;
void pb_settings_poll(struct PbRuntime *);
/* Guarded hooks at core entries 401cce84 /401cd3ec, not plan A callsites.
 * Link old_* to trampolines that replay one displaced STP then branch +4. */
FxNativePage *pb_settings_old_menu_page(uint32_t,uint32_t);
void pb_settings_old_menu_text(int32_t,int32_t,const uint16_t **,uint32_t *);
FxNativePage *pb_settings_menu_page(uint32_t,uint32_t);
void pb_settings_menu_text(int32_t,int32_t,const uint16_t **,uint32_t *);
#endif
