/* Card-stored Picture Control slots (V67 localized).
 * After activation: load the store from A:\FENESTRA, inject the records, restore
 * the selection, then watch for changes and commit them as new generations.
 * Queries slots.state op 15 (SLOTS_LANG_GET, magic 'LANG') to match Nikon's active
 * UI language (8 = zh-CN, 9 = zh-TW, else English), gracefully falling back to
 * English on older bases without op 15. */
#include "fenestra_sdk.h"
#include "fenestra_file_sdk.h"
#include "slotcard.h"
#define POLL_MS 2000u
#define CHUNK 700u
#define DEFAULT_PHOTO 0x258u
#define LANG_MAGIC 0x4c414e47u
#define LANG_ZH_CN 8u
#define LANG_ZH_TW 9u
enum {PAGE=1u,R_STATE,R_STORE,R_LIMIT,CONTROLS=R_LIMIT};
enum {SLOTS_READ=1u,SLOTS_WRITE,SLOTS_COMMIT,SLOTS_SELECTION,SLOTS_APPLY,SLOTS_INVALIDATE,SLOTS_SUMMARY,SLOTS_HASH,SLOTS_STATUS_SET,SLOTS_INFO,SLOTS_LIMIT,SLOTS_PROXY,SLOTS_LANG_GET=15u};
enum {ST_START,ST_LOADING,ST_INJECTING,ST_APPLYING,ST_RUNNING,ST_OTHER_CARD};
typedef struct {uint32_t index,offset,bytes,value;} SlotsRequest;
static FxSdkClient client;
static uint64_t files,slots,handle;
static Sc store;
static ScIo io;
static ScReply answer;
static FxServiceReply reply;
static uint32_t pc,row,i,offset,seen,moved,held,sent_proxy[2],proxy_sent;
static uint32_t base_crc[SC_SLOTS],summary,pending_summary,selected[2],legal[2];
static uint32_t state,load_status,injected,inject_failed,applied,apply_failed,commits,commit_status,checks_other,used_count,want_commit;
static uint32_t ui_lang=1u;
static uint64_t last_poll;
static uint8_t record[SC_RECORD];
static char text[CONTROLS+1u][48];

#define CALL(e) CALL_(e,__COUNTER__)
#define CALL_(e,n) do{pc=2u*(n)+1u;case 2u*(n)+1u:{FxResult t_=(e);if(t_==FX_BUSY)return 0;if(t_!=FX_OK)return 5;}\
    pc=2u*(n)+2u;return 0;case 2u*(n)+2u:if(reply.result==FX_BUSY){pc=2u*(n)+1u;return 0;}}while(0)
#define RUN_MACHINE() do{while(sc_next(&store,&answer,&io)==SC_RUN){CALL(submit_io(api));map_reply();}}while(0)

static uint32_t slots_active,limit; /* built capacity; limit in effect this boot */
static int active(uint32_t index){uint32_t slot=index%64u;return slot>=9u && slot<limit;}
static uint32_t t_add(char *o,uint32_t at,const char *s){
    while(*s && at<47u)o[at++]=*s++;
    o[at]=0;return at;
}
static uint32_t t_num(char *o,uint32_t at,uint32_t v){
    char d[12];uint32_t n=0u;
    do{d[n++]=(char)('0'+v%10u);v/=10u;}while(v);
    while(n && at<47u)o[at++]=d[--n];
    o[at]=0;return at;
}
static const char *page_title(void){
    if(ui_lang==LANG_ZH_CN)return slots_active==32u?"C32 \xe6\xbb\xa4\xe9\x95\x9c\xe6\xa7\xbd":"C64 \xe6\xbb\xa4\xe9\x95\x9c\xe6\xa7\xbd";
    if(ui_lang==LANG_ZH_TW)return slots_active==32u?"C32 \xe6\xbf\xbe\xe9\x8f\xa1\xe6\xa7\xbd":"C64 \xe6\xbf\xbe\xe9\x8f\xa1\xe6\xa7\xbd";
    return "Slot card";
}
static const char *state_label_zh(uint32_t st,int tw){
    static const char *const sc[6]={
        "\xe5\x90\xaf\xe5\x8a\xa8\xe4\xb8\xad", /* 启动中 */
        "\xe8\xaf\xbb\xe5\x8f\x96\xe4\xb8\xad", /* 读取中 */
        "\xe5\x86\x99\xe5\x85\xa5\xe4\xb8\xad", /* 写入中 */
        "\xe5\xba\x94\xe7\x94\xa8\xe4\xb8\xad", /* 应用中 */
        "\xe8\xbf\x90\xe8\xa1\x8c\xe4\xb8\xad", /* 运行中 */
        "\xe5\xb7\xb2\xe6\x8d\xa2\xe5\x8d\xa1"  /* 已换卡 */
    };
    static const char *const tc[6]={
        "\xe5\x95\x9f\xe5\x8b\x95\xe4\xb8\xad", /* 啟動中 */
        "\xe8\xae\x80\xe5\x8f\x96\xe4\xb8\xad", /* 讀取中 */
        "\xe5\xaf\xab\xe5\x85\xa5\xe4\xb8\xad", /* 寫入中 */
        "\xe5\xa5\x97\xe7\x94\xa8\xe4\xb8\xad", /* 套用中 */
        "\xe9\x81\x8b\xe8\xa1\x8c\xe4\xb8\xad", /* 運行中 */
        "\xe5\xb7\xb2\xe6\x8f\x9b\xe5\x8d\xa1"  /* 已換卡 */
    };
    uint32_t idx=st<6u?st:0u;
    return tw?tc[idx]:sc[idx];
}
static FxResult put(const FxServiceModule *api,uint32_t id){
    FxUiControl c={.abi=1u,.bytes=sizeof(c),.id=id,.parent=id==PAGE?0u:PAGE,.kind=id==PAGE?FX_UI_PAGE:FX_UI_ACTION,
        .flags=FX_UI_VISIBLE|FX_UI_ENABLED};
    t_add(c.label,0u,id==PAGE?page_title():text[id]);
    return fx_sdk_ui(&client,api,&c);
}
static FxResult slot_call(const FxServiceModule *api,uint32_t op,uint32_t index,uint32_t at,uint32_t bytes,uint32_t value,const void *data){
    struct {SlotsRequest r;uint8_t data[CHUNK];} q;
    q.r=(SlotsRequest){index,at,bytes,value};
    uint32_t size=(uint32_t)sizeof(SlotsRequest);
    if(data){const uint8_t *p=data;for(uint32_t k=0u;k<bytes;++k)q.data[k]=p[k];size+=bytes;}
    return fx_sdk_platform(&client,api,FX_BIND_CALL,slots,op,0u,&q,size);
}
static FxResult file_call(const FxServiceModule *api,uint32_t op,uint64_t h,uint32_t bytes,uint32_t mode,const char *name,const uint8_t *payload){
    struct {FxFileRequest r;uint8_t data[SC_WIRE];} q;
    uint8_t *raw=(uint8_t *)&q;
    for(uint32_t k=0u;k<sizeof(FxFileRequest);++k)raw[k]=0u;
    q.r.handle=h;q.r.bytes=bytes;q.r.mode=mode;
    for(uint32_t k=0u;name && name[k];++k)q.r.name[k]=name[k];
    uint32_t size=(uint32_t)sizeof(FxFileRequest);
    if(payload){for(uint32_t k=0u;k<bytes;++k)q.data[k]=payload[k];size+=bytes;}
    return fx_sdk_platform(&client,api,FX_BIND_CALL,files,op,0u,&q,size);
}
static FxResult submit_io(const FxServiceModule *api){
    char name[16];
    switch(io.op){
    case SC_IO_OPEN_READ:sc_name(io.gen,name);return file_call(api,FX_FILE_OPEN,0u,0u,FX_FILE_READ_ONLY,name,0);
    case SC_IO_CREATE:sc_name(io.gen,name);return file_call(api,FX_FILE_OPEN,0u,0u,FX_FILE_CREATE_EXCLUSIVE,name,0);
    case SC_IO_READ:return file_call(api,FX_FILE_READ,handle,io.bytes,0u,0,0);
    case SC_IO_WRITE:return file_call(api,FX_FILE_WRITE,handle,io.bytes,0u,0,io.write);
    case SC_IO_CLOSE:return file_call(api,FX_FILE_CLOSE,handle,0u,0u,0,0);
    default:return FX_ARGUMENT;
    }
}
static void map_reply(void){
    answer=(ScReply){SC_R_FAILED,0u,0u};
    if(reply.result==FX_EMPTY)answer.result=SC_R_MISSING;
    else if(reply.result==FX_OK) {
        answer.result=SC_R_OK;
        if(io.op==SC_IO_OPEN_READ || io.op==SC_IO_CREATE)handle=reply.value[0];
        if(io.op==SC_IO_WRITE)answer.done=(uint32_t)reply.value[0];
        if(io.op==SC_IO_READ) {
            answer.done=reply.bytes;
            if(reply.bytes<=io.bytes)for(uint32_t k=0u;k<reply.bytes;++k)io.read[k]=reply.data[k];
        }
    }
}
static FxResult publish_status(const FxServiceModule *api){
    uint32_t v[18]={UINT32_C(0x53434431),state,load_status,store.head,used_count,store.bad_slots,store.files_skipped,injected,
        inject_failed,applied,apply_failed,commits,commit_status,selected[0],selected[1],checks_other,limit,store.next_limit};
    return slot_call(api,SLOTS_STATUS_SET,0u,0u,(uint32_t)sizeof(v),0u,v);
}
static void set_text(void){
    uint32_t lim=store.next_limit?store.next_limit:limit;
    if(ui_lang==LANG_ZH_CN){
        uint32_t a=t_add(text[R_STATE],0u,"\xe7\x8a\xb6\xe6\x80\x81 "); /* 状态  */
        a=t_add(text[R_STATE],a,state_label_zh(state,0));
        a=t_add(text[R_STATE],a," \xe5\xb7\xb2\xe7\x94\xa8 "); /*  已用  */
        a=t_num(text[R_STATE],a,used_count);
        a=t_add(text[R_STATE],a," \xe5\xbc\x82\xe5\xb8\xb8 "); /*  异常  */
        t_num(text[R_STATE],a,store.bad_slots);

        a=t_add(text[R_STORE],0u,"\xe5\xad\x98\xe6\xa1\xa3\xe4\xbb\xa3 "); /* 存档代  */
        a=t_num(text[R_STORE],a,store.head);
        a=t_add(text[R_STORE],a," \xe6\x8f\x90\xe4\xba\xa4 "); /*  提交  */
        t_num(text[R_STORE],a,commits);

        a=t_add(text[R_LIMIT],0u,"\xe6\xa7\xbd\xe4\xbd\x8d\xe4\xb8\x8a\xe9\x99\x90 C"); /* 槽位上限 C */
        a=t_num(text[R_LIMIT],a,lim);
        if(store.next_limit && store.next_limit!=limit)t_add(text[R_LIMIT],a," (\xe9\x87\x8d\xe5\x90\xaf\xe7\x94\x9f\xe6\x95\x88)");
    } else if(ui_lang==LANG_ZH_TW){
        uint32_t a=t_add(text[R_STATE],0u,"\xe7\x8b\x80\xe6\x85\x8b "); /* 狀態  */
        a=t_add(text[R_STATE],a,state_label_zh(state,1));
        a=t_add(text[R_STATE],a," \xe5\xb7\xb2\xe7\x94\xa8 "); /*  已用  */
        a=t_num(text[R_STATE],a,used_count);
        a=t_add(text[R_STATE],a," \xe7\x95\xb0\xe5\xb8\xb8 "); /*  異常  */
        t_num(text[R_STATE],a,store.bad_slots);

        a=t_add(text[R_STORE],0u,"\xe5\xad\x98\xe6\xaa\x94\xe4\xbb\xa3 "); /* 存檔代  */
        a=t_num(text[R_STORE],a,store.head);
        a=t_add(text[R_STORE],a," \xe6\x8f\x90\xe4\xba\xa4 "); /*  提交  */
        t_num(text[R_STORE],a,commits);

        a=t_add(text[R_LIMIT],0u,"\xe6\xa7\xbd\xe4\xbd\x8d\xe4\xb8\x8a\xe9\x99\x90 C"); /* 槽位上限 C */
        a=t_num(text[R_LIMIT],a,lim);
        if(store.next_limit && store.next_limit!=limit)t_add(text[R_LIMIT],a," (\xe9\x87\x8d\xe5\x95\x9f\xe7\x94\x9f\xe6\x95\x88)");
    } else {
        uint32_t a=t_add(text[R_STATE],0u,"State ");a=t_num(text[R_STATE],a,state);
        a=t_add(text[R_STATE],a," used ");a=t_num(text[R_STATE],a,used_count);
        a=t_add(text[R_STATE],a," bad ");t_num(text[R_STATE],a,store.bad_slots);
        a=t_add(text[R_STORE],0u,"Head ");a=t_num(text[R_STORE],a,store.head);
        a=t_add(text[R_STORE],a," commits ");t_num(text[R_STORE],a,commits);
        /* The limit takes effect at the next boot; the row says so until then. */
        a=t_add(text[R_LIMIT],0u,"Slot limit C");a=t_num(text[R_LIMIT],a,lim);
        if(store.next_limit && store.next_limit!=limit)t_add(text[R_LIMIT],a," (next boot)");
    }
}
static int selector_extended(uint32_t v){return sc_selector_extended(v,0);}
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
        sc_init(&store,1u,0u);
        CALL(fx_sdk_open(&client,api,"card.files",1u));if(reply.result!=FX_OK)return 6;files=reply.value[0];
        CALL(fx_sdk_open(&client,api,"slots.state",1u));if(reply.result!=FX_OK)return 7;slots=reply.value[0];
        CALL(slot_call(api,SLOTS_INFO,0u,0u,0u,0u,0));
        if(reply.result!=FX_OK || (reply.value[0]!=32u && reply.value[0]!=64u))return 10;
        slots_active=limit=(uint32_t)reply.value[0];
        CALL(slot_call(api,SLOTS_LANG_GET,0u,0u,0u,0u,0));
        if(reply.result==FX_OK && reply.value[1]==LANG_MAGIC)ui_lang=(uint32_t)reply.value[0];
        set_text();
        for(row=PAGE;row<=CONTROLS;++row){CALL(put(api,row));if(reply.result!=FX_OK)return 4;}
        state=ST_LOADING;CALL(publish_status(api));

        sc_begin_load(&store);answer=(ScReply){0};RUN_MACHINE();
        load_status=store.status;
        if(store.limit==32u && slots_active==64u){
            CALL(slot_call(api,SLOTS_LIMIT,0u,0u,0u,32u,0));
            if(reply.result==FX_OK)limit=32u;
        }
        store.next_limit=store.limit;
        store.next_selection[0]=store.selection[0];store.next_selection[1]=store.selection[1];
        store.next_proxy[0]=store.proxy[0];store.next_proxy[1]=store.proxy[1];
        used_count=0u;for(i=0u;i<SC_SLOTS;++i)used_count+=store.used[i];

        state=ST_INJECTING;CALL(publish_status(api));
        for(i=0u;i<SC_SLOTS;++i){
            if(!store.used[i] || store.bad[i] || !active(i))continue;
            for(offset=0u;offset<SC_RECORD;offset+=CHUNK){
                seen=SC_RECORD-offset;if(seen>CHUNK)seen=CHUNK;
                CALL(slot_call(api,SLOTS_WRITE,i,offset,seen,0u,store.records[i]+offset));
                if(reply.result!=FX_OK)break;
            }
            /* A stage that did not fill completely must not reach live state. */
            if(offset<SC_RECORD){++inject_failed;continue;}
            CALL(slot_call(api,SLOTS_COMMIT,i,0u,0u,0u,0));
            if(reply.result==FX_OK)++injected;else ++inject_failed;
        }
        if(injected){CALL(slot_call(api,SLOTS_INVALIDATE,0u,0u,0u,0u,0));}

        state=ST_APPLYING;CALL(publish_status(api));
        CALL(slot_call(api,SLOTS_SELECTION,0u,0u,0u,0u,0));
        selected[0]=(uint32_t)reply.value[0];selected[1]=reply.value[2]?0u:(uint32_t)reply.value[1];
        for(i=0u;i<2u;++i)legal[i]=sc_selector_legal(i,selected[i])?selected[i]:(store.proxy[i]?store.proxy[i]:(i?0u:DEFAULT_PHOTO));
        moved=0u;held=0u;
        for(i=0u;i<2u;++i){
            uint32_t want=store.selection[i],index;
            if(!sc_selector_extended(want,&index)){moved|=1u<<i;continue;}
            /* Hidden only by the limit: keep it for when the limit goes back up. */
            if(store.used[index] && !store.bad[index] && !active(index) && index%64u<slots_active)held|=1u<<i;
            if(!store.used[index] || store.bad[index] || !active(index))continue;
            /* Something other than the saved fallback or the default means the user moved during the window. */
            if(selected[i]!=store.proxy[i] && selected[i]!=(i?0u:DEFAULT_PHOTO)){moved|=1u<<i;continue;}
            CALL(slot_call(api,SLOTS_APPLY,i,0u,0u,store.selection[i],0));
            if(reply.result==FX_OK)++applied;else ++apply_failed;
        }
        CALL(slot_call(api,SLOTS_SELECTION,0u,0u,0u,0u,0));
        selected[0]=(uint32_t)reply.value[0];selected[1]=reply.value[2]?0u:(uint32_t)reply.value[1];
        /* The user's own choice in the boot window is what the next generation must hold. */
        for(i=0u;i<2u;++i)if(moved>>i&1u && selected[i]!=store.selection[i]){
            store.next_selection[i]=selected[i];
            store.next_proxy[i]=sc_selector_legal(i,selected[i])?selected[i]:legal[i];
        }

        /* Baseline is what the store holds, not what RAM holds now: a record created
         * while loading differs from it and is picked up by the first poll. */
        for(i=0u;i<SC_SLOTS;++i)base_crc[i]=store.used[i] && !store.bad[i]?sc_crc32(0u,store.records[i],SC_RECORD):0u;
        CALL(slot_call(api,SLOTS_SUMMARY,0u,0u,0u,0u,0));summary=pending_summary=(uint32_t)reply.value[0];
        want_commit=1u;
        state=load_status==SC_OK || load_status==SC_EMPTY_STORE?ST_RUNNING:ST_RUNNING;
        set_text();CALL(publish_status(api));CALL(put(api,R_STATE));CALL(put(api,R_STORE));CALL(put(api,R_LIMIT));
        last_poll=api->time_ms;

        for(;;){
            CALL(fx_sdk_events(&client,api));
            if(reply.result==FX_OK && reply.bytes==sizeof(FxUiEvent)){
                FxUiEvent e;uint8_t *p=(uint8_t *)&e;
                for(uint32_t k=0u;k<sizeof(e);++k)p[k]=reply.data[k];
                if(e.event==FX_INPUT_ACTIVATE && e.control==R_LIMIT && slots_active==64u){
                    store.next_limit=(store.next_limit?store.next_limit:limit)==64u?32u:64u;
                    want_commit=1u;set_text();CALL(put(api,R_LIMIT));
                }
            }
            /* The core puts these back at power-off so settings never keep an extended selection. */
            if(store.next_proxy[0]!=sent_proxy[0] || store.next_proxy[1]!=sent_proxy[1] || !proxy_sent){
                CALL(slot_call(api,SLOTS_PROXY,store.next_proxy[1],0u,0u,store.next_proxy[0],0));
                if(reply.result==FX_OK){sent_proxy[0]=store.next_proxy[0];sent_proxy[1]=store.next_proxy[1];proxy_sent=1u;}
            }
            if(api->time_ms-last_poll<POLL_MS)continue;
            last_poll=api->time_ms;
            CALL(slot_call(api,SLOTS_LANG_GET,0u,0u,0u,0u,0));
            if(reply.result==FX_OK && reply.value[1]==LANG_MAGIC && (uint32_t)reply.value[0]!=ui_lang){
                ui_lang=(uint32_t)reply.value[0];
                set_text();
                for(row=PAGE;row<=CONTROLS;++row){CALL(put(api,row));}
            }
            if(state==ST_OTHER_CARD)continue;

            CALL(slot_call(api,SLOTS_SELECTION,0u,0u,0u,0u,0));
            for(i=0u;i<2u;++i){
                uint32_t now=i?(reply.value[2]?0u:(uint32_t)reply.value[1]):(uint32_t)reply.value[0];
                if(now==selected[i])continue;
                /* While a saved slot is hidden by the limit, the camera's own fallback is not a
                 * user choice. Native selections persist natively; only a slot choice is recorded. */
                if(held>>i&1u && !sc_selector_extended(now,0)){selected[i]=now;continue;}
                held&=~(1u<<i);
                if(sc_selector_legal(i,selected[i]))legal[i]=selected[i];
                selected[i]=now;
                store.next_proxy[i]=sc_selector_legal(i,now)?now:legal[i];
                store.next_selection[i]=now;
                want_commit=1u;
            }
            CALL(slot_call(api,SLOTS_SUMMARY,0u,0u,0u,0u,0));
            {
                uint32_t now=(uint32_t)reply.value[0];
                if(now!=pending_summary){pending_summary=now;continue;} /* still changing: wait until stable */
                if(now==summary && !want_commit)continue;
            }
            for(i=0u;i<SC_SLOTS;++i){
                if(!active(i))continue;
                CALL(slot_call(api,SLOTS_HASH,i,0u,0u,0u,0));
                if((uint32_t)reply.value[0]==base_crc[i] || reply.value[1]!=1u)continue;
                base_crc[i]=(uint32_t)reply.value[0];
                for(offset=0u;offset<SC_RECORD;offset+=seen){
                    seen=SC_RECORD-offset;if(seen>CHUNK)seen=CHUNK;
                    CALL(slot_call(api,SLOTS_READ,i,offset,seen,0u,0));
                    if(reply.result!=FX_OK || reply.bytes!=seen)return 8;
                    for(uint32_t k=0u;k<seen;++k)record[offset+k]=reply.data[k];
                }
                sc_put_record(&store,i,record);
            }
            summary=pending_summary;
            {
                uint32_t dirty=0u,moved_selection=0u;
                for(uint32_t k=0u;k<SC_SLOTS;++k)dirty|=store.dirty[k];
                for(uint32_t b=0u;b<2u;++b)moved_selection|=store.next_selection[b]!=store.selection[b] || store.next_proxy[b]!=store.proxy[b];
                moved_selection|=store.next_limit!=store.limit;
                if(!dirty && !moved_selection){want_commit=0u;continue;}
                /* Without a store, a plain native selection is not worth creating one. */
                if(!dirty && !store.head && store.next_limit==store.limit && !selector_extended(selected[0]) && !selector_extended(selected[1])){want_commit=0u;continue;}
            }
            /* Until a commit succeeds, keep retrying what is pending at each poll. */
            want_commit=1u;
            /* A card that no longer holds this store must not receive its generations.
             * An empty view meeting a card that has one stops too: writing would bury that store. */
            sc_begin_check(&store);answer=(ScReply){0};RUN_MACHINE();
            if(store.status==SC_OTHER_CARD){
                ++checks_other;state=ST_OTHER_CARD;
                set_text();CALL(publish_status(api));CALL(put(api,R_STATE));continue;
            }
            if(store.status==SC_NO_MEDIA)continue;
            /* The counter register traps at EL0; uptime at the first commit tells two stores apart. */
            store.entropy=(api->time_ms+1u)*UINT64_C(0x9e3779b97f4a7c15);
            sc_begin_commit(&store);answer=(ScReply){0};RUN_MACHINE();
            commit_status=store.status;
            if(store.status==SC_OK){++commits;want_commit=0u;}
            used_count=0u;for(i=0u;i<SC_SLOTS;++i)used_count+=store.used[i];
            set_text();CALL(publish_status(api));CALL(put(api,R_STATE));CALL(put(api,R_STORE));
        }
    default:return 9;
    }
}
