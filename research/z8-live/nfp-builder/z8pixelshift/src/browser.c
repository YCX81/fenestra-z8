/* PS8: explicit selection including merged groups, stable table indexes, generation-guarded ownership.
 * GUI must publish a complete scan, then may render rows in scan.order. It must not mutate/free the scan
 * until finish/back has stopped using it. finish invalidates the old table: re-scan before showing statuses.
 * Native adapter: browser_fw.inc. Host integration: test_selection.py.
 */
#include "browser.h"
static void clear_selection(psb_browser *b){for(unsigned i=0;i<PSM_SCAN_GROUPS/32;i++)b->selected[i]=0;b->selected_count=0;b->target_count=0;}
void psb_begin_scan(psb_browser *b,uint32_t generation){
 b->scan=0;b->generation=generation;b->phase=PSB_SCANNING;b->cursor=0;clear_selection(b);
}
int psb_publish(psb_browser *b,uint32_t generation,const psm_scan *s){
 if(!s||b->phase!=PSB_SCANNING||b->generation!=generation||s->ngroups>PSM_SCAN_GROUPS)return 1;
 uint32_t seen[PSM_SCAN_GROUPS/32]={0};
 for(unsigned i=0;i<s->ngroups;i++){unsigned k=s->order[i];if(k>=s->ngroups||(seen[k/32]&(1u<<(k%32))))return 1;seen[k/32]|=1u<<(k%32);}
 b->scan=s;b->phase=PSB_LIST;return 0;
}
int psb_move(psb_browser *b,int delta){
 if(b->phase!=PSB_LIST||!b->scan)return 1;
 int64_t k=(int64_t)b->cursor+delta;if(k<0)k=0;
 if(!b->scan->ngroups)k=0;else if(k>=b->scan->ngroups)k=b->scan->ngroups-1;
 b->cursor=(uint32_t)k;return 0;
}
int psb_toggle(psb_browser *b,uint32_t row){
 if(b->phase!=PSB_LIST||!b->scan||row>=b->scan->ngroups)return 1;
 uint32_t k=b->scan->order[row];if(!psm_group_mergeable(&b->scan->groups[k]))return 1;
 uint32_t mask=1u<<(k%32);if(b->selected[k/32]&mask){b->selected[k/32]&=~mask;--b->selected_count;}
 else {b->selected[k/32]|=mask;++b->selected_count;}return 0;
}
int psb_select_unmerged(psb_browser *b){
 if(b->phase!=PSB_LIST||!b->scan)return 1;clear_selection(b);
 for(unsigned i=0;i<b->scan->ngroups;i++){const psm_group *g=&b->scan->groups[b->scan->order[i]];if(!g->merged&&psm_group_mergeable(g))psb_toggle(b,i);}
 return 0;
}
int psb_confirm(psb_browser *b){
 if(b->phase!=PSB_LIST||!b->scan||!b->selected_count)return 1;
 b->target_count=0;
 for(unsigned i=0;i<b->scan->ngroups;i++){uint16_t k=b->scan->order[i];if(b->selected[k/32]&(1u<<(k%32)))b->targets[b->target_count++]=k;}
 if(b->target_count!=b->selected_count)return 1;
 b->phase=PSB_CONFIRM;return 0;
}
int psb_back(psb_browser *b){if(b->phase!=PSB_CONFIRM)return 1;b->phase=PSB_LIST;b->target_count=0;return 0;}
int psb_start(psb_browser *b,uint32_t generation){
 if(b->phase!=PSB_CONFIRM||b->generation!=generation)return 1;
 b->phase=PSB_RUNNING;return 0;
}
int psb_finish(psb_browser *b,uint32_t generation){
 if(b->phase!=PSB_RUNNING||b->generation!=generation)return 1;
 psb_begin_scan(b,generation+1);return 0;
}

int psb_identity_at(const psb_browser *b,uint32_t row,psb_identity *id){
 if(!b->scan||row>=b->scan->ngroups)return 1;
 const psm_group *g=&b->scan->groups[b->scan->order[row]];
 id->start=g->start;id->group=g->group;id->count=g->count;id->card=g->card;
 for(unsigned i=0;i<32;++i)id->serial[i]=g->serial[i];return 0;
}
int psb_find_identity(const psm_scan *s,const psb_identity *id,uint32_t *row){
 if(!s||!id||!row||s->ngroups>PSM_SCAN_GROUPS)return 1;
 for(unsigned i=0;i<s->ngroups;++i){
  if(s->order[i]>=s->ngroups)return 1;
  const psm_group *g=&s->groups[s->order[i]];
  if(g->start!=id->start||g->group!=id->group||g->count!=id->count||g->card!=id->card)continue;
  unsigned j=0;while(j<32&&g->serial[j]==id->serial[j])++j;
  if(j==32){*row=i;return 0;}
 }return 1;
}
