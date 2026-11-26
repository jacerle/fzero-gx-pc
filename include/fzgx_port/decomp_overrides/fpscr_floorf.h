/* fzero-gx-pc native replacement for external/fzero-gx-decomp/include/fpscr_floorf.h
 * (R4 platform boundary: the port supplies the hardware/asm kernel headers). Same API and
 * include guard as the original; see fpscr.h. */

#ifndef FPSCR_FLOORF_H
#define FPSCR_FLOORF_H

#include "types.h"
#include "fpscr.h"

/* fpscr_floor returning the integer as f32. */
static inline f32 fpscr_floorf(f32 x) {
    return (f32)fpscr_floor(x);
}

#endif
