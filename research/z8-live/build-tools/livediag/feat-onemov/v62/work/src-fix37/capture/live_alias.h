#ifndef LIVE_ALIAS_H
#define LIVE_ALIAS_H
#include <stdint.h>
#include <stddef.h>
#ifndef __aarch64__
/* host replay build: no alias, cache maintenance is a no-op */
static inline uintptr_t la_alias(uintptr_t va,size_t n){(void)va;(void)n;return 0;}
static inline void la_dc_ivac(uintptr_t p,size_t n){(void)p;(void)n;}
static inline void la_dc_cvac(uintptr_t p,size_t n){(void)p;(void)n;}
static inline void la_dc_civac(uintptr_t p,size_t n){(void)p;(void)n;}
#else
/* PAR_EL1 of an EL1 read translation; interrupts masked and PAR restored (Fenestra pattern). */
static inline uint64_t la_par(uintptr_t va) {
 uint64_t daif,old,par;
 __asm__ volatile("mrs %0,daif\n msr daifset,#15\n mrs %1,par_el1\n at s1e1r,%3\n isb\n mrs %2,par_el1\n msr par_el1,%1\n msr daif,%0"
                  :"=&r"(daif),"=&r"(old),"=&r"(par):"r"(va):"memory");
 return par;
}
/* VA+0x400000000 when every page of [va,va+n) maps the same PA there with MAIR 0xff; else 0. */
static inline uintptr_t la_alias(uintptr_t va,size_t n) {
 if(!n||va<UINT64_C(0x800000000)||va+n>UINT64_C(0xc00000000))return 0;
 uintptr_t d=UINT64_C(0x400000000);
 for(uintptr_t p=va&~(uintptr_t)4095;p<va+n;p+=4096){
  uint64_t a=la_par(p),b=la_par(p+d);
  if((a|b)&1)return 0;
  if(((a^b)&UINT64_C(0x0000fffffffff000))!=0)return 0;
  if((b>>56)!=0xffu)return 0;
 }
 return va+d;
}
static inline unsigned la_line(void){uint64_t ctr;__asm__ volatile("mrs %0,ctr_el0":"=r"(ctr));return 4u<<((ctr>>16)&15);}
#define LA_DC(name,op) static inline void name(uintptr_t p,size_t n){ \
 if(!n)return;unsigned l=la_line();uintptr_t e=p+n;p&=~(uintptr_t)(l-1); \
 for(;p<e;p+=l)__asm__ volatile("dc " op ", %0"::"r"(p):"memory"); \
 __asm__ volatile("dsb sy":::"memory");}
LA_DC(la_dc_ivac,"ivac")
LA_DC(la_dc_cvac,"cvac")
LA_DC(la_dc_civac,"civac")
#endif
#endif
