/* portrait.wlm (V67 localized): switch for the portrait-video layer built into MAIN.
 * The base keeps the layer gated off after every boot (stock tkhd and video OSD).
 * Once Fenestra activates this module it turns the gate on through slots.state ops 13/14.
 * Queries slots.state op 15 (SLOTS_LANG_GET, magic 'LANG') to match Nikon's active UI
 * language (8 = zh-CN, 9 = zh-TW, else English), gracefully falling back to English on
 * older bases without op 15. */
#include "fenestra_sdk.h"
enum {PAGE=1u,R_STATE,R_NOTE,CONTROLS=R_NOTE};
enum {PORTRAIT_SET=13u,PORTRAIT_GET=14u,SLOTS_LANG_GET=15u};
#define PORTRAIT_MAGIC 0x50525431u
#define LANG_MAGIC     0x4c414e47u
#define LANG_ZH_CN     8u
#define LANG_ZH_TW     9u
#define LANG_POLL_MS   1000u
typedef struct {uint32_t index,offset,bytes,value;} SlotsRequest;
static FxSdkClient client;
static FxServiceReply reply;
static uint64_t slots,last_lang_ms;
static uint32_t pc,row,want,gate,fault,ui_lang=1u;
static char text[CONTROLS+1u][48];

#define CALL(e) CALL_(e,__COUNTER__)
#define CALL_(e,n) do{pc=2u*(n)+1u;case 2u*(n)+1u:{FxResult t_=(e);if(t_==FX_BUSY)return 0;if(t_!=FX_OK)return 5;}\
    pc=2u*(n)+2u;return 0;case 2u*(n)+2u:if(reply.result==FX_BUSY){pc=2u*(n)+1u;return 0;}}while(0)

static uint32_t t_add(char *o,uint32_t at,const char *s){
    while(*s && at<47u)o[at++]=*s++;
    o[at]=0;return at;
}
static const char *page_title(void){
    if(ui_lang==LANG_ZH_CN)return "\xe7\xab\x96\xe5\xb1\x8f\xe5\xbd\x95\xe5\x83\x8f"; /* 竖屏录像 */
    if(ui_lang==LANG_ZH_TW)return "\xe7\x9b\xb4\xe6\x8b\x8d\xe9\x8c\x84\xe5\xbd\xb1"; /* 直拍錄影 */
    return "Portrait video";
}
static void set_text(void){
    if(ui_lang==LANG_ZH_CN){
        t_add(text[R_STATE],0u,fault?"\xe7\xab\x96\xe5\xb1\x8f\xe5\xbd\x95\xe5\x83\x8f: \xe4\xb8\x8d\xe5\x8f\xaf\xe7\x94\xa8"
            :gate?"\xe7\xab\x96\xe5\xb1\x8f\xe5\xbd\x95\xe5\x83\x8f: \xe5\xbc\x80\xe5\x90\xaf"
                 :"\xe7\xab\x96\xe5\xb1\x8f\xe5\xbd\x95\xe5\x83\x8f: \xe5\x85\xb3\xe9\x97\xad");
        t_add(text[R_NOTE],0u,"\xe6\x8c\x89 OK \xe5\x88\x87\xe6\x8d\xa2 (\xe9\x87\x8d\xe5\x90\xaf\xe5\x90\x8e\xe6\x81\xa2\xe5\xa4\x8d\xe5\xbc\x80\xe5\x90\xaf)");
    } else if(ui_lang==LANG_ZH_TW){
        t_add(text[R_STATE],0u,fault?"\xe7\x9b\xb4\xe6\x8b\x8d\xe9\x8c\x84\xe5\xbd\xb1: \xe4\xb8\x8d\xe5\x8f\xaf\xe7\x94\xa8"
            :gate?"\xe7\x9b\xb4\xe6\x8b\x8d\xe9\x8c\x84\xe5\xbd\xb1: \xe9\x96\x8b\xe5\x95\x9f"
                 :"\xe7\x9b\xb4\xe6\x8b\x8d\xe9\x8c\x84\xe5\xbd\xb1: \xe9\x97\x9c\xe9\x96\x89");
        t_add(text[R_NOTE],0u,"\xe6\x8c\x89 OK \xe5\x88\x87\xe6\x8f\x9b (\xe9\x87\x8d\xe5\x95\x9f\xe5\xbe\x8c\xe6\x81\xa2\xe5\xbe\xa9\xe9\x96\x8b\xe5\x95\x9f)");
    } else {
        t_add(text[R_STATE],0u,fault?"Portrait video: unavailable":gate?"Portrait video: On":"Portrait video: Off");
        t_add(text[R_NOTE],0u,"OK toggles until restart");
    }
}
static FxResult put(const FxServiceModule *api,uint32_t id){
    FxUiControl c={.abi=1u,.bytes=sizeof(c),.id=id,.parent=id==PAGE?0u:PAGE,.kind=id==PAGE?FX_UI_PAGE:FX_UI_ACTION,
        .flags=FX_UI_VISIBLE|FX_UI_ENABLED};
    t_add(c.label,0u,id==PAGE?page_title():text[id]);
    return fx_sdk_ui(&client,api,&c);
}
static FxResult portrait_call(const FxServiceModule *api,uint32_t op,uint32_t value){
    SlotsRequest q={0u,0u,0u,value};
    return fx_sdk_platform(&client,api,FX_BIND_CALL,slots,op,0u,&q,sizeof(q));
}
int fenestra_module_entry(const FxModuleServices *base){
    if(!base || base->bytes!=sizeof(FxServiceModule))return 1;
    const FxServiceModule *api=(const FxServiceModule *)base;
    if(api->service_abi!=FX_SERVICE_ABI)return 2;
    if(client.pending){
        FxResult taken=fx_sdk_take(&client,api,&reply);
        if(taken==FX_BUSY)return 0;
        if(taken!=FX_OK)return 3;
    }
    switch(pc){
    case 0u:
        CALL(fx_sdk_open(&client,api,"slots.state",1u));
        if(reply.result!=FX_OK){
            fault=1u;set_text();
            for(row=PAGE;row<=CONTROLS;++row){CALL(put(api,row));}
            for(;;){CALL(fx_sdk_events(&client,api));}
        }
        slots=reply.value[0];
        CALL(portrait_call(api,SLOTS_LANG_GET,0u));
        if(reply.result==FX_OK && reply.value[1]==LANG_MAGIC)ui_lang=(uint32_t)reply.value[0];
        last_lang_ms=api->time_ms;
        set_text();
        for(row=PAGE;row<=CONTROLS;++row){CALL(put(api,row));if(reply.result!=FX_OK)return 4;}
        want=1u;
        for(;;){
            if(want!=2u){
                CALL(portrait_call(api,PORTRAIT_SET,want));
                fault=reply.result!=FX_OK;
                CALL(portrait_call(api,PORTRAIT_GET,0u));
                /* A base without the gate answers FX_ARGUMENT: say so instead of claiming On. */
                if(reply.result!=FX_OK || reply.value[1]!=PORTRAIT_MAGIC)fault=1u;
                else gate=(uint32_t)reply.value[0];
                want=2u;set_text();CALL(put(api,R_STATE));
            }
            CALL(fx_sdk_events(&client,api));
            if(reply.result==FX_OK && reply.bytes==sizeof(FxUiEvent)){
                FxUiEvent e;uint8_t *p=(uint8_t *)&e;
                for(uint32_t k=0u;k<sizeof(e);++k)p[k]=reply.data[k];
                if(e.event==FX_INPUT_ACTIVATE && e.control==R_STATE && !fault)want=gate?0u:1u;
            }
            if(api->time_ms-last_lang_ms>=LANG_POLL_MS){
                last_lang_ms=api->time_ms;
                CALL(portrait_call(api,SLOTS_LANG_GET,0u));
                if(reply.result==FX_OK && reply.value[1]==LANG_MAGIC && (uint32_t)reply.value[0]!=ui_lang){
                    ui_lang=(uint32_t)reply.value[0];
                    set_text();
                    for(row=PAGE;row<=CONTROLS;++row){CALL(put(api,row));}
                }
            }
        }
    default:return 9;
    }
}
