#include "../native_sync.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define MISSING 0x80000010u
#define END 0x80000030u
static const char manifest[]="native=A:\\DCIM\\100NCZ_8\\DSC_0001.JPG\nsize=1169905\nuuid=19FEF38C-CA15-4047-D02B-A6A72CC4A198\nvolume_generation=1\n";
static const char*files[]={"PAIR.JPG","CLIP.MOV","PREV.LVP","MANIFEST"};
static struct{unsigned exists[4],attr[4],jpg,nef,protect,offset,locked,lock_calls,mutations,root_error,close_error,extra,removed_dir,unknown_file,nested_error,missing_native_folder,native_next_missing,too_many,too_deep;struct{unsigned kind,index,open;} dirs[32];} s;
static int file_index(const char*p){const char*root="A:\\LIVE\\100NCZ_8\\DSC_0001\\";assert(!strncmp(p,root,strlen(root)));for(int i=0;i<4;i++)if(!strcmp(p+strlen(root),files[i]))return i;assert(0);return -1;}
static int32_t open_(const char*p,uint32_t f,uint32_t a){(void)f;(void)a;assert(s.locked&&file_index(p)==3);s.offset=0;return s.exists[3]?99:(int32_t)MISSING;}
static uint32_t read_(int32_t fd,void*out,uint32_t n,uint32_t*done){assert(s.locked&&fd==99);unsigned total=sizeof manifest-1+s.extra;unsigned left=total-s.offset;if(n>left)n=left;for(unsigned i=0;i<n;i++)((char*)out)[i]=s.offset+i<sizeof manifest-1?manifest[s.offset+i]:'!';s.offset+=n;*done=n;return 0;}
static uint32_t close_(int32_t fd){assert(s.locked&&fd==99);return s.close_error?0x80000020:0;}
static uint32_t getattr_(const char*p,uint32_t*a){assert(s.locked);if(!strcmp(p,"A:\\LIVE\\100NCZ_8\\DSC_0001")){*a=0x10;return s.removed_dir?MISSING:0;}int i=file_index(p);if(!s.exists[i])return MISSING;*a=s.attr[i];return 0;}
static uint32_t setattr_(const char*p,uint32_t a){assert(s.locked);int i=file_index(p);assert(s.exists[i]);s.attr[i]=a;s.mutations++;return 0;}
static uint32_t remove_(const char*p){assert(s.locked&&!s.jpg&&!s.nef);int i=file_index(p);assert(!s.attr[i]&&s.exists[i]);s.exists[i]=0;s.mutations++;return 0;}
static uint32_t rmdir_(const char*p){assert(s.locked&&!strcmp(p,"A:\\LIVE\\100NCZ_8\\DSC_0001"));for(unsigned i=0;i<4;i++)assert(!s.exists[i]);s.removed_dir=1;s.mutations++;return 0;}
static uint32_t dir_open_(const char*p,int32_t*fd){assert(s.locked);unsigned kind=0;
    if(!strcmp(p,"A:\\"))kind=1;
    else if(!strcmp(p,"A:\\DCIM\\100NCZ_8")){if(s.missing_native_folder)return MISSING;kind=2;}
    else if(!strcmp(p,"A:\\LIVE\\100NCZ_8\\DSC_0001"))kind=3;
    else if(!strcmp(p,"A:\\DCIM"))kind=4;
    else if(!strcmp(p,"A:\\LIVE"))kind=5;
    else if(!strcmp(p,"A:\\LIVE\\100NCZ_8"))kind=6;
    else if(!strncmp(p,"A:\\DEEP",7))kind=7;
    else {fprintf(stderr,"unexpected directory: %s\n",p);assert(0);}
    for(unsigned i=0;i<32;i++)if(!s.dirs[i].open){s.dirs[i].open=1;s.dirs[i].kind=kind;s.dirs[i].index=0;*fd=(int32_t)i;return 0;}
    assert(0);return MISSING;
}
static uint32_t dir_next_(int32_t fd,void*out){assert(s.locked&&fd>=0&&fd<32&&s.dirs[fd].open);uint8_t*d=out;const char*name=0;unsigned attrs=0;
    unsigned kind=s.dirs[fd].kind,index=s.dirs[fd].index++;
    if(kind==1){if(s.root_error)return 0x80000020;if(index==0)name="DCIM";else if(index==1)name="LIVE";else if(index==2&&s.too_deep)name="DEEP";else if(s.too_many&&index<4100)name="MISC";attrs=index<2||(index==2&&s.too_deep)?0x10:0;}
    if(kind==2){if(s.native_next_missing)return MISSING;if(index==0&&s.jpg)name="DSC_0001.JPG";else if(index<=1&&s.nef)name="DSC_0001.NEF";else if(index<=2)name="DSC_0002.JPG";attrs=s.protect?1:0;}
    if(kind==3){unsigned n=0;for(unsigned i=0;i<4;i++)if(s.exists[i]){if(index==n)name=files[i];n++;}if(index==n&&s.unknown_file)name="OTHER.TXT";}
    if(kind==4&&!s.missing_native_folder&&!index){name="100NCZ_8";attrs=0x10;}
    if(kind==5&&!index){name="100NCZ_8";attrs=0x10;}
    if(kind==6){if(s.nested_error)return 0x80000020;if(!index){name="DSC_0001";attrs=0x10;}}
    if(kind==7&&!index){name="X";attrs=0x10;}
    if(!name)return END;
    for(unsigned i=0;name[i];i++){d[14+2*i]=(uint8_t)name[i];}d[0x20e]=(uint8_t)attrs;return 0;
}
static uint32_t dir_close_(int32_t fd){assert(s.locked&&fd>=0&&fd<32&&s.dirs[fd].open);s.dirs[fd].open=0;return 0;}
static void lock_(void){assert(!s.locked);s.locked=1;s.lock_calls++;}
static void unlock_(void){assert(s.locked);s.locked=0;}
static void reset(LiveNativeSync*c){memset(&s,0,sizeof s);for(unsigned i=0;i<4;i++)s.exists[i]=1;live_native_sync_init(c,'A',1);c->fs=(LiveNativeSyncBackend){open_,read_,close_,getattr_,setattr_,remove_,rmdir_,dir_open_,dir_next_,dir_close_,lock_,unlock_};}
int main(void){LiveManifest m;assert(!live_manifest_parse(manifest,sizeof manifest-1,&m));LiveNativeSync c;LiveSyncJournal j={0};
    reset(&c);s.nef=1;assert(!live_sync_enqueue(&j,LIVE_SYNC_DELETE,&m));assert(live_native_sync_step(&j,&c)==1&&!s.mutations&&!s.locked&&s.lock_calls==1);
    s.nef=0;assert(!live_sync_enqueue(&j,LIVE_SYNC_DELETE,&m));assert(live_native_sync_step(&j,&c)==1&&s.removed_dir&&s.mutations==5&&!s.locked);
    reset(&c);s.jpg=1;s.protect=1;assert(!live_sync_enqueue(&j,LIVE_SYNC_PROTECT,&m));assert(live_native_sync_step(&j,&c)==1);for(unsigned i=0;i<4;i++)assert(s.attr[i]==1&&s.exists[i]);
    s.protect=0;assert(!live_sync_enqueue(&j,LIVE_SYNC_PROTECT,&m));assert(live_native_sync_step(&j,&c)==1);for(unsigned i=0;i<4;i++)assert(!s.attr[i]);
    reset(&c);s.root_error=1;assert(!live_sync_enqueue(&j,LIVE_SYNC_DELETE,&m));assert(live_native_sync_step(&j,&c)==2&&!s.mutations&&j.count&&!s.locked);s.root_error=0;assert(live_native_sync_step(&j,&c)==1&&s.removed_dir);
    reset(&c);s.extra=1;assert(!live_sync_enqueue(&j,LIVE_SYNC_DELETE,&m));assert(live_native_sync_step(&j,&c)==2&&!s.mutations);j=(LiveSyncJournal){0};
    reset(&c);s.close_error=1;assert(!live_sync_enqueue(&j,LIVE_SYNC_DELETE,&m));assert(live_native_sync_step(&j,&c)==2&&!s.mutations&&c.poisoned&&!s.locked);j=(LiveSyncJournal){0};
    reset(&c);s.unknown_file=1;assert(!live_sync_enqueue(&j,LIVE_SYNC_DELETE,&m));assert(live_native_sync_step(&j,&c)==2&&!s.removed_dir);assert(s.mutations==4);j=(LiveSyncJournal){0};
    reset(&c);assert(!live_sync_enqueue(&j,LIVE_SYNC_GC,&m));assert(live_native_sync_step(&j,&c)==2&&!s.lock_calls&&!s.mutations);j=(LiveSyncJournal){0};
    reset(&c);s.nested_error=1;assert(live_native_sync_gc_boot(&j,&c)==-1&&!j.count&&!c.gc_ready&&!s.mutations&&!s.locked);
    reset(&c);s.too_many=1;assert(live_native_sync_gc_boot(&j,&c)==-1&&!j.count&&!c.gc_ready&&!s.mutations&&!s.locked);
    reset(&c);s.too_deep=1;assert(live_native_sync_gc_boot(&j,&c)==-1&&!j.count&&!c.gc_ready&&!s.mutations&&!s.locked);
    reset(&c);s.extra=1;assert(live_native_sync_gc_boot(&j,&c)==-1&&!j.count&&!c.gc_ready&&!s.mutations&&!s.locked);
    reset(&c);s.jpg=s.nef=1;assert(live_native_sync_gc_boot(&j,&c)==0&&!j.count&&c.gc_ready&&!s.mutations&&!s.locked);
    reset(&c);assert(live_native_sync_gc_boot(&j,&c)==1&&j.count==1&&c.gc_ready&&!s.mutations&&!s.locked);assert(j.entries[j.head].operation==LIVE_SYNC_GC);assert(live_native_sync_step(&j,&c)==1&&s.removed_dir);
    reset(&c);s.missing_native_folder=1;assert(live_native_sync_gc_boot(&j,&c)==1&&j.count==1&&c.gc_ready&&!s.mutations);assert(live_native_sync_step(&j,&c)==1&&s.removed_dir);
    reset(&c);assert(live_native_sync_gc_boot(&j,&c)==1);s.native_next_missing=1;assert(live_native_sync_step(&j,&c)==2&&!s.mutations&&j.count==1);j=(LiveSyncJournal){0};
    reset(&c);assert(live_native_sync_gc_boot(&j,&c)==1);c.generation=2;assert(live_native_sync_step(&j,&c)==2&&!s.mutations);j=(LiveSyncJournal){0};
    puts("preview_sync_native: PASS (C3.11 adapter, DCF ownership, complete scans, readonly, error/close/extra-file guards, atomic bounded boot GC census)");return 0;}
