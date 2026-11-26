/* fzero-gx-pc native replacement for external/fzero-gx-decomp/include/psvec.h
 * (R4 platform boundary: the port supplies the hardware/asm kernel headers). Same API and
 * include guard as the original; paired-single asm kernels become C. */

#ifndef PSVEC_H
#define PSVEC_H

#include "types.h"

/* Vectors are three consecutive f32 (x, y, z); pointers are untyped as in the original. */

/* ab = a - b */
static inline void psvec_sub(const void *a, const void *b, void *ab) {
    const f32 *pa = (const f32 *)a;
    const f32 *pb = (const f32 *)b;
    f32 *out = (f32 *)ab;
    const f32 x = pa[0] - pb[0], y = pa[1] - pb[1], z = pa[2] - pb[2];
    out[0] = x;
    out[1] = y;
    out[2] = z;
}

/* ab = a + b */
static inline void psvec_add(const void *a, const void *b, void *ab) {
    const f32 *pa = (const f32 *)a;
    const f32 *pb = (const f32 *)b;
    f32 *out = (f32 *)ab;
    const f32 x = pa[0] + pb[0], y = pa[1] + pb[1], z = pa[2] + pb[2];
    out[0] = x;
    out[1] = y;
    out[2] = z;
}

/* dst = a * s */
static inline void psvec_scale(const void *a, f32 s, void *dst) {
    const f32 *pa = (const f32 *)a;
    f32 *out = (f32 *)dst;
    const f32 x = pa[0] * s, y = pa[1] * s, z = pa[2] * s;
    out[0] = x;
    out[1] = y;
    out[2] = z;
}

/* dst = acc + v * s (fused, as ps_madds0 / fmadds) */
static inline void psvec_scale_add(const void *acc, const void *v, f32 s, void *dst) {
    const f32 *pa = (const f32 *)acc;
    const f32 *pv = (const f32 *)v;
    f32 *out = (f32 *)dst;
    const f32 x = fzgx_fmaf(pv[0], s, pa[0]), y = fzgx_fmaf(pv[1], s, pa[1]), z = fzgx_fmaf(pv[2], s, pa[2]);
    out[0] = x;
    out[1] = y;
    out[2] = z;
}

/* d = a - b; out = b + d * s */
static inline void psvec_sub_scale_add(const void *a, const void *b, f32 s, void *d, void *out) {
    const f32 *pa = (const f32 *)a;
    const f32 *pb = (const f32 *)b;
    f32 *pd = (f32 *)d;
    f32 *po = (f32 *)out;
    const f32 bx = pb[0], by = pb[1], bz = pb[2];
    const f32 dx = pa[0] - bx, dy = pa[1] - by, dz = pa[2] - bz;
    pd[0] = dx;
    pd[1] = dy;
    pd[2] = dz;
    po[0] = fzgx_fmaf(dx, s, bx);
    po[1] = fzgx_fmaf(dy, s, by);
    po[2] = fzgx_fmaf(dz, s, bz);
}

/* dst = (x, y, z); parameters in the original's z, y, x order. */
static inline void psvec_set(void *dst, f32 z, f32 y, f32 x) {
    f32 *out = (f32 *)dst;
    out[0] = x;
    out[1] = y;
    out[2] = z;
}

#endif
