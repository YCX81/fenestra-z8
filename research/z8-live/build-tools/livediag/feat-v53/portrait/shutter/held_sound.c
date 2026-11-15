#include "held_sound.h"
typedef void (*reply_fn)(uint32_t,void *);
static uint32_t r32(uintptr_t p){return *(const uint32_t *)p;}
static uintptr_t r64(uintptr_t p){return *(const uintptr_t *)p;}
int held_sound_cancel(struct held_sound *h,void *cancel_message){
 if(__atomic_load_n(&h->state,__ATOMIC_ACQUIRE)!=1u)return 0;
 uintptr_t start=(uintptr_t)h->message,stop=(uintptr_t)cancel_message;
 if(!start||!stop||start==stop)return -1;
 if(r32(0xa038feb74ull)!=0u||r64(0xa038febb8ull)!=0u)return -1;
 uint32_t se=r32(start+32),ce=r32(stop+32);
 if(se!=0x04100030u||(ce!=0x04100031u&&ce!=0x04100032u))return -1;
 uintptr_t so=r64(start+40),co=r64(stop+40);
 if(!so||!co||so==co)return -1;
 if(r64(start+24)!=0x40e32ed8u||r64(stop+24)!=0x40e32ed8u||
    r64(start+48)!=start||r64(stop+48)!=stop||
    r64(so+8)!=0x40e3159cu||r64(co+8)!=0x40e3159cu||r64(co)!=0)return -1;
 uintptr_t inner=r64(so);if(!inner)return -1;
 uint32_t tone=r32(inner+12);
 if((tone!=1u&&tone!=2u)||r32(inner+8)!=1u||r64(inner+24)!=0x40f9d194u)return -1;
 uint32_t expected=1;
 if(!__atomic_compare_exchange_n(&h->state,&expected,2,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return 0;
 /* Same complete rejection contract as 40e34ddc..e30 + dispatcher cleanup.
  * No native queue/DMA ever owned this retained start. Never claim it played. */
 uint32_t result[2]={tone,0};
 reply_fn inner_reply=(reply_fn)r64(inner+24);
 inner_reply(0x04102004u,result);inner_reply(0x04102005u,result);
 *(uint32_t *)(so+16)=1; /* failed start, exact dispatcher inversion */
 ((reply_fn)r64(start+24))(0x04102000u,(void *)r64(start+48));
 /* The cancellation succeeded in software; no hardware STOP is needed.
  * Complete original allocated cancel envelope, preserving cleanup/getters. */
 *(uint32_t *)(co+16)=0;
 ((reply_fn)r64(stop+24))(ce+0x1fd0u,(void *)r64(stop+48));
 h->message=0;__atomic_store_n(&h->state,3,__ATOMIC_RELEASE);return 1;
}
