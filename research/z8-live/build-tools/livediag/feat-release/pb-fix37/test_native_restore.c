/* Host contract test of the real detach implementation and native API order. */
#include "native_ui.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static _Alignas(8) unsigned char native_view[0x118],other_view[0x118],surface[0x120];
static uintptr_t current_view;
static uint32_t state=1,view_id=2;
static uint16_t current_viewport[4]={0,0,1280,800};
static int step,removed,keep_link,drop_owner,paints,clears,draws,submits,visible,enabled,foreign_page;
static void held(void *p) {(void)p;}
static uint32_t get_id(void *p) {(void)p;return view_id;}
static uint32_t get_state(void *p) {(void)p;return state;}
static uint32_t device(void) {return 1;}
static uint32_t target(uint32_t i) {assert(i==4);return 7;}
static uint32_t viewport(uint32_t a,uint32_t b,uint32_t c,void *out) {
 assert(a==1&&b==1&&c==7);memcpy(out,current_viewport,8);return 1;
}
static uint32_t remove_object(void *s,void *o) {assert(s==surface&&o);assert(!step);step=1;removed=!keep_link;if(drop_owner)state=4;return removed;}
static void *find_object(uint32_t id,void *s) {assert(id==0x7fe2&&s==surface&&step==1);return keep_link?(void *)current_view:0;}
static void bind_target(void *o) {assert(removed&&step>=1&&o);step=2;}
static void paint(const void *r,const void *s,const void *b,void *p) {(void)r;(void)s;(void)b;(void)p;assert(removed&&step==2);paints++;}
static void graphics(void) {assert(removed&&step>=2);}
static uint32_t geti(uint32_t k) {(void)k;return 1;}
static void getfv(uint32_t k,int32_t n,void *v) {(void)k;assert(n==4);memset(v,0,16);}
static void seti(uint32_t k,int32_t v) {(void)k;(void)v;}
static void setfv(uint32_t k,int32_t n,const void *v) {(void)k;(void)v;assert(n==4);}
static void clear(int32_t x,int32_t y,int32_t w,int32_t h) {assert(removed&&x==0&&y==0&&w==1280&&h==800);clears++;}
static uint32_t error(void) {return 0;}
static uint32_t draw(void *s,void *a,void *b) {assert(removed&&s==surface&&!a&&!b);draws++;return 1;}
static void setvisible(void *s,uint32_t v) {assert(s==surface&&removed&&v==0);visible++;}
static void enable(uint32_t a,uint32_t b,uint32_t c,uint32_t v) {assert(a==1&&b==1&&c==7&&v==0&&removed);enabled++;}
static void submit(void *p,uint32_t a,uint32_t b) {(void)p;assert(a==3&&b==7&&removed);submits++;}
static void *current_page(void) {foreign_page++;assert(0&&"global detach must never walk current page");return 0;}
#define P(f) ((uintptr_t)(f))
const FxNativeDisplayProfile pb_native_display_profile={
 .ram_begin=0,.ram_end=UINTPTR_MAX,.current_view_pointer=P(&current_view),.shooting_view_id=2,.global_surface_index=4,
 .native_mutex_acquire=P(held),.native_mutex_release=P(held),.native_view_id=P(get_id),.native_view_state=P(get_state),
 .native_graphics_current_device=P(device),.native_global_target=P(target),.native_graphics_backing_rectangle=P(viewport),
 .gui_object_remove=P(remove_object),.native_menu_find_row_by_id=P(find_object),.gui_bind_target=P(bind_target),
 .native_constant_paint_rectangle=P(paint),.native_graphics_acquire=P(graphics),.native_graphics_release=P(graphics),
 .vgGeti=P(geti),.vgGetfv=P(getfv),.vgSeti=P(seti),.vgSetfv=P(setfv),.vgClear=P(clear),.vgGetError=P(error),
 .gui_extended_draw_body=P(draw),.gui_set_visible=P(setvisible),.native_graphics_target_enable=P(enable),
 .native_menu_compositor_submit=P(submit),.native_current_page=P(current_page)
};
void *fx_memcpy(void *d,const void *s,size_t n) {return memcpy(d,s,n);}
void *fx_memset(void *d,int c,size_t n) {return memset(d,c,n);}
int fx_memcmp(const void *a,const void *b,size_t n) {return memcmp(a,b,n);}
extern void pb_native_display_detach(FxNativeDisplay *);
static FxDemoScene scene;
static PbNativeDisplay instance;
static void reset(void) {
 memset(&instance,0,sizeof(instance));memset(surface,0,sizeof(surface));
 current_view=(uintptr_t)native_view;state=1;view_id=2;current_viewport[2]=1280;
 step=removed=keep_link=drop_owner=paints=clears=draws=submits=visible=enabled=foreign_page=0;
 FxNativeDisplay *d=&instance.display;d->attached=1;d->global_overlay=1;
 d->application=current_view;d->surface=surface;d->width=1280;d->height=800;
 scene.width=1280;scene.height=800;d->scene=&scene;
 memcpy(d->viewport,current_viewport,8);d->target[0]=1;d->target[1]=1;d->target[2]=7;
 memcpy(surface+0x108,d->target,12);
 /* A native leaf callback: invalidate only the owned root, no child list. */
 *(uintptr_t *)(void *)(surface+0x48)=1;
}
static void unchanged_target(void) {
 assert(!paints&&!clears&&!draws&&!submits&&!visible&&!enabled&&!foreign_page);
 assert(!instance.display.attached&&instance.unlinks==1&&instance.skipped_owner==1);
}
int main(void) {
 reset();pb_display_detach(&instance);
 assert(!instance.display.attached&&instance.unlinks==1&&!instance.skipped_owner);
 assert(paints==1&&clears==1&&draws==1&&submits==1&&visible==1&&enabled==1&&!foreign_page);
 reset();current_view=(uintptr_t)other_view;pb_display_detach(&instance);unchanged_target();
 reset();state=4;pb_display_detach(&instance);unchanged_target();
 reset();current_viewport[2]=1024;pb_display_detach(&instance);unchanged_target();
 reset();((uint32_t *)(void *)(surface+0x108))[2]=8;pb_display_detach(&instance);unchanged_target();
 reset();drop_owner=1;pb_display_detach(&instance);unchanged_target();
 reset();keep_link=1;current_view=(uintptr_t)instance.display.object;pb_display_detach(&instance);
 assert(instance.display.attached&&!instance.unlinks&&!instance.skipped_owner&&!paints&&!submits);
 puts("{\"native_detach_contract\":true,\"unlink_before_clear\":true,\"owner_state_tuple_viewport_guards\":true,\"failed_unlink_retained\":true,\"no_foreign_page_walk\":true}");
}
