#!/usr/bin/env python3
"""Flat Live file layout (2026-11-05), for A:\\DCIM\\100NCZ_8\\DSC_0001.JPG:
    A:\\LIVE\\100NCZ_8\\DSC_0001.JPG / .MOV       importable Live Photo pair (same stem)
    A:\\LIVEDATA\\100NCZ_8\\DSC_0001.LVP / .MAN   camera-only preview + manifest
replacing LIVE\\100NCZ_8\\DSC_0001\\{PAIR.JPG,CLIP.MOV,PREV.LVP,MANIFEST}. The playback
layer (build-tools/pb/live-playback-ui) derives the same four paths in sync.c.

usage: flat_patch.py SRC_DIR   (src-w5 or src-audio tree)"""
import sys
from pathlib import Path

src = Path(sys.argv[1])


def patch(path, old, new):
    t = path.read_text()
    assert t.count(old) == 1, f'{path}: {old[:70]!r}'
    path.write_text(t.replace(old, new))


tx = src / 'codec/live_transaction.c'
t = tx.read_text()
start = t.index('int live_paths(const char*source,char jpg[128],char mov[128]){')
end = t.index('\n}\n', start) + 3
t = t[:start] + r'''/* <drive><sep><top><sep><DCF folder><sep><stem><ext>, keeping the source separator. */
static int layout(const char*source,const char*top,size_t topn,const char*ext,char out[128]){
 if(!source||!out)return -1;
 size_t n=0,base=0,folder=0;while(source[n]){if(n>=90)return -1;if(sep(source[n])){folder=base;base=n+1;}n++;}
 if(n-base!=12||source[base+8]!='.'||base-folder!=9||folder<6)return -1;
 for(unsigned i=0;i<8;i++){char c=source[base+i];if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'))return -1;}
 size_t drive=0;while(drive<n && !sep(source[drive]))drive++;
 if(drive==n || drive>3)return -1;
 char delimiter=source[drive];char *p=out;memcpy(p,source,drive);p+=drive;*p++=delimiter;memcpy(p,top,topn);p+=topn;*p++=delimiter;
 memcpy(p,source+folder,8);p+=8;*p++=delimiter;memcpy(p,source+base,8);p+=8;memcpy(p,ext,5);return 0;
}
int live_paths(const char*source,char jpg[128],char mov[128]){
 if(!jpg||!mov)return -1;
 return layout(source,"LIVE",4,".JPG",jpg)||layout(source,"LIVE",4,".MOV",mov)?-1:0;
}
int live_data_paths(const char*source,char man[128],char lvp[128]){
 if(!man||!lvp)return -1;
 return layout(source,"LIVEDATA",8,".MAN",man)||layout(source,"LIVEDATA",8,".LVP",lvp)?-1:0;
}
''' + t[end:]
tx.write_text(t)

patch(tx, ''' if(!f->ensure_dir)return -1;
 size_t root=0;while(jpg[root])root++;while(root && !sep(jpg[root-1]))root--;
 memcpy(manifest,jpg,root);memcpy(manifest+root,"MANIFEST",9);memcpy(prev,jpg,root);memcpy(prev+root,"PREV.LVP",9);
 for(size_t i=0;i<root;i++)if(sep(jpg[i]) && i && !(i==2 && jpg[1]==':')){
   char save=jpg[i];jpg[i]=0;int er=f->ensure_dir(f->opaque,jpg);jpg[i]=save;if(er)goto done;
 }''', ''' if(!f->ensure_dir||live_data_paths(t->native_path,manifest,prev))return -1;
 /* Parents of both file sets: LIVE, LIVE\\folder, LIVEDATA, LIVEDATA\\folder. */
 char *parents[2]={jpg,manifest};
 for(unsigned k=0;k<2;k++){char *q=parents[k];size_t root=0;while(q[root])root++;while(root && !sep(q[root-1]))root--;
  for(size_t i=0;i<root;i++)if(sep(q[i]) && i && !(i==2 && q[1]==':')){
   char save=q[i];q[i]=0;int er=f->ensure_dir(f->opaque,q);q[i]=save;if(er)goto done;
  }}''')

patch(src / 'codec/live_transaction.h',
      '''/* Same-volume LIVE/folder/stem paths; native playback enumeration remains unverified. */
int live_paths(const char *source, char jpg[128], char mov[128]);''',
      '''/* Flat same-volume layout: LIVE/folder/stem.JPG|.MOV (importable pair) and
   LIVEDATA/folder/stem.MAN|.LVP (camera-only), stem = native DCF stem. */
int live_paths(const char *source, char jpg[128], char mov[128]);
int live_data_paths(const char *source, char man[128], char lvp[128]);''')

(src / 'codec/test_paths.c').write_text(r'''#include "live_transaction.h"
#include <assert.h>
#include <string.h>
int main(void) {
 char jpg[128], mov[128], man[128], lvp[128];
 assert(!live_paths("A:\\DCIM\\100NCZ_8\\DSC_0001.JPG",jpg,mov));
 assert(!strcmp(jpg,"A:\\LIVE\\100NCZ_8\\DSC_0001.JPG"));
 assert(!strcmp(mov,"A:\\LIVE\\100NCZ_8\\DSC_0001.MOV"));
 assert(!live_data_paths("A:\\DCIM\\100NCZ_8\\DSC_0001.JPG",man,lvp));
 assert(!strcmp(man,"A:\\LIVEDATA\\100NCZ_8\\DSC_0001.MAN"));
 assert(!strcmp(lvp,"A:\\LIVEDATA\\100NCZ_8\\DSC_0001.LVP"));
 assert(!live_paths("B:/DCIM/100NCZ_8/ABC_0001.JPG",jpg,mov));
 assert(!strcmp(jpg,"B:/LIVE/100NCZ_8/ABC_0001.JPG"));
 assert(!live_data_paths("B:/DCIM/101NCZ_8/ABC_0002.NEF",man,lvp));
 assert(!strcmp(lvp,"B:/LIVEDATA/101NCZ_8/ABC_0002.LVP"));
 assert(live_paths("A/DSC_01.JPG",jpg,mov));
 assert(live_paths(0,jpg,mov));
 assert(live_data_paths(0,man,lvp));
 return 0;
}
''')
print('flat patch applied')
