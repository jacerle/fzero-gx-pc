/* fzero-gx-pc native replacement for external/fzero-gx-decomp/include/mathutil_mtxA_set_translate.h
 * (R4 platform boundary: the port supplies the hardware/asm kernel headers). Same API and
 * include guard as the original; the locked-cache matrix is fzgx_locked_cache. */

#ifndef MATHUTIL_MTXA_SET_TRANSLATE_H
#define MATHUTIL_MTXA_SET_TRANSLATE_H

#include "types.h"
#include "game/vec3.h"
#include "fzgx_port/ppc_state.h"

/* mtxA translation = v */
static inline void mathutil_mtxA_set_translate(Vec3 *v) {
    FZGX_MTXA[3] = v->x;
    FZGX_MTXA[7] = v->y;
    FZGX_MTXA[11] = v->z;
}

#endif  // MATHUTIL_MTXA_SET_TRANSLATE_H
