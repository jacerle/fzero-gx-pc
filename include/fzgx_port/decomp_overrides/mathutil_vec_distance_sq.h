/* fzero-gx-pc native replacement for external/fzero-gx-decomp/include/mathutil_vec_distance_sq.h
 * (R4 platform boundary: the port supplies the hardware/asm kernel headers). Same API and
 * include guard as the original; the FPR kernel becomes C. */

#ifndef MATHUTIL_VEC_DISTANCE_SQ_H
#define MATHUTIL_VEC_DISTANCE_SQ_H

#include "types.h"
#include "game/vec3.h"
#include "mathutil.h"

/* (a - b) . (a - b) */
static inline f32 mathutil_vec_distance_sq(Vec3 *a, Vec3 *b) {
    return mathutil_sum_of_sq_3(a->x - b->x, a->y - b->y, a->z - b->z);
}

#endif  // MATHUTIL_VEC_DISTANCE_SQ_H
