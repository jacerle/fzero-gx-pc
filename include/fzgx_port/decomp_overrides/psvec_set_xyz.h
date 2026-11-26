/* fzero-gx-pc native replacement for external/fzero-gx-decomp/include/psvec_set_xyz.h
 * (R4 platform boundary: the port supplies the hardware/asm kernel headers). Same API and
 * include guard as the original; the stfs device is its honest C form. */

#ifndef PSVEC_SET_XYZ_H
#define PSVEC_SET_XYZ_H

#include "types.h"

static inline void vec_set_xyz(void *dst, f32 x, f32 y, f32 z) {
    f32 *out = (f32 *)dst;
    out[0] = x;
    out[1] = y;
    out[2] = z;
}

#endif
