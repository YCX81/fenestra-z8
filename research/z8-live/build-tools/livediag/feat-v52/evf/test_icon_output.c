#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "fenestra_native_display.h"
static _Alignas(8) uint8_t surface[0x118],view[0x118],descriptor[0x208];
static uintptr_t current_view,callback=0x40325980;
static uint32_t selected=4,selects;
static uint32_t output(void){return selected;}
static const void *desc(uint32_t g,uint32_t o,uint32_t t){assert(g==1&&o==4&&t==4);return descriptor;}
static uint32_t select_native(uint32_t g,uint32_t o,uint32_t t,uint32_t locked){assert(g==1&&o==4&&t==4&&locked==1);selects++;return 1;}
static uintptr_t resolve(uintptr_t a){switch(a){case 0x40398570:return(uintptr_t)output;case 0x4033d8cc:return(uintptr_t)desc;case 0x4033e748:return(uintptr_t)select_native;case 0xa02f35f50:return(uintptr_t)&callback;default:assert(0);return 0;}}
#define ICON32_NATIVE_ADDR(a) resolve(a)
#include "icon32_alpha.h"
static uint32_t id(void*v){assert(v==view);return 3;}
static uint32_t state(void*v){assert(v==view);return 1;}
static uint32_t group(void){return 1;}
static uint32_t target(uint32_t i){assert(i==4);return 4;}
static void* root(void*p,uint32_t i){assert(!p&&i==4);return surface;}
int main(void){
 FxNativeDisplay d={0};FxNativeDisplayProfile p={0};
 current_view=(uintptr_t)view;d.application=current_view;d.global_overlay=1;d.surface=surface;d.width=1920;d.height=1080;d.target[0]=1;d.target[1]=4;d.target[2]=4;
 memcpy(surface+0x108,d.target,12);*(uintptr_t*)(void*)(descriptor+0x28)=0x12340000;*(uint16_t*)(void*)(descriptor+8)=1920;*(uint16_t*)(void*)(descriptor+10)=1080;
 p.current_view_pointer=(uintptr_t)&current_view;p.shooting_view_id=3;p.global_surface_index=4;p.native_view_id=(uintptr_t)id;p.native_view_state=(uintptr_t)state;p.native_global_surface=(uintptr_t)root;p.native_graphics_current_device=(uintptr_t)group;p.native_global_target=(uintptr_t)target;
 assert(icon32_owner(&d,&p));assert(icon32_select_locked(&d)==0x12340000&&selects==1);
 selected=1;assert(!icon32_owner(&d,&p));
 d.target[1]=3;assert(!icon32_select_locked(&d)&&selects==1);
 d.target[1]=4;*(uint16_t*)(void*)(descriptor+8)=1280;assert(!icon32_select_locked(&d)&&selects==1);
 puts("{\"actual_icon32_header\":true,\"checks\":5,\"evf_output4_allowed\":true,\"stale_output_rejected\":true,\"wrong_dimensions_rejected\":true}");
}
