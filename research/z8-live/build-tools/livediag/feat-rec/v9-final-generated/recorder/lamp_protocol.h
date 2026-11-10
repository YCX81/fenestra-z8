#ifndef FR_LAMP_PROTOCOL_H
#define FR_LAMP_PROTOCOL_H
#include <stdint.h>
/* MSB-first: stage[2], code[3], optional raw[32/8], CRC[4].
 * CRC polynomial x^4+x+1, init=0, xorout=0, no reflection, no extra zero bits.
 * I code=0; R code=0..3 (actual NR skip mask). S codes 0..5, 6=escape. M 0/1/2 => 0/1/ff, 6=escape.
 * Other codewords are reserved and MUST be rejected by the decoder. */
static inline uint64_t fr_lamp_bits(uint32_t stage,uint32_t value,unsigned *count){
 unsigned ext=0,code=stage==1?(value&3u):0;
 if(stage==2){code=value<=5?value:6;ext=code==6?32:0;}
 if(stage==3){code=value==0?0:value==1?1:value==255?2:6;ext=code==6?8:0;}
 unsigned n=5+ext;
 uint64_t bits=((uint64_t)stage<<3)|code;
 /* D uses a distinct half-speed sync: stage[3]=100, wire_diag[5], detail[2].
  * v7: D1 detail=busy(bit0)/alive(bit1), D4=1, D5=0.
  * Logical D3 uses wire_diag=6+busy, detail=saturated seconds (0..3).
  * Other D detail fields retain the actual NR mask.
  * I/R/S/M bits and timings are unchanged. */
 if(stage==4){n=10;bits=((uint64_t)4<<7)|(value&127u);}
 if(stage==5){n=15;bits=((uint64_t)5<<12)|(value&4095u);}
 if(ext)bits=(bits<<ext)|(ext==8?(value&255u):value);
 uint32_t crc=0;
 for(unsigned i=n;i;i--){
  unsigned feedback=((crc>>3)^(unsigned)(bits>>(i-1)))&1u;
  crc=((crc<<1)&15u)^(feedback?3u:0u);
 }
 *count=n+4;return (bits<<4)|crc;
}
#endif
