/* PS8 selection controller, shared by the native adapter and host tests. */
#ifndef PS8_BROWSER_H
#define PS8_BROWSER_H
#include "group.h"
enum psb_phase {PSB_EMPTY,PSB_SCANNING,PSB_LIST,PSB_CONFIRM,PSB_RUNNING};
typedef struct {uint32_t start,group,count;uint8_t card,serial[32];} psb_identity;
typedef struct {
 const psm_scan *scan;
 uint32_t generation,phase,cursor,selected_count;
 uint32_t selected[PSM_SCAN_GROUPS/32];
 uint16_t targets[PSM_SCAN_GROUPS];
 uint32_t target_count;
} psb_browser;
int psb_identity_at(const psb_browser *,uint32_t,psb_identity *);
int psb_find_identity(const psm_scan *,const psb_identity *,uint32_t *);
void psb_begin_scan(psb_browser *,uint32_t generation);
int psb_publish(psb_browser *,uint32_t generation,const psm_scan *);
int psb_move(psb_browser *,int delta);
int psb_toggle(psb_browser *,uint32_t order_row);
int psb_select_unmerged(psb_browser *);
int psb_confirm(psb_browser *);
int psb_back(psb_browser *);
int psb_start(psb_browser *,uint32_t generation);
int psb_finish(psb_browser *,uint32_t generation);
#endif
