#include "sync.h"
static size_t length(const char *s,size_t n){size_t i=0;if(!s)return n;while(i<n&&s[i])i++;return i;}
static void copy(void *d,const void *s,size_t n){uint8_t *a=d;const uint8_t*b=s;while(n--)*a++=*b++;}
static int equal(const void*a,const void*b,size_t n){const uint8_t*x=a,*y=b;while(n--)if(*x++!=*y++)return 0;return 1;}
static int namechar(char c){return(c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_';}
static int uuid_ok(const char *u){if(length(u,37)!=36)return 0;for(unsigned i=0;i<36;i++){if(i==8||i==13||i==18||i==23){if(u[i]!='-')return 0;}else if(!((u[i]>='0'&&u[i]<='9')||(u[i]>='a'&&u[i]<='f')||(u[i]>='A'&&u[i]<='F')))return 0;}return 1;}
int live_sync_directory(char out[LIVE_SYNC_PATH],const char *p){
    /* A:\DCIM\100NCZ_8\DSC_0001.JPG (29 bytes). */
    if(!out||length(p,LIVE_SYNC_PATH)!=29||(p[0]!='A'&&p[0]!='B')||
       !equal(p+1,":\\DCIM\\",7)||p[16]!='\\'||p[25]!='.'||
       (!equal(p+26,"JPG",3)&&!equal(p+26,"NEF",3)))return -1;
    for(unsigned i=8;i<16;i++)if(!namechar(p[i]))return -1;
    for(unsigned i=17;i<25;i++)if(!namechar(p[i]))return -1;
    unsigned folder=0,number=0;
    for(unsigned i=8;i<11;i++){if(p[i]<'0'||p[i]>'9')return -1;folder=folder*10+(unsigned)(p[i]-'0');}
    for(unsigned i=21;i<25;i++){if(p[i]<'0'||p[i]>'9')return -1;number=number*10+(unsigned)(p[i]-'0');}
    if(folder<100||!number)return -1;
    out[0]=p[0];copy(out+1,":\\LIVE\\",7);copy(out+8,p+8,8);
    out[16]='\\';copy(out+17,p+17,8);out[25]=0;return 0;
}
static int number(const uint8_t *s,size_t n,uint64_t *v){
    if(!n||n>20)return -1;
    uint64_t x=0;
    for(size_t i=0;i<n;i++){if(s[i]<'0'||s[i]>'9')return -1;unsigned d=s[i]-'0';if(x>(UINT64_MAX-d)/10)return -1;x=x*10+d;}
    if(!x)return -1;
    *v=x;return 0;
}
int live_manifest_parse(const void *data,size_t bytes,LiveManifest *out){
    if(!data||!out||!bytes||bytes>LIVE_SYNC_MANIFEST_MAX)return -1;
    const uint8_t *p=data;LiveManifest m={0};unsigned fields=0;
    size_t start=0;
    while(start<bytes){
        size_t end=start;while(end<bytes&&p[end]!='\n'){if(!p[end]||p[end]=='\r')return -1;end++;}
        size_t n=end-start;const uint8_t *s=p+start;unsigned bit=0;
        if(n>=7&&equal(s,"native=",7)){
            bit=1;n-=7;if(n>=sizeof m.native_path)return -1;copy(m.native_path,s+7,n);
            char dir[LIVE_SYNC_PATH];if(live_sync_directory(dir,m.native_path))return -1;
        }else if(n>=5&&equal(s,"size=",5)){bit=2;if(number(s+5,n-5,&m.native_size))return -1;
        }else if(n>=5&&equal(s,"uuid=",5)){
            bit=4;if(n-5!=36)return -1;copy(m.uuid,s+5,36);if(!uuid_ok(m.uuid))return -1;
        }else if(n>=18&&equal(s,"volume_generation=",18)){bit=8;if(number(s+18,n-18,&m.volume_generation))return -1;
        }else return -1;
        if(fields&bit)return -1;
        fields|=bit;start=end+1;
    }
    if(fields!=15)return -1;
    *out=m;return 0;
}
static int valid(const LiveManifest *m){char d[LIVE_SYNC_PATH];return m&&m->native_size&&m->volume_generation&&uuid_ok(m->uuid)&&!live_sync_directory(d,m->native_path);}
static int same(const LiveManifest*a,const LiveManifest*b){return a->native_size==b->native_size&&a->volume_generation==b->volume_generation&&equal(a->uuid,b->uuid,37)&&equal(a->native_path,b->native_path,30);}
int live_sync_enqueue(LiveSyncJournal *j,unsigned op,const LiveManifest *m){
    if(!j||!valid(m)||(op!=LIVE_SYNC_DELETE&&op!=LIVE_SYNC_PROTECT&&op!=LIVE_SYNC_GC))return -1;
    if(j->count>=LIVE_SYNC_CAPACITY){j->overflow++;return -1;}
    LiveSyncEntry *e=&j->entries[(j->head+j->count)%LIVE_SYNC_CAPACITY];
    e->identity=*m;e->operation=(uint8_t)op;e->phase=0;j->count++;return 0;
}
static void filepath(char out[LIVE_SYNC_PATH],const char *dir,const char *file){size_t n=length(dir,LIVE_SYNC_PATH);copy(out,dir,n);out[n++]='\\';size_t m=length(file,16);copy(out+n,file,m+1);}
static void consume(LiveSyncJournal*j){j->head=(j->head+1)%LIVE_SYNC_CAPACITY;j->count--;}
int live_sync_step(LiveSyncJournal *j,const LiveSyncOps *o){
    if(!j||!o||!o->volume_ready||!o->read_manifest||!o->native_group||!o->remove_file||!o->remove_directory||!o->set_readonly)return -1;
    if(!j->count)return 0;
    LiveSyncEntry *e=&j->entries[j->head];char dir[LIVE_SYNC_PATH],path[LIVE_SYNC_PATH];
    if(!valid(&e->identity)||live_sync_directory(dir,e->identity.native_path)){consume(j);return 1;}
    if(o->volume_ready(o->ctx,e->identity.native_path[0],e->identity.volume_generation))return 2;
    unsigned members=0,protected_members=0;
    if(o->native_group(o->ctx,e->identity.native_path,&members,&protected_members)||protected_members>members)return 2;
    if(e->phase){
        if(!members&&o->remove_directory(o->ctx,dir))return 2;
        consume(j);return 1;
    }
    uint8_t buf[LIVE_SYNC_MANIFEST_MAX];size_t bytes=0;LiveManifest actual;
    filepath(path,dir,"MANIFEST");
    if(o->read_manifest(o->ctx,path,buf,sizeof buf,&bytes)||bytes>sizeof buf||live_manifest_parse(buf,bytes,&actual)||!same(&actual,&e->identity))return 2;
    if(e->operation!=LIVE_SYNC_PROTECT&&members){consume(j);return 1;}
    if(e->operation==LIVE_SYNC_PROTECT&&!members){consume(j);return 1;}
    static const char *const names[]={"PAIR.JPG","CLIP.MOV","PREV.LVP","MANIFEST"};
    for(unsigned i=0;i<4;i++){
        /* Revalidate media/epoch before EACH destructive callback. The native
         * adapter holds media mutation ownership for the complete step. */
        if(o->volume_ready(o->ctx,e->identity.native_path[0],e->identity.volume_generation))return 2;
        filepath(path,dir,names[i]);
        if(e->operation==LIVE_SYNC_PROTECT){if(o->set_readonly(o->ctx,path,protected_members!=0))return 2;
        }else{
            if(o->set_readonly(o->ctx,path,0)||o->remove_file(o->ctx,path))return 2;
            if(i==3)e->phase=1;
        }
        if(o->yield)o->yield(o->ctx);
    }
    if(e->operation!=LIVE_SYNC_PROTECT){
        if(o->volume_ready(o->ctx,e->identity.native_path[0],e->identity.volume_generation)||o->remove_directory(o->ctx,dir))return 2;
    }
    consume(j);return 1;
}
