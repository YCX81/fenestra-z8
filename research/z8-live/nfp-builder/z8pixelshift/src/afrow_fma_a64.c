/* Freestanding AArch64 leaf for the C compiler's fma call. No system-register writes or runtime library. */
#if !defined(__aarch64__)
#error AArch64 only
#endif
double fma(double a,double b,double c){
 double out;
 __asm__("fmadd %d0, %d1, %d2, %d3" : "=w"(out) : "w"(a),"w"(b),"w"(c));
 return out;
}
