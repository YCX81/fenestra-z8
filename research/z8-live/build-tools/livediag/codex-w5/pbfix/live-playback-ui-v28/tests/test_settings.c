#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "settings.h"
#include "runtime.h"
static PbRuntime runtime;
static FxNativeMenu core;
static uint8_t header[340];
static uint32_t key=9,original_text;
void *fx_memset(void*p,int c,size_t n){return memset(p,c,n);}
void *fx_memcpy(void*p,const void*q,size_t n){return memcpy(p,q,n);}
PbRuntime *pb_runtime_get(void){return &runtime;}
uint32_t pb_get_key(void){return key;}
void pb_set_key(uint32_t k){assert(k==0||k==9);key=k;}
FxNativePage *pb_settings_old_menu_page(uint32_t group,uint32_t id){return group==2&&id==0x7000?&core.page:NULL;}
void pb_settings_old_menu_text(int32_t lang,int32_t id,const uint16_t **v,uint32_t*n){(void)lang;(void)id;(void)v;(void)n;original_text++;}
int main(void){
 void *m=mmap((void*)0x43bff000,0x2000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);assert(m!=(void*)-1);
 FxNativeEvent *nav=(void*)0x43bff8e8;
 unsigned ev[]={0x286,0x28b,0x29f,0x2ae,0x285,0x3bb};
 for(unsigned i=0;i<6;i++)nav[i]=(FxNativeEvent){ev[i],0xabc+i,0x10+i,0x100+i};
 PbSettings state={0};runtime.settings=&state;
 core.page.identity=0x70000002;core.page.descriptor=(uintptr_t)&core.descriptor;
 core.descriptor.header=(uintptr_t)header;core.descriptor.rows=(uintptr_t)core.rows;core.descriptor.events=(uintptr_t)core.events;core.descriptor.count=7;
 for(unsigned i=0;i<7;i++){core.rows[i].word[0]=3154+i;core.rows[i].word[6]=0x10+i;core.events[i]=0x300+i;}
 core.rows[0].word[0]=3156;core.rows[6].word[0]=3158;
 for(unsigned i=0;i<5;i++)core.callbacks[i]=0x100+i;
 FxNativeRow original[7];memcpy(original,core.rows,sizeof(original));uintptr_t back=core.events[6];
 assert(pb_settings_install(&state));assert(core.descriptor.count==8);
 for(unsigned i=0;i<6;i++)assert(!memcmp(&original[i],&core.rows[i],sizeof(original[0])));
 assert(!memcmp(&original[6],&core.rows[7],sizeof(original[0]))&&core.events[7]==back);
 assert(core.rows[6].word[6]==PB_SETTINGS_ROW);
 assert(pb_settings_menu_page(2,PB_SETTINGS_PAGE)==&state.choices.page);
 assert(state.choices.page.selected==0);
 uint32_t(*off)(void*)=(void*)(uintptr_t)state.choices.handlers[1][0].callback;assert(off(0)==1&&key==0);
 assert(pb_settings_menu_page(2,PB_SETTINGS_PAGE)->selected==1);
 uint32_t(*rec)(void*)=(void*)(uintptr_t)state.choices.handlers[0][0].callback;assert(rec(0)==1&&key==9);
 for(unsigned row=0;row<2;row++){assert(state.choices.handlers[row][4].next==0x7000);assert(state.choices.handlers[row][5].event==0x3bb);}
 const uint16_t *text=0;uint32_t bytes=0;pb_settings_menu_text(1,3161,&text,&bytes);assert(bytes==6&&text[0]=='R');
 pb_settings_menu_text(1,100,&text,&bytes);assert(original_text==1);
 puts("SETTINGS_HOST_PASS original service rows/Back preserved, native REC/Off actions, selection and text dispatch");
}
