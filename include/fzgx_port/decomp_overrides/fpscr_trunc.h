/* fzero-gx-pc native replacement for external/fzero-gx-decomp/include/fpscr_trunc.h
 * (R4 platform boundary: the port supplies the hardware/asm kernel headers). Same API and
 * include guard as the original; fctiwz becomes C. */

#ifndef FPSCR_TRUNC_H
#define FPSCR_TRUNC_H

#include "types.h"
#include "fpscr.h"

/* Truncate to integer (fctiwz), saturating. */
static inline s32 fpscr_trunc(f32 x) {
    return fzgx_fctiw_saturate(fzgx_trunc((f64)x));
}

#endif
