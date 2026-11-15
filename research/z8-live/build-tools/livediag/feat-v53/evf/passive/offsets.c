#include "live_ui.c"
#define O(t,m) offsetof(t,m)
const uint32_t diag_offsets[]={sizeof(UiState),O(UiState,evf_diag),O(UiState,display),O(UiState,gate),
 O(UiState,loaded),O(UiState,shutdown),O(UiState,icon_blocked),O(UiState,presents),O(UiState,present_fail),O(UiState,not_current),O(UiState,target_lost),O(UiState,display_status),
 sizeof(struct live_runtime),O(struct live_runtime,stream),O(struct live_runtime,stream.job),O(struct live_runtime,writer_active),O(struct live_runtime,capture.phase),O(struct live_runtime,capture.press_mailbox),O(struct live_runtime,crumb_head),O(struct live_runtime,crumbs),
 O(FxNativeDisplay,gate),O(FxNativeDisplay,attached),O(FxNativeDisplay,surface),O(FxNativeDisplay,callbacks),O(FxNativeDisplay,error),O(FxNativeDisplay,global_overlay),
 sizeof(FxNativePage),O(FxNativePage,descriptor),O(CorePrefix,setup.count),O(CorePrefix,events[46]),
 sizeof(struct evf_diag),O(struct evf_diag,draws),O(struct evf_diag,result),O(struct evf_diag,submits),O(struct evf_diag,published),O(struct evf_diag,used),O(struct evf_diag,drained),O(struct evf_diag,samples),sizeof(struct evf_diag_sample),O(struct live_runtime,stream.log),O(struct live_runtime,stream.log_head)};
