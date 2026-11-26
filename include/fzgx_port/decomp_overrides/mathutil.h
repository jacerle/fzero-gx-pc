/* fzero-gx-pc native replacement for external/fzero-gx-decomp/include/mathutil.h
 * (R4 platform boundary: the port supplies the hardware/asm kernel headers). Same API and
 * include guard as the original; the FPR kernel becomes C. */

#ifndef MATHUTIL_H
#define MATHUTIL_H

#include "types.h"

/* a*a + b*b + c*c, accumulated as fmuls; fmadds; fmadds. */
static inline f32 mathutil_sum_of_sq_3(f32 a, f32 b, f32 c) {
    return fzgx_fmaf(c, c, fzgx_fmaf(b, b, a * a));
}

#endif  // MATHUTIL_H
