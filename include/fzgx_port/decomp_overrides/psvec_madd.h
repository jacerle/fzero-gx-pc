/* fzero-gx-pc native replacement for external/fzero-gx-decomp/include/psvec_madd.h
 * (R4 platform boundary: the port supplies the hardware/asm kernel headers). Same API and
 * include guard as the original; the paired multiply-add becomes C. */

#ifndef PSVEC_MADD_H
#define PSVEC_MADD_H

#include "types.h"

/* acc += v * s, in place (fused) */
static inline void psvec_madd(void *acc, const void *v, f32 s) {
    f32 *pa = (f32 *)acc;
    const f32 *pv = (const f32 *)v;
    pa[0] = fzgx_fmaf(pv[0], s, pa[0]);
    pa[1] = fzgx_fmaf(pv[1], s, pa[1]);
    pa[2] = fzgx_fmaf(pv[2], s, pa[2]);
}

#endif
