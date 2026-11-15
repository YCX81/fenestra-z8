#include "au_ring.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void){uint8_t storage[10],out[8];struct au_ring r;unsigned i;au_ring_init(&r,storage,sizeof(storage));
 assert(!au_ring_push(&r,(const uint8_t *)"abc",3,10,1));assert(!au_ring_push(&r,(const uint8_t *)"defg",4,20,0));
 assert(!au_ring_push(&r,(const uint8_t *)"hijk",4,30,1));assert(r.count==2&&r.used==8);assert(au_ring_find_start(&r,29,&i));
 assert(!au_ring_find_start(&r,30,&i)&&i==1);assert(au_ring_copy(&r,i,out,sizeof(out))==4&&!memcmp(out,"hijk",4));
 assert(au_ring_push(&r,(const uint8_t *)"x",1,30,0));assert(au_ring_copy(&r,1,out,3)<0);puts("AU ring wrap, eviction, IDR selection: passed");}
