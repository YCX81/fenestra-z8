#include "au_ring.h"
#include <string.h>
typedef uint64_t __attribute__((may_alias)) au_u64a;
/* Strictly-aligned 64-bit copy (works at full 8-byte width even when d and s
 * differ mod 8, avoiding the stock 1-byte-at-a-time memcpy under -fno-builtin). */
void live_fast_copy(uint8_t *d,const uint8_t *s,size_t n) {
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
 if(n>=16u){
  while((uintptr_t)d&7u){*d++=*s++;n--;}
  unsigned off=(unsigned)((uintptr_t)s&7u);
  if(!off){
   au_u64a *dw=(au_u64a *)(void *)d;const au_u64a *sw=(const au_u64a *)(const void *)s;
   size_t q=n>>3;for(size_t i=0;i<q;i++)dw[i]=sw[i];
   d+=q<<3;s+=q<<3;n&=7u;
  }else if(n>=16u){
   unsigned k=8u-off,shl=k*8u,shr=off*8u;uint64_t carry=0;
   for(unsigned i=0;i<k;i++)carry|=(uint64_t)s[i]<<(i*8u);
   s+=k;
   while(n>=16u){
    uint64_t cur=*(const au_u64a *)(const void *)s;
    *(au_u64a *)(void *)d=carry|(cur<<shl);
    carry=cur>>shr;d+=8;s+=8;n-=8;
   }
   for(unsigned i=0;i<k;i++)d[i]=(uint8_t)(carry>>(i*8u));
   d+=k;n-=k;
  }
 }
#endif
 while(n--)*d++=*s++;
}
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
 live_fast_copy(r->data+r->write,data,first);live_fast_copy(r->data,data+first,n-first);r->write=(r->write+n)%r->capacity;
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
 live_fast_copy(dst,r->data+e->offset,first);live_fast_copy(dst+first,r->data,e->size-first);return (int)e->size;
}
