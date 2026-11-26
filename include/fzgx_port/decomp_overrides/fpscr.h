/* fzero-gx-pc native replacement for external/fzero-gx-decomp/include/fpscr.h
 * (R4 platform boundary: the port supplies the hardware/asm kernel headers). Same API and
 * include guard as the original; fctiw under round-toward-minus-infinity becomes C. */

#ifndef FPSCR_H
#define FPSCR_H

#include "types.h"

/* fctiw: saturating conversion of an already-rounded value; NaN gives 0x80000000. */
static inline s32 fzgx_fctiw_saturate(f64 rounded) {
    if (rounded != rounded || rounded < -2147483648.0) {
        return (s32)0x80000000u;
    }
    if (rounded > 2147483647.0) {
        return 0x7FFFFFFF;
    }
    return (s32)rounded;
}

/* Floor to integer (fctiw with RN = round toward minus infinity). */
static inline s32 fpscr_floor(f32 x) {
    return fzgx_fctiw_saturate(fzgx_floor((f64)x));
}

#endif
