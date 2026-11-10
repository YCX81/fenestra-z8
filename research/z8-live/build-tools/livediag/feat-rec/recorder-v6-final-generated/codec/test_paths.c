#include "live_transaction.h"
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
