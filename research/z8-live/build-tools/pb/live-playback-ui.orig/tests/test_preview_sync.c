#include "../preview_writer.h"
#include "../sync.h"
#include "../../live-playback/preview.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char manifest[]="native=A:\\DCIM\\100NCZ_8\\DSC_0001.JPG\nsize=1169905\nuuid=19FEF38C-CA15-4047-D02B-A6A72CC4A198\nvolume_generation=1\n";
typedef struct {unsigned frames,writes,yields;size_t used;int short_write,fail_frame;uint8_t *data;} Preview;
static int frame(void *v,uint32_t i,void *p,size_t n){Preview*s=v;s->frames++;if((int)i==s->fail_frame)return -1;memset(p,(int)i,n);return 0;}
static int write_data(void *v,const void *p,size_t n,size_t *done){Preview*s=v;s->writes++;if(s->short_write){*done=n-1;return 0;}assert(s->used+n<=LIVE_PREVIEW_BYTES);memcpy(s->data+s->used,p,n);s->used+=n;*done=n;return 0;}
static void preview_yield(void*v){((Preview*)v)->yields++;}
static void preview_test(void){
    Preview p={.fail_frame=-1,.data=malloc(LIVE_PREVIEW_BYTES)};void *scratch=malloc(LIVE_PREVIEW_FRAME_BYTES);assert(p.data&&scratch);
    LivePreviewWriter w={&p,frame,write_data,preview_yield};
    assert(live_preview_write(&w,scratch,LIVE_PREVIEW_FRAME_BYTES)==0);
    assert(p.used==LIVE_PREVIEW_BYTES&&p.frames==45&&p.writes==46&&p.yields==45);
    LpPreview parsed;assert(lp_preview_open(p.data,p.used,&parsed,77)==0);
    assert(parsed.width==320&&parsed.height==212&&parsed.frames==45&&parsed.duration_us==3000015);
    for(unsigned i=0;i<45;i++){uint64_t offset=0;assert(lp_preview_frame(&parsed,(uint64_t)i*66667,77,&offset)==0);assert(offset==32+(uint64_t)i*LIVE_PREVIEW_FRAME_BYTES);for(unsigned j=0;j<LIVE_PREVIEW_FRAME_BYTES;j++)assert(p.data[offset+j]==i);}
    uint64_t offset;assert(lp_preview_frame(&parsed,3000015,77,&offset)==1);assert(lp_preview_frame(&parsed,0,78,&offset)==-1);
    p.used=p.frames=p.writes=p.yields=0;p.short_write=1;assert(live_preview_write(&w,scratch,LIVE_PREVIEW_FRAME_BYTES)==-1);assert(p.frames==0&&p.writes==1);
    p.short_write=0;p.fail_frame=2;p.used=p.frames=p.writes=p.yields=0;assert(live_preview_write(&w,scratch,LIVE_PREVIEW_FRAME_BYTES)==-1);assert(p.frames==3&&p.writes==3&&p.yields==2);
    p.frames=p.writes=0;assert(live_preview_write(&w,scratch,LIVE_PREVIEW_FRAME_BYTES-1)==-1);assert(!p.frames&&!p.writes);
    free(scratch);free(p.data);
}
typedef struct {
    unsigned jpg,nef,protected_members,ready_calls,read_calls,group_calls,mutations,yields;
    unsigned exists[4],readonly[4],remove_order[4],removes,dir_removed;
    int unreadable,manifest_bad,group_error,fail_dir,fail_file,change_after;
    uint64_t epoch;char volume;
} Card;
static int index_of(const char *path){
    static const char *const names[]={"PAIR.JPG","CLIP.MOV","PREV.LVP","MANIFEST"};
    const char *root="A:\\LIVE\\100NCZ_8\\DSC_0001\\";size_t n=strlen(root);
    assert(strncmp(path,root,n)==0);for(unsigned i=0;i<4;i++)if(!strcmp(path+n,names[i]))return (int)i;
    assert(0&&"non-allowlisted path reached mutation adapter");return -1;
}
static int ready(void*v,char drive,uint64_t gen){Card*s=v;s->ready_calls++;assert(drive==s->volume);if(s->change_after&&s->mutations>=(unsigned)s->change_after)return -1;return s->unreadable||gen!=s->epoch?-1:0;}
static int read_manifest(void*v,const char*p,void*out,size_t cap,size_t *n){Card*s=v;s->read_calls++;assert(index_of(p)==3);if(s->manifest_bad||!s->exists[3])return -1;assert(cap>=sizeof manifest-1);memcpy(out,manifest,sizeof manifest-1);*n=sizeof manifest-1;return 0;}
static int native_group(void*v,const char*p,unsigned *members,unsigned *protect){Card*s=v;s->group_calls++;assert(!strcmp(p,"A:\\DCIM\\100NCZ_8\\DSC_0001.JPG"));if(s->group_error)return -1;*members=s->jpg+s->nef;*protect=s->protected_members;return 0;}
static int remove_file(void*v,const char*p){Card*s=v;int i=index_of(p);s->mutations++;assert(!s->jpg&&!s->nef);if(i==s->fail_file)return -1;assert(!s->readonly[i]);if(s->exists[i]){s->remove_order[s->removes++]=(unsigned)i;s->exists[i]=0;}return 0;}
static int remove_dir(void*v,const char*p){Card*s=v;s->mutations++;assert(!strcmp(p,"A:\\LIVE\\100NCZ_8\\DSC_0001"));assert(!s->jpg&&!s->nef);for(unsigned i=0;i<4;i++)assert(!s->exists[i]);if(s->fail_dir)return -1;s->dir_removed=1;return 0;}
static int readonly(void*v,const char*p,int ro){Card*s=v;int i=index_of(p);s->mutations++;s->readonly[i]=(unsigned)ro;return 0;}
static void sync_yield(void*v){((Card*)v)->yields++;}
static Card fresh(void){Card s={.exists={1,1,1,1},.epoch=1,.volume='A',.fail_file=-1};return s;}
static LiveSyncOps ops(Card*s){LiveSyncOps o={s,ready,read_manifest,native_group,remove_file,remove_dir,readonly,sync_yield};return o;}
static void manifest_test(void){
    LiveManifest m;char dir[64];assert(live_manifest_parse(manifest,sizeof manifest-1,&m)==0);assert(m.native_size==1169905&&m.volume_generation==1);assert(live_sync_directory(dir,m.native_path)==0);assert(!strcmp(dir,"A:\\LIVE\\100NCZ_8\\DSC_0001"));
    assert(live_sync_directory(dir,"A:\\DCIM\\100NCZ_8\\DSC_0001.NEF")==0);
    const char *bad[]={"A:\\DCIM\\100NCZ_8\\DSC_0000.JPG","A:\\DCIM\\099NCZ_8\\DSC_0001.JPG","A:\\DCIM\\100NCZ_8\\..C_0001.JPG","A:\\DCIM\\100NCZ_8\\DSC_0001.JPG/../CLIP.MOV","C:\\DCIM\\100NCZ_8\\DSC_0001.JPG","A:/DCIM/100NCZ_8/DSC_0001.JPG","A:\\LIVE\\100NCZ_8\\DSC_0001.JPG"};
    for(unsigned i=0;i<sizeof bad/sizeof *bad;i++)assert(live_sync_directory(dir,bad[i])==-1);
    char buf[300];snprintf(buf,sizeof buf,"%ssize=1\n",manifest);assert(live_manifest_parse(buf,strlen(buf),&m)==-1);
    snprintf(buf,sizeof buf,"native=A:\\DCIM\\100NCZ_8\\DSC_0001.JPG\nsize=18446744073709551616\nuuid=19FEF38C-CA15-4047-D02B-A6A72CC4A198\nvolume_generation=1\n");assert(live_manifest_parse(buf,strlen(buf),&m)==-1);
    memcpy(buf,manifest,sizeof manifest);buf[9]=0;assert(live_manifest_parse(buf,sizeof manifest-1,&m)==-1);
    assert(live_manifest_parse(manifest,sizeof manifest-2,&m)==0); /* final LF optional */
    assert(live_manifest_parse(manifest,20,&m)==-1);
}
static void sync_test(void){
    LiveManifest m;assert(!live_manifest_parse(manifest,sizeof manifest-1,&m));LiveSyncJournal j={0};Card c=fresh();LiveSyncOps o=ops(&c);
    c.nef=1;assert(!live_sync_enqueue(&j,LIVE_SYNC_DELETE,&m));assert(!c.read_calls);assert(live_sync_step(&j,&o)==1);assert(!c.mutations&&!c.dir_removed); /* JPEG removed, RAW remains. */
    c.nef=0;assert(!live_sync_enqueue(&j,LIVE_SYNC_DELETE,&m));assert(live_sync_step(&j,&o)==1);assert(c.dir_removed&&c.removes==4&&c.yields==4);for(unsigned i=0;i<4;i++)assert(c.remove_order[i]==i); /* MANIFEST last. */
    c=fresh();o=ops(&c);c.jpg=c.nef=1;c.protected_members=1;assert(!live_sync_enqueue(&j,LIVE_SYNC_PROTECT,&m));assert(live_sync_step(&j,&o)==1);for(unsigned i=0;i<4;i++)assert(c.readonly[i]);assert(c.jpg&&c.nef&&!c.removes);
    c.protected_members=0;assert(!live_sync_enqueue(&j,LIVE_SYNC_PROTECT,&m));assert(live_sync_step(&j,&o)==1);for(unsigned i=0;i<4;i++)assert(!c.readonly[i]);
    for(unsigned reason=0;reason<5;reason++){
        c=fresh();o=ops(&c);c.unreadable=reason==0;c.epoch=reason==1?2:1;c.manifest_bad=reason==2;c.group_error=reason==3;LiveManifest id=m;if(reason==4)id.native_size++;
        assert(!live_sync_enqueue(&j,LIVE_SYNC_GC,&id));assert(live_sync_step(&j,&o)==2);assert(j.count==1&&!c.mutations);j=(LiveSyncJournal){0};
    }
    c=fresh();o=ops(&c);c.change_after=2;assert(!live_sync_enqueue(&j,LIVE_SYNC_DELETE,&m));assert(live_sync_step(&j,&o)==2);assert(c.removes==1&&!c.dir_removed&&j.count==1);c.change_after=0;assert(live_sync_step(&j,&o)==1&&c.dir_removed); /* idempotent retry */
    c=fresh();o=ops(&c);c.fail_dir=1;assert(!live_sync_enqueue(&j,LIVE_SYNC_DELETE,&m));assert(live_sync_step(&j,&o)==2);assert(!c.exists[3]&&j.entries[j.head].phase==1);c.fail_dir=0;assert(live_sync_step(&j,&o)==1&&c.dir_removed);
    c=fresh();o=ops(&c);c.fail_file=1;assert(!live_sync_enqueue(&j,LIVE_SYNC_DELETE,&m));assert(live_sync_step(&j,&o)==2);assert(c.exists[3]&&j.count==1);c.fail_file=-1;assert(live_sync_step(&j,&o)==1&&c.dir_removed);
    for(unsigned i=0;i<LIVE_SYNC_CAPACITY;i++)assert(!live_sync_enqueue(&j,LIVE_SYNC_GC,&m));assert(live_sync_enqueue(&j,LIVE_SYNC_GC,&m)==-1&&j.overflow==1);
    LiveManifest unsafe=m;strcpy(unsafe.native_path,"A:\\DCIM\\100NCZ_8\\..C_0001.JPG");j=(LiveSyncJournal){0};assert(live_sync_enqueue(&j,LIVE_SYNC_DELETE,&unsafe)==-1&&j.count==0);
    assert(live_sync_step(&j,&o)==0);
}
int main(void){preview_test();manifest_test();sync_test();puts("preview_sync: PASS (LVP1, stream errors, manifest, RAW+JPEG, protect, media/error guards, path confinement, retries, queue)");return 0;}
