/* fzero-gx-pc native replacement for external/fzero-gx-decomp/include/mathutil_vec_dot_prod.h
 * (R4 platform boundary: the port supplies the hardware/asm kernel headers). Same API and
 * include guard as the original; the FPR kernel becomes C. */

#ifndef MATHUTIL_VEC_DOT_PROD_H
#define MATHUTIL_VEC_DOT_PROD_H

#include "types.h"
#include "game/vec3.h"

/* a . b accumulated as fmuls; fmadds; fmadds */
static inline f32 mathutil_vec_dot_prod(Vec3 *a, Vec3 *b) {
    return fzgx_fmaf(a->z, b->z, fzgx_fmaf(a->y, b->y, a->x * b->x));
}

#endif  // MATHUTIL_VEC_DOT_PROD_H
