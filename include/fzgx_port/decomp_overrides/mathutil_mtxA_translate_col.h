/* fzero-gx-pc native replacement for external/fzero-gx-decomp/include/mathutil_mtxA_translate_col.h
 * (R4 platform boundary: the port supplies the hardware/asm kernel headers). Same API and
 * include guard as the original; the locked-cache matrix is fzgx_locked_cache. */

#ifndef MATHUTIL_MTXA_TRANSLATE_COL_H
#define MATHUTIL_MTXA_TRANSLATE_COL_H

#include "types.h"
#include "game/vec3.h"
#include "fzgx_port/ppc_state.h"

/* mtxA translation += column `col` of mtxA * d (fused per row). */
static inline void fzgx_mtxA_translate_col(int col, f32 d) {
    f32 *m = FZGX_MTXA;
    m[3] = fzgx_fmaf(m[col], d, m[3]);
    m[7] = fzgx_fmaf(m[4 + col], d, m[7]);
    m[11] = fzgx_fmaf(m[8 + col], d, m[11]);
}

static inline void mathutil_mtxA_translate_z(f32 d) {
    fzgx_mtxA_translate_col(2, d);
}

static inline void mathutil_mtxA_translate_x(f32 d) {
    fzgx_mtxA_translate_col(0, d);
}

#endif  // MATHUTIL_MTXA_TRANSLATE_COL_H
