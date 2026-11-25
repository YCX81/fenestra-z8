#include "../native_sync.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define MISSING 0x80000010u
#define END 0x80000030u
static char manifest[]="native=A:\\DCIM\\100NCZ_8\\DSC_0001.JPG\nsize=1169905\nuuid=19FEF38C-CA15-4047-D02B-A6A72CC4A198\nvolume_generation=1\n";
static const char*files[]={"A:\\LIVE\\100NCZ_8\\DSC_0001.JPG","A:\\LIVE\\100NCZ_8\\DSC_0001.MOV","A:\\LIVEDATA\\100NCZ_8\\DSC_0001.LVP","A:\\LIVEDATA\\100NCZ_8\\DSC_0001.MAN"};
static const char*names[]={"DSC_0001.JPG","DSC_0001.MOV","DSC_0001.LVP","DSC_0001.MAN"};
static struct{
    char vol;
    unsigned dual_other_no_ld;
    unsigned exists[4],attr[4],jpg,nef,protect,offset,locked,require_lock,lock_calls,mutations;
    unsigned root_error,close_error,extra,removed_dir,unknown_file,nested_error,missing_native_folder;
    unsigned native_next_missing,too_many,too_deep,no_livedata,dcim_filler,dcim_dir_opens,fenestra_opens;
    struct{unsigned kind,index,open;} dirs[32];
} s;
static char mapped[256];
static const char*as_a(const char*p){assert(p[0]==s.vol);strncpy(mapped,p,255);mapped[0]='A';return mapped;}
static int is_native_jpg(const char*p){
    if(p[0]!=s.vol)return 0;
    return !strcmp(p+1,":\\DCIM\\100NCZ_8\\DSC_0001.JPG");
}
static int file_index(const char*p){p=as_a(p);for(int i=0;i<4;i++)if(!strcmp(p,files[i]))return i;fprintf(stderr,"unexpected file: %s\n",p);assert(0);return -1;}
static void check_lock_mode(void){if(s.require_lock)assert(s.locked);else assert(!s.locked);}
static int32_t open_(const char*p,uint32_t f,uint32_t a){(void)f;(void)a;check_lock_mode();assert(file_index(p)==3);s.offset=0;return s.exists[3]?99:(int32_t)MISSING;}
static uint32_t read_(int32_t fd,void*out,uint32_t n,uint32_t*done){check_lock_mode();assert(fd==99);unsigned total=sizeof manifest-1+s.extra;unsigned left=total-s.offset;if(n>left)n=left;for(unsigned i=0;i<n;i++)((char*)out)[i]=s.offset+i<sizeof manifest-1?manifest[s.offset+i]:'!';s.offset+=n;*done=n;return 0;}
static uint32_t close_(int32_t fd){check_lock_mode();assert(fd==99);return s.close_error?0x80000020:0;}
static uint32_t getattr_(const char*p,uint32_t*a){
    check_lock_mode();
    if(is_native_jpg(p)){
        if(s.missing_native_folder||!s.jpg)return MISSING;
        *a=s.protect?1u:0u;return 0;
    }
    int i=file_index(p);if(!s.exists[i])return MISSING;*a=s.attr[i];return 0;
}
static uint32_t setattr_(const char*p,uint32_t a){assert(s.locked);int i=file_index(p);assert(s.exists[i]);s.attr[i]=a;s.mutations++;return 0;}
static uint32_t remove_(const char*p){assert(s.locked&&!s.jpg&&!s.nef);int i=file_index(p);assert(!s.attr[i]&&s.exists[i]);s.exists[i]=0;s.mutations++;return 0;}
static uint32_t rmdir_(const char*p){(void)p;assert(0&&"flat layout never removes directories");return 0;}
static uint32_t dir_open_(const char*p,int32_t*fd){
    check_lock_mode();
    unsigned kind=0;
    if(strstr(p,"FENESTRA")||strstr(p,"fenestra")){s.fenestra_opens++;assert(0&&"must never open FENESTRA");}
    if(p[0]!=s.vol){
        if(s.dual_other_no_ld){
            if(!strcmp(p+1,":\\"))kind=9;
            else if(!strcmp(p+1,":\\LIVEDATA"))return MISSING;
            else {fprintf(stderr,"unexpected other-vol dir: %s\n",p);assert(0);}
        }else{
            assert(!strcmp(p+1,":\\"));return MISSING;
        }
    }else{
        p=as_a(p);
        if(!strcmp(p,"A:\\"))kind=1;
        else if(!strcmp(p,"A:\\DCIM\\100NCZ_8")){s.dcim_dir_opens++;if(s.missing_native_folder)return MISSING;kind=2;}
        else if(!strcmp(p,"A:\\LIVEDATA")){if(s.no_livedata)return MISSING;kind=6;}
        else if(!strcmp(p,"A:\\LIVEDATA\\100NCZ_8"))kind=8;
        else if(!strncmp(p,"A:\\LIVEDATA\\DEEP",16))kind=7;
        else {fprintf(stderr,"unexpected directory (must only scan LIVEDATA): %s\n",p);assert(0);}
    }
    for(unsigned i=0;i<32;i++)if(!s.dirs[i].open){s.dirs[i].open=1;s.dirs[i].kind=kind;s.dirs[i].index=0;*fd=(int32_t)i;return 0;}
    assert(0);return MISSING;
}
static uint32_t dir_next_(int32_t fd,void*out){
    check_lock_mode();
    assert(fd>=0&&fd<32&&s.dirs[fd].open);
    uint8_t*d=out;const char*name=0;unsigned attrs=0;
    unsigned kind=s.dirs[fd].kind,index=s.dirs[fd].index++;
    if(kind==1||kind==9){
        if(kind==1&&s.root_error)return 0x80000020;
        if(index==0)name="DCIM";else if(index==1)name="FENESTRA";else if(index==2)name="LIVE";
        else if(index==3&&!(kind==9||s.no_livedata))name="LIVEDATA";
        attrs=0x10;
    }
    if(kind==2){
        if(s.native_next_missing)return MISSING;
        if(index<s.dcim_filler){name="DSC_9999.JPG";}
        else{
            unsigned rel=index-s.dcim_filler;
            if(rel==0&&s.jpg)name="DSC_0001.JPG";
            else if(rel<=1&&s.nef)name="DSC_0001.NEF";
            else if(rel<=2)name="DSC_0002.JPG";
        }
        attrs=s.protect?1:0;
    }
    if(kind==8){
        unsigned n=0;
        if(s.nested_error)return 0x80000020;
        for(unsigned i=2;i<4;i++)if(s.exists[i]){if(index==n)name=names[i];n++;}
        if(index==n&&s.unknown_file)name="OTHER.TXT";
    }
    if(kind==6){
        if(index==0){name="100NCZ_8";attrs=0x10;}
        else if(index==1&&s.too_deep){name="DEEP";attrs=0x10;}
        else if(s.too_many&&index<4100){name="MISC.TMP";attrs=0;}
    }
    if(kind==7&&!index){name="X";attrs=0x10;}
    if(!name)return END;
    for(unsigned i=0;name[i];i++){d[14+2*i]=(uint8_t)name[i];}d[0x20e]=(uint8_t)attrs;return 0;
}
static uint32_t dir_close_(int32_t fd){check_lock_mode();assert(fd>=0&&fd<32&&s.dirs[fd].open);s.dirs[fd].open=0;return 0;}
static void lock_(void){assert(!s.locked);s.locked=1;s.lock_calls++;}
static void unlock_(void){assert(s.locked);s.locked=0;}
static void set_vol(char v){manifest[7]=v;}
static void reset(LiveNativeSync*c){
    memset(&s,0,sizeof s);s.vol=manifest[7];s.require_lock=1;
    for(unsigned i=0;i<4;i++)s.exists[i]=1;
    live_native_sync_init(c,'A',1);
    c->fs=(LiveNativeSyncBackend){open_,read_,close_,getattr_,setattr_,remove_,rmdir_,dir_open_,dir_next_,dir_close_,lock_,unlock_};
}
static int run_boot(LiveSyncJournal*j,LiveNativeSync*c){
    s.require_lock=0;
    int r=live_native_sync_gc_boot(j,c);
    assert(!s.locked&&!s.lock_calls&&!s.fenestra_opens);
    s.require_lock=1;
    return r;
}
int main(void){
    LiveManifest m;assert(!live_manifest_parse(manifest,sizeof manifest-1,&m));LiveNativeSync c;LiveSyncJournal j={0};
    /* Step operations still strictly hold DCF lock (0xa04821284) across scan+mutation. */
    reset(&c);s.nef=1;assert(!live_sync_enqueue(&j,LIVE_SYNC_DELETE,&m));assert(live_native_sync_step(&j,&c)==1&&!s.mutations&&!s.locked&&s.lock_calls==1);
    s.nef=0;assert(!live_sync_enqueue(&j,LIVE_SYNC_DELETE,&m));assert(live_native_sync_step(&j,&c)==1&&!s.removed_dir&&s.mutations==4&&!s.locked);for(unsigned i=0;i<4;i++)assert(!s.exists[i]);
    reset(&c);s.jpg=1;s.protect=1;assert(!live_sync_enqueue(&j,LIVE_SYNC_PROTECT,&m));assert(live_native_sync_step(&j,&c)==1);for(unsigned i=0;i<4;i++)assert(s.attr[i]==1&&s.exists[i]);
    s.protect=0;assert(!live_sync_enqueue(&j,LIVE_SYNC_PROTECT,&m));assert(live_native_sync_step(&j,&c)==1);for(unsigned i=0;i<4;i++)assert(!s.attr[i]);
    reset(&c);s.root_error=1;assert(!live_sync_enqueue(&j,LIVE_SYNC_DELETE,&m));assert(live_native_sync_step(&j,&c)==2&&!s.mutations&&j.count&&!s.locked);s.root_error=0;assert(live_native_sync_step(&j,&c)==1&&!s.exists[3]);
    reset(&c);s.extra=1;assert(!live_sync_enqueue(&j,LIVE_SYNC_DELETE,&m));assert(live_native_sync_step(&j,&c)==2&&!s.mutations);j=(LiveSyncJournal){0};
    reset(&c);s.close_error=1;assert(!live_sync_enqueue(&j,LIVE_SYNC_DELETE,&m));assert(live_native_sync_step(&j,&c)==2&&!s.mutations&&c.poisoned&&!s.locked);j=(LiveSyncJournal){0};
    reset(&c);s.unknown_file=1;assert(!live_sync_enqueue(&j,LIVE_SYNC_DELETE,&m));assert(live_native_sync_step(&j,&c)==1&&s.mutations==4);j=(LiveSyncJournal){0};
    reset(&c);assert(!live_sync_enqueue(&j,LIVE_SYNC_GC,&m));assert(live_native_sync_step(&j,&c)==2&&!s.lock_calls&&!s.mutations);j=(LiveSyncJournal){0};

    /* Boot GC census: ONLY scans X:\LIVEDATA\, NEVER acquires DCF lock (lock_calls==0), NEVER touches FENESTRA/DCIM/LIVE! */
    reset(&c);s.nested_error=1;assert(run_boot(&j,&c)==-1&&!j.count&&!c.gc_ready&&!s.mutations);
    reset(&c);s.too_many=1;assert(run_boot(&j,&c)==-1&&!j.count&&!c.gc_ready&&!s.mutations);
    reset(&c);s.too_deep=1;assert(run_boot(&j,&c)==-1&&!j.count&&!c.gc_ready&&!s.mutations);
    reset(&c);s.extra=1;assert(run_boot(&j,&c)==-1&&!j.count&&!c.gc_ready&&!s.mutations);
    reset(&c);s.no_livedata=1;assert(run_boot(&j,&c)==0&&!j.count&&c.gc_ready&&!s.mutations&&!s.dcim_dir_opens);

    /* Fast-path stat: when native JPG still exists, boot GC does 0 DCIM directory scans! */
    reset(&c);s.jpg=s.nef=1;s.dcim_filler=10317;
    assert(run_boot(&j,&c)==0&&!j.count&&c.gc_ready&&!s.mutations&&s.dcim_dir_opens==0);

    /* Real 10,317-file CFB card: JPG deleted but NEF sibling survives at entry #10,317 (> old 4096 cap!) -> NOT deleted! */
    reset(&c);s.jpg=0;s.nef=1;s.dcim_filler=10317;
    assert(run_boot(&j,&c)==0&&!j.count&&c.gc_ready&&!s.mutations&&s.dcim_dir_opens==1);

    /* Real 10,317-file CFB card: both JPG & NEF deleted (genuine orphan among 10,317 other photos!) -> queued & cleaned under lock! */
    reset(&c);s.jpg=0;s.nef=0;s.dcim_filler=10317;
    assert(run_boot(&j,&c)==1&&j.count==1&&c.gc_ready&&!s.mutations&&s.dcim_dir_opens==1);
    assert(j.entries[j.head].operation==LIVE_SYNC_GC);
    assert(live_native_sync_step(&j,&c)==1&&s.lock_calls==1&&!s.exists[0]&&!s.exists[3]);

    /* Missing native folder & mid-scan errors */
    reset(&c);s.missing_native_folder=1;assert(run_boot(&j,&c)==1&&j.count==1&&c.gc_ready&&!s.mutations);
    assert(live_native_sync_step(&j,&c)==1&&s.lock_calls==1&&!s.exists[0]&&!s.exists[3]);
    reset(&c);assert(run_boot(&j,&c)==1);s.native_next_missing=1;assert(live_native_sync_step(&j,&c)==2&&!s.mutations&&j.count==1);j=(LiveSyncJournal){0};
    reset(&c);assert(run_boot(&j,&c)==1);c.generation=2;assert(live_native_sync_step(&j,&c)==2&&!s.mutations);j=(LiveSyncJournal){0};

    /* Card-slot B & Dual-card (A has 10,317 DCIM + no LIVEDATA, B has FENESTRA + LIVEDATA orphan) */
    set_vol('B');assert(!live_manifest_parse(manifest,sizeof manifest-1,&m));j=(LiveSyncJournal){0};
    reset(&c);s.nef=0;assert(!live_sync_enqueue(&j,LIVE_SYNC_DELETE,&m));assert(live_native_sync_step(&j,&c)==1&&s.mutations==4);for(unsigned i=0;i<4;i++)assert(!s.exists[i]);
    reset(&c);s.dual_other_no_ld=1;assert(run_boot(&j,&c)==1&&j.count==1&&c.gc_ready);
    assert(live_native_sync_step(&j,&c)==1&&s.lock_calls==1&&!s.exists[0]&&!s.exists[3]);
    reset(&c);s.jpg=1;s.dual_other_no_ld=1;assert(run_boot(&j,&c)==0&&!j.count&&c.gc_ready&&!s.mutations&&s.dcim_dir_opens==0);
    reset(&c);c.generation=7;assert(!live_sync_enqueue(&j,LIVE_SYNC_DELETE,&m));assert(live_native_sync_step(&j,&c)==1&&s.mutations==4);
    reset(&c);c.generation=0;assert(!live_sync_enqueue(&j,LIVE_SYNC_DELETE,&m));assert(live_native_sync_step(&j,&c)==2&&!s.mutations);j=(LiveSyncJournal){0};
    set_vol('A');
    puts("preview_sync_native: PASS (LIVEDATA-only lock-free boot census, O(1) getattr fast-path, 10317-file DCIM scan, dual-card, DCF lock on step only)");
    return 0;
}
