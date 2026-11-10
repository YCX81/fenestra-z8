#include "native_sync.h"
#define FN(addr,type) ((type)(uintptr_t)(addr))
#define MISSING UINT32_C(0x80000010)
#define END UINT32_C(0x80000030)
#define BAD UINT32_C(0xffffffff)
static size_t len(const char*s,size_t n){size_t i=0;while(i<n&&s[i])i++;return i;}
static void cp(void*d,const void*s,size_t n){uint8_t*a=d;const uint8_t*b=s;while(n--)*a++=*b++;}
static int eq(const char*a,const char*b){for(unsigned i=0;i<LIVE_SYNC_PATH;i++){if(a[i]!=b[i])return 0;if(!a[i])return 1;}return 0;}
static unsigned fold(unsigned a){return a>='a'&&a<='z'?a-32:a;}
static int prefix(const char*a,const char*b,unsigned n){for(unsigned i=0;i<n;i++)if(fold((unsigned char)a[i])!=fold((unsigned char)b[i]))return 0;return 1;}
static int failure(LiveNativeSync*c,uint32_t r){if(r)c->last_error=r;return r?-1:0;}
static int entry_name(const uint8_t *data,char out[256]){
    for(unsigned i=0;i<256;i++){
        unsigned ch=data[14+2*i]|(unsigned)data[15+2*i]<<8;
        if(!ch){out[i]=0;if(!i){for(unsigned j=0;j<13;j++){if(data[j]>127)return -1;out[j]=(char)data[j];if(!data[j])return 0;}return -1;}return 0;}
        if(ch>127)return -1;
        out[i]=(char)ch;
    }
    return -1;
}
/* Complete directory enumeration, with a strict EOF and checked close.
 * Never convert an error or a bounded scan limit to native absence. */
static int scan(LiveNativeSync*c,const char *path,unsigned mode,unsigned *members,unsigned *protect){
    int32_t fd=-1;uint32_t r=c->fs.dir_open(path,&fd);
    if(r||fd<0){failure(c,r?r:BAD);return r==MISSING?-2:-1;}
    int result=-1;
    for(unsigned i=0;i<4096;i++){
        uint8_t data[0x220]={0};char name[256];r=c->fs.dir_next(fd,data);
        if(r==END){result=0;break;}
        if(r){failure(c,r);break;}
        if(entry_name(data,name)){failure(c,BAD);break;}
        if(mode==1&&len(name,256)>=9&&prefix(name,c->native_path+17,8)&&name[8]=='.'){
            (*members)++;if(data[0x20e]&1)(*protect)++;
        }
        if(mode==2&&name[0]&&!(name[0]=='.'&&(!name[1]||(name[1]=='.'&&!name[2])))){failure(c,BAD);break;}
    }
    r=c->fs.dir_close(fd);if(r){c->poisoned=1;return failure(c,r);}return result;
}
static int ready(void*v,char drive,uint64_t generation){
    LiveNativeSync*c=v;if(c->poisoned||drive!=c->drive||generation!=c->generation||!generation)return -1;
    char root[4]={drive,':','\\',0};unsigned n=0,p=0;return scan(c,root,0,&n,&p);
}
static int confined(LiveNativeSync*c,const char *path,int directory){
    if(directory)return eq(path,c->directory)?0:-1;
    size_t n=len(c->directory,LIVE_SYNC_PATH);if(!prefix(path,c->directory,(unsigned)n)||path[n]!='\\')return -1;
    static const char*const files[]={"PAIR.JPG","CLIP.MOV","PREV.LVP","MANIFEST"};
    for(unsigned i=0;i<4;i++)if(eq(path+n+1,files[i]))return 0;
    return -1;
}
static int read_manifest(void*v,const char*path,void*out,size_t capacity,size_t *got){
    LiveNativeSync*c=v;*got=0;if(confined(c,path,0)||capacity>UINT32_MAX)return -1;
    int32_t fd=c->fs.open(path,0,0);if(fd<0||(uint32_t)fd==0x7fff0000)return failure(c,(uint32_t)fd);
    uint32_t n=0;int result=failure(c,c->fs.read(fd,out,(uint32_t)capacity,&n));
    if(n>capacity)result=-1;
    if(!result){uint8_t extra;uint32_t extra_n=0;if(failure(c,c->fs.read(fd,&extra,1,&extra_n))||extra_n)result=-1;}
    uint32_t closed=c->fs.close(fd);if(closed){c->poisoned=1;result=failure(c,closed);}if(!result)*got=n;return result;
}
static int native_group(void*v,const char*path,unsigned*members,unsigned*protect){
    LiveNativeSync*c=v;if(!eq(path,c->native_path))return -1;char folder[17];cp(folder,path,16);folder[16]=0;
    *members=*protect=0;c->last_error=0;int r=scan(c,folder,1,members,protect);
    /* Missing parent is absence only after the complete, same-epoch boot
     * census. Ordinary delete/protect never treats open failure as absence. */
    if(r==-2&&c->gc_active&&c->gc_ready&&c->gc_generation==c->generation)return 0;
    return r;
}
static int readonly(void*v,const char*path,int readonly){
    LiveNativeSync*c=v;uint32_t attrs=0;if(confined(c,path,0))return -1;
    uint32_t r=c->fs.getattr(path,&attrs);if(r==MISSING)return 0;if(r)return failure(c,r);
    if(attrs&0x18)return failure(c,BAD);
    uint32_t desired=readonly?attrs|1:attrs&~1u;if(attrs==desired)return 0;
    if(failure(c,c->fs.setattr(path,desired)))return -1;
    return failure(c,c->fs.getattr(path,&attrs))||attrs!=desired?-1:0;
}
static int remove_file(void*v,const char*path){
    LiveNativeSync*c=v;uint32_t attrs=0;if(confined(c,path,0))return -1;
    uint32_t r=c->fs.getattr(path,&attrs);if(r==MISSING)return 0;if(r)return failure(c,r);
    if(attrs&0x18)return failure(c,BAD);
    if(failure(c,c->fs.remove(path)))return -1;
    r=c->fs.getattr(path,&attrs);return r==MISSING?0:failure(c,r?r:BAD);
}
static int remove_directory(void*v,const char*path){
    LiveNativeSync*c=v;if(confined(c,path,1))return -1;uint32_t attrs=0,r=c->fs.getattr(path,&attrs);
    if(r==MISSING)return 0;
    if(r||!(attrs&0x10))return failure(c,r?r:BAD);
    unsigned n=0,p=0;if(scan(c,path,2,&n,&p))return -1;
    if(failure(c,c->fs.rmdir(path)))return -1;
    r=c->fs.getattr(path,&attrs);return r==MISSING?0:failure(c,r?r:BAD);
}
void live_native_sync_init(LiveNativeSync*c,char drive,uint64_t generation){
    *c=(LiveNativeSync){0};c->drive=drive;c->generation=generation;
    c->fs=(LiveNativeSyncBackend){
        FN(0x402dd4f8,int32_t(*)(const char*,uint32_t,uint32_t)),
        FN(0x402dd6c8,uint32_t(*)(int32_t,void*,uint32_t,uint32_t*)),
        FN(0x402dd558,uint32_t(*)(int32_t)),
        FN(0x402ddb34,uint32_t(*)(const char*,uint32_t*)),
        FN(0x402ddb8c,uint32_t(*)(const char*,uint32_t)),
        FN(0x402dda4c,uint32_t(*)(const char*)),
        FN(0x402ddc24,uint32_t(*)(const char*)),
        FN(0x402ddc6c,uint32_t(*)(const char*,int32_t*)),
        FN(0x402ddd1c,uint32_t(*)(int32_t,void*)),
        FN(0x402ddd6c,uint32_t(*)(int32_t)),
        FN(0x4085d348,void(*)(void)),FN(0x4085d364,void(*)(void))};
}
int live_native_sync_step(LiveSyncJournal*j,LiveNativeSync*c){
    if(!j||!c||c->poisoned)return -1;
    if(!j->count)return 0;
    LiveSyncEntry*e=&j->entries[j->head];
    if(e->operation==LIVE_SYNC_GC&&(!c->gc_ready||c->gc_generation!=c->generation))return 2;
    if(live_sync_directory(c->directory,e->identity.native_path))return -1;
    cp(c->native_path,e->identity.native_path,LIVE_SYNC_PATH);
    LiveSyncOps o={c,ready,read_manifest,native_group,remove_file,remove_directory,readonly,0};
    c->fs.lock();c->gc_active=e->operation==LIVE_SYNC_GC;
    int result=live_sync_step(j,&o);c->gc_active=0;c->fs.unlock();c->steps++;return result;
}
typedef struct {LiveManifest candidates[LIVE_SYNC_CAPACITY];unsigned count,entries;} Census;
static int collect_manifest(LiveNativeSync*c,Census*s,const char*path){
    /* Only the proven v3 LIVE/folder/stem/MANIFEST layout is a candidate. */
    if(len(path,LIVE_SYNC_PATH)!=34||!prefix(path+1,":\\LIVE\\",7)||path[16]!='\\'||path[25]!='\\'||!eq(path+26,"MANIFEST"))return 0;
    if(s->count>=LIVE_SYNC_CAPACITY)return failure(c,BAD);
    cp(c->directory,path,25);c->directory[25]=0;
    uint8_t data[LIVE_SYNC_MANIFEST_MAX];size_t bytes=0;LiveManifest m;
    if(read_manifest(c,path,data,sizeof data,&bytes)||live_manifest_parse(data,bytes,&m))return failure(c,BAD);
    char expected[LIVE_SYNC_PATH];
    if(m.native_path[0]!=c->drive||m.volume_generation!=c->generation||live_sync_directory(expected,m.native_path)||!eq(expected,c->directory))return failure(c,BAD);
    s->candidates[s->count++]=m;return 0;
}
static int census_tree(LiveNativeSync*c,Census*s,const char*path,unsigned depth){
    int32_t fd=-1;uint32_t r=c->fs.dir_open(path,&fd);if(r||fd<0)return failure(c,r?r:BAD);
    int result=-1;
    for(;;){
        uint8_t data[0x220]={0};char name[256],child[LIVE_SYNC_PATH];
        r=c->fs.dir_next(fd,data);if(r==END){result=0;break;}if(r){failure(c,r);break;}
        if(++s->entries>4096||entry_name(data,name)){failure(c,BAD);break;}
        if(!name[0]||(name[0]=='.'&&(!name[1]||(name[1]=='.'&&!name[2]))))continue;
        /* Reject malformed names which could escape the enumerated parent. */
        size_t n=len(name,256),base=len(path,LIVE_SYNC_PATH);unsigned bad=0;
        for(size_t i=0;i<n;i++)if(name[i]=='\\'||name[i]=='/'||name[i]==':')bad=1;
        size_t sep=base&&path[base-1]!='\\';
        if(bad||n>=256||base+sep+n>=LIVE_SYNC_PATH){failure(c,BAD);break;}
        cp(child,path,base);if(sep)child[base++]='\\';cp(child+base,name,n+1);
        if(data[0x20e]&8)continue; /* volume label */
        if(data[0x20e]&0x10){
            if(depth>=8||census_tree(c,s,child,depth+1)){failure(c,BAD);break;}
        }else if(collect_manifest(c,s,child))break;
    }
    r=c->fs.dir_close(fd);if(r){c->poisoned=1;return failure(c,r);}return result;
}
int live_native_sync_gc_boot(LiveSyncJournal*j,LiveNativeSync*c){
    if(!j||!c||j->count||c->poisoned||!c->generation||(c->drive!='A'&&c->drive!='B'))return -1;
    c->gc_ready=0;c->gc_generation=0;
    Census census={0};unsigned absent[LIVE_SYNC_CAPACITY]={0};
    char root[4]={c->drive,':','\\',0};int result=-1;
    c->fs.lock();
    if(census_tree(c,&census,root,0))goto done;
    for(unsigned i=0;i<census.count;i++){
        LiveManifest*m=&census.candidates[i];cp(c->native_path,m->native_path,LIVE_SYNC_PATH);
        unsigned members=0,protect=0;c->last_error=0;
        int group_result=native_group(c,m->native_path,&members,&protect);
        if(group_result){
            /* Full census under this ownership already traversed every
             * existing parent. A missing native folder is now proven absent. */
            if(group_result!=-2)goto done;
        }
        if(protect>members)goto done;
        absent[i]=members==0;
    }
    if(ready(c,c->drive,c->generation))goto done;
    /* Commit the immutable batch only after all reads/scans passed. */
    for(unsigned i=0;i<census.count;i++)if(absent[i]){
        if(live_sync_enqueue(j,LIVE_SYNC_GC,&census.candidates[i])){j->count=0;goto done;}
    }
    c->gc_ready=1;c->gc_generation=c->generation;result=(int)j->count;
done:
    c->fs.unlock();return result;
}
