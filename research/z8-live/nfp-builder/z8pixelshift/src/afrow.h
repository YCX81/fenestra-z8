/* Candidate NX 1.11-compatible AF-row rule; not linked into a camera package yet. */
#ifndef PS8_AFROW_H
#define PS8_AFROW_H
#include <stdint.h>
/* Columns: BP, RP, GP1+GP2, BN, RN, GN1+GN2, branch-5 guard.
 * For blue source slot%4==0 the guard is the original fourth frame at the same sensor coordinate
 * (output P+scale in these lattices); otherwise it is BP. Preserve green sums before truncating RGB G.
 */
void ps8_af_one_batch(const uint16_t *features,uint16_t *out,uint32_t n);
void ps8_af_pair_batch(const uint16_t *features,uint16_t *out,uint32_t n);
uint16_t ps8_af_pair(uint16_t a,uint16_t b,uint32_t row_a,uint32_t row_b);
#endif
