/* fzero-gx-pc native replacement for external/fzero-gx-decomp/include/psvec_fill.h
 * (R4 platform boundary: the port supplies the hardware/asm kernel headers). Same API and
 * include guard as the original; the paired-store kernel becomes C. */

#ifndef PSVEC_FILL_H
#define PSVEC_FILL_H

#include "types.h"

/* dst[0..11] = v */
static inline void psvec_fill4(void *dst, f32 v) {
    f32 *out = (f32 *)dst;
    int i;
    for (i = 0; i < 12; i++) {
        out[i] = v;
    }
}

#endif  // PSVEC_FILL_H
