/* Self-contained arithmetic. Compile without fast-math and with -ffp-contract=off.
 * Explicit fma in the 0.3/0.7 paths matches the native library; ordinary half mixes are exact products.
 * Region and neighbour/guard selection belong to the caller and are independently checked in full_af_check.py.
 */
#include "afrow.h"
static double mix(double bp,double est,double w,double v){return __builtin_fma(bp,w,est*v);}
static uint16_t one(const uint16_t *f){
 double bp=f[0],rp=f[1],gp=f[2]*0.5,bn=f[3],rn=f[4],gn=f[5]*0.5,guard=f[6];
 if(!rp||!rn||!gp||!bp)return (uint16_t)bp; /* explicit undefined-input fallback, untested against NX */
 float q=(float)rn/(float)rp;
 double est=bn/(double)q,r=rn/rp,a=gn/gp,t=bn/bp,out=bp;
 if(a<=r+.1&&a<=r&&a>t+.1)out=mix(bp,est,.7,.3);
 if(a<=r+.1&&a>r&&r>t+.1)out=mix(bp,est,.3,.7);
 if(a>r+.1&&t<=r+.1&&r<=t+.1)out=mix(bp,est,.5,.5);
 if(a>t+.1&&r>t+.1&&r+.1>a){
  if(r>=1.1)out=mix(bp,est,.5,.5);
  else out=(guard<=est+2000&&rp<=est+300)?est:bp;
 }
 if(a>t+.1&&r<=t+.1&&t<=r+.1)out=t<=r?est:bp;
 if(a<=t+.1&&t>a+.1&&t<=r+.3&&rp/gp>bp/gp)out=mix(bp,est,.5,.5);
 if(a<=t+.1&&t>a+.1&&t>r+.3&&gp>2000&&bp/gp+.4>rp/gp)out=(est>bp+500&&gp<=est+500)?bp:est;
 if(a<=t+.1&&t<=a+.1&&t>a&&r+.1>t)out=mix(bp,est,.5,.5);
 if(out>16383)out=16383;
 return (uint16_t)out;
}
#ifdef PSM_FIRMWARE_AF
__attribute__((section(".text.entry")))
#endif
void ps8_af_one_batch(const uint16_t *f,uint16_t *out,uint32_t n){for(uint32_t i=0;i<n;i++)out[i]=one(f+7u*i);}
static int af(uint32_t y){return y>=29&&y<=5489&&y%12==5;}
uint16_t ps8_af_pair(uint16_t a,uint16_t b,uint32_t ya,uint32_t yb){return af(ya)?b:af(yb)?a:(uint16_t)(((uint32_t)a+b)>>1);}
void ps8_af_pair_batch(const uint16_t *f,uint16_t *out,uint32_t n){for(uint32_t i=0;i<n;i++)out[i]=ps8_af_pair(f[4*i],f[4*i+1],f[4*i+2],f[4*i+3]);}
