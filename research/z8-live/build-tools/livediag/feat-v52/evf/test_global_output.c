#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "native_ui.h"
static _Alignas(8) unsigned char root[0x118],saved[0x118];
static uint32_t selected=1,target=4,children,valid=1,constructs,resizes,queries,enables,enabled_reads;
static uint16_t vp[4]={0,0,1280,720};
static uint32_t group(void){return 1;}
static uint32_t output(void){return selected;}
static uint32_t gettarget(uint32_t i){assert(i==4);return target;}
static uint32_t backing(uint32_t o,uint32_t g,uint32_t t,void*r){assert(o==selected&&g==1&&t==target);queries++;memcpy(r,vp,8);return valid;}
static uint32_t count(void*p){assert(p==root+0xc8);return children;}
static void bounds(void*p,const void*b){assert(p==root&&!children);resizes++;memcpy(root+0x10,b,8);memcpy(root+0x60,b,8);root[0x3c]=1;}
static void construct(void*p,const void*d){assert(p==root&&!children);constructs++;memcpy(root,d,0x18);memcpy(root+0x60,(const uint8_t*)d+0x10,8);memcpy(root+0x108,(const uint8_t*)d+0x70,12);*(uintptr_t*)(void*)(root+0x48)=0x40325a80;*(uintptr_t*)(void*)(root+0xb0)=0x4032987c;}
static uint32_t getenable(uint32_t o,uint32_t g,uint32_t t){assert(o==4&&g==1&&t==3);enabled_reads++;return 1;}
static void enable(uint32_t o,uint32_t g,uint32_t t,uint32_t e){assert(o==4&&g==1&&t==3&&e==0);enables++;}
static uintptr_t resolve(uintptr_t a){switch(a){
#define A(n,f) case n:return (uintptr_t)f
 A(0x40398570,output);A(0x403f1b18,group);A(0x403f0db8,gettarget);A(0x40340d64,backing);A(0x40c7a304,count);A(0x40325e0c,bounds);A(0x403297e4,construct);A(0x403409d4,getenable);A(0x403409a4,enable);
 default:assert(0);return 0;}}
#define PB_OUTPUT_NATIVE(a,t) ((t)resolve(a))
#include "global_output.h"
static void same(void){assert(!memcmp(root,saved,sizeof(root)));}
static void snapshot(void){memcpy(saved,root,sizeof(root));}
int main(void){unsigned checks=0;
 assert(pb_output_initialize_global(root)==FX_OK&&constructs==1);assert(pb_output_valid(root));checks++;
 assert(((uint32_t*)(void*)(root+0x108))[1]==1);checks++;
 snapshot();assert(pb_output_initialize_global(root)==FX_OK&&constructs==1&&!resizes);same();checks++;
 selected=4;vp[2]=1920;vp[3]=1080;children=1;snapshot();assert(pb_output_initialize_global(root)==FX_BUSY);same();checks++;
 children=0;memset(root+0xc8,0x6d,0x40);snapshot();assert(pb_output_initialize_global(root)==FX_OK&&resizes==1&&constructs==1);assert(!memcmp(root+0xc8,saved+0xc8,0x40));assert(pb_output_valid(root));checks++;
 assert(((uint32_t*)(void*)(root+0x108))[1]==4&&((uint16_t*)(void*)(root+0x10))[2]==1920);checks++;
 uint16_t r[4];assert(pb_output_backing_rectangle(1,4,4,r)==1);checks++;
 selected=1;unsigned before=queries;assert(!pb_output_valid(root));assert(!pb_output_backing_rectangle(1,4,4,r)&&queries==before);checks++;
 vp[2]=1280;vp[3]=720;assert(pb_output_initialize_global(root)==FX_OK&&resizes==2&&pb_output_valid(root));checks++;
 target=3;assert(pb_output_initialize_global(root)==FX_OK&&resizes==3);checks++;
 children=5;snapshot();assert(pb_output_initialize_global(root)==FX_OK);same();checks++;
 selected=4;*(uint32_t*)(void*)root=0x1234;snapshot();assert(pb_output_initialize_global(root)==FX_OK);same();checks++;
 *(uint32_t*)(void*)root=0x7fdf;*(uintptr_t*)(void*)(root+0x48)=0;children=0;snapshot();assert(pb_output_initialize_global(root)==FX_CAPABILITY);same();checks++;
 *(uintptr_t*)(void*)(root+0x48)=0x40325a80;*(uintptr_t*)(void*)(root+0x40)=8;snapshot();assert(pb_output_initialize_global(root)==FX_CAPABILITY);same();checks++;
 *(uintptr_t*)(void*)(root+0x40)=0;valid=0;snapshot();assert(pb_output_initialize_global(root)==FX_CAPABILITY);same();checks++;
 valid=1;vp[2]=0;snapshot();assert(pb_output_initialize_global(root)==FX_CAPABILITY);same();checks++;
 vp[2]=4097;snapshot();assert(pb_output_initialize_global(root)==FX_CAPABILITY);same();checks++;
 vp[2]=1280;selected=2;snapshot();assert(pb_output_initialize_global(root)==FX_CAPABILITY);same();checks++;
 assert(pb_output_target_enabled(1,4,3)==1&&enabled_reads==1);pb_output_target_enable(1,4,3,0);assert(enables==1);checks++;
 printf("{\"global_output_actual_source\":true,\"checks\":%u,\"lcd_evf_lcd\":true,\"foreign_untouched\":true,\"nonempty_refused\":true,\"list_cache_retained\":true,\"native_argument_order\":true}\n",checks);
}
