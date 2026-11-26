/* fzero-gx-pc native replacement for external/fzero-gx-decomp/include/mathutil_vec_cross_prod.h
 * (R4 platform boundary: the port supplies the hardware/asm kernel headers). Same API and
 * include guard as the original; the FPR kernel becomes C. */

#ifndef MATHUTIL_VEC_CROSS_PROD_H
#define MATHUTIL_VEC_CROSS_PROD_H

#include "types.h"
#include "game/vec3.h"

/* out = a x b, each component as fmuls then fnmsubs (out may alias a or b). */
static inline void mathutil_vec_cross_prod(Vec3 *a, Vec3 *b, Vec3 *out) {
    const f32 ax = a->x, ay = a->y, az = a->z;
    const f32 bx = b->x, by = b->y, bz = b->z;
    const f32 x = fzgx_fmaf(-az, by, ay * bz);
    const f32 y = fzgx_fmaf(-ax, bz, az * bx);
    const f32 z = fzgx_fmaf(-ay, bx, ax * by);
    out->x = x;
    out->y = y;
    out->z = z;
}

#endif  // MATHUTIL_VEC_CROSS_PROD_H
