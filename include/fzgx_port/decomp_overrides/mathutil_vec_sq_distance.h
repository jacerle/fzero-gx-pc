/* fzero-gx-pc native replacement for external/fzero-gx-decomp/include/mathutil_vec_sq_distance.h
 * (R4 platform boundary: the port supplies the hardware/asm kernel headers). Same API and
 * include guard as the original; the FPR kernel becomes C. */

#ifndef MATHUTIL_VEC_SQ_DISTANCE_H
#define MATHUTIL_VEC_SQ_DISTANCE_H

#include "types.h"
#include "game/vec3.h"

/* (a - b) . (a - b), accumulated into the x difference */
static inline f32 mathutil_vec_sq_distance(Vec3 *a, Vec3 *b) {
    const f32 x = a->x - b->x, y = a->y - b->y, z = a->z - b->z;
    return fzgx_fmaf(z, z, fzgx_fmaf(y, y, x * x));
}

#endif  // MATHUTIL_VEC_SQ_DISTANCE_H
