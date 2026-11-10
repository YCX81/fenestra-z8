#ifndef LP_UI_H
#define LP_UI_H
#include "fenestra_demo.h"
#define PB_SCENE_COMMANDS 8u
#define PB_PREVIEW_WIDTH 1024u   /* largest accepted preview (Live clip size); 320x212 also plays */
#define PB_PREVIEW_HEIGHT 682u
#define PB_PREVIEW_FRAME_BYTES (PB_PREVIEW_WIDTH*PB_PREVIEW_HEIGHT*3u/2u)
#define PB_BADGE_WIDTH 112u
#define PB_BADGE_HEIGHT 44u
#define PB_BADGE_DATA_BYTES (sizeof(FxSceneImage)+PB_BADGE_WIDTH*PB_BADGE_HEIGHT*4u)
#define PB_SCENE_DATA_BYTES (sizeof(FxSceneImage)+PB_PREVIEW_WIDTH*PB_PREVIEW_HEIGHT*4u)
/* No allocation, I/O, GUI calls, native-file reads or writable globals. Caller
 * retains scene, commands and data immutably while the draw gate is enabled. */
FxResult pb_scene_badge(FxDemoScene *,FxSceneCommand commands[PB_SCENE_COMMANDS]);
/* Bitmap path used by native playback: avoids the unproven vector glyph painter. */
FxResult pb_scene_badge_bitmap(FxDemoScene *,FxSceneCommand commands[PB_SCENE_COMMANDS],
    void *data,uint32_t capacity);
FxResult pb_scene_frame(FxDemoScene *,FxSceneCommand commands[PB_SCENE_COMMANDS],
    void *data,uint32_t capacity,const void *i420,uint32_t bytes);
#endif
