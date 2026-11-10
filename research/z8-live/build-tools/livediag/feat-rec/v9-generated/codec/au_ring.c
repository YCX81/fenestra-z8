#include "au_ring.h"
#include <string.h>
void au_ring_init(struct au_ring *r,uint8_t *data,uint32_t capacity) {
 *r=(struct au_ring){0};r->data=data;r->capacity=capacity;
}
static void pop(struct au_ring *r) {
 r->used-=r->entries[r->head].size;r->head=(r->head+1)%AU_RING_SLOTS;r->count--;
}
int au_ring_push(struct au_ring *r,const uint8_t *data,uint32_t n,uint64_t pts,unsigned idr) {
 if(!r || !r->data || !data || !n || n>r->capacity || (r->count && pts<=r->entries[(r->head+r->count-1)%AU_RING_SLOTS].pts))return -1;
 while(r->count && (r->capacity-r->used<n || r->count==AU_RING_SLOTS))pop(r);
 uint32_t offset=r->write;
 uint32_t first=n<r->capacity-r->write?n:r->capacity-r->write;
 memcpy(r->data+r->write,data,first);memcpy(r->data,data+first,n-first);r->write=(r->write+n)%r->capacity;
 r->entries[(r->head+r->count)%AU_RING_SLOTS]=(struct au_entry){pts,offset,n,!!idr};r->count++;r->used+=n;return 0;
}
int au_ring_find_start(const struct au_ring *r,uint64_t target,unsigned *index) {
 if(!r||!index)return -1;
 int found=0;
 for(unsigned i=0;i<r->count;i++){const struct au_entry *e=&r->entries[(r->head+i)%AU_RING_SLOTS];if(e->pts>target)break;if(e->idr){*index=i;found=1;}}
 return found?0:-1;
}
int au_ring_copy(const struct au_ring *r,unsigned index,uint8_t *dst,uint32_t capacity) {
 if(!r||!dst||index>=r->count)return -1;
 const struct au_entry *e=&r->entries[(r->head+index)%AU_RING_SLOTS];if(capacity<e->size)return -1;
 uint32_t first=e->size<r->capacity-e->offset?e->size:r->capacity-e->offset;
 memcpy(dst,r->data+e->offset,first);memcpy(dst+first,r->data,e->size-first);return (int)e->size;
}
