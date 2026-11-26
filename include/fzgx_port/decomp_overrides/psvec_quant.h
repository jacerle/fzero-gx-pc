/* fzero-gx-pc native replacement for external/fzero-gx-decomp/include/psvec_quant.h
 * (R4 platform boundary: the port supplies the hardware/asm kernel headers). Same API and
 * include guard as the original; quantized paired-single kernels run on the
 * emulated GQRs of fzgx_port/ppc_state.h. Update-form macros advance the caller's pointers by
 * the pair size of the GQR type, as psq_lu/psq_stu do with their displacement. */

#ifndef PSVEC_QUANT_H
#define PSVEC_QUANT_H

#include "types.h"
#include "fzgx_port/ppc_state.h"

/* Four qr3 pairs from src+0,4,8,12 to dst+0,2,4,6. */
static inline void psq3_row(const void *src, void *dst) {
    const u8 *s = (const u8 *)src;
    u8 *d = (u8 *)dst;
    const FzgxPs a = fzgx_psq_l(s + 0, 3), b = fzgx_psq_l(s + 4, 3);
    const FzgxPs c = fzgx_psq_l(s + 8, 3), e = fzgx_psq_l(s + 12, 3);
    fzgx_psq_st(d + 0, a, 3);
    fzgx_psq_st(d + 2, b, 3);
    fzgx_psq_st(d + 4, c, 3);
    fzgx_psq_st(d + 6, e, 3);
}

/* psq_lu / psq_stu with displacement 2 through `qr`: pre-increment, then access. */
#define FZGX_PSQ_LU2(p, qr) ((p) = (void *)((u8 *)(p) + 2), fzgx_psq_l((p), (qr)))
#define FZGX_PSQ_STU2(p, v, qr) ((p) = (void *)((u8 *)(p) + 2), fzgx_psq_st((p), (v), (qr)))

/* Two pairs from a, one each from b and c, scaled (x * k + k0), merged, four pairs stored (qr2). */
#define psq2_merge4(d, a, b, c, k0, k1, k2, k3)                       \
    do {                                                              \
        FzgxPs f4_ = FZGX_PSQ_LU2(a, 2);                              \
        FzgxPs f6_ = FZGX_PSQ_LU2(b, 2);                              \
        FzgxPs f5_, f7_, f8_, o_;                                     \
        f4_ = fzgx_ps_madd_ss(f4_, (k1), (k0));                       \
        f6_ = fzgx_ps_madd_ss(f6_, (k2), (k0));                       \
        f5_ = FZGX_PSQ_LU2(a, 2);                                     \
        f7_ = FZGX_PSQ_LU2(c, 2);                                     \
        f5_ = fzgx_ps_madd_ss(f5_, (k1), (k0));                       \
        f7_ = fzgx_ps_madd_ss(f7_, (k3), (k0));                       \
        f8_.ps0 = f4_.ps0;                                            \
        f8_.ps1 = f6_.ps0;                                            \
        o_.ps0 = f4_.ps1;                                             \
        o_.ps1 = f7_.ps0;                                             \
        FZGX_PSQ_STU2(d, f8_, 2);                                     \
        FZGX_PSQ_STU2(d, o_, 2);                                      \
        o_.ps0 = f5_.ps0;                                             \
        o_.ps1 = f6_.ps1;                                             \
        FZGX_PSQ_STU2(d, o_, 2);                                      \
        o_.ps0 = f5_.ps1;                                             \
        o_.ps1 = f7_.ps1;                                             \
        FZGX_PSQ_STU2(d, o_, 2);                                      \
    } while (0)

/* The scale pairs psq27_merge2 reads (f4 = (k1, k2), f5 = (k1, k3)), set before the loop. */
static inline FzgxPs *fzgx_psq27_scales(void) {
    static FzgxPs scales[2];
    return scales;
}
#define psq27_merge2_setup(k1, k2, k3)      \
    do {                                    \
        FzgxPs *s_ = fzgx_psq27_scales();   \
        s_[0].ps0 = (k1);                   \
        s_[0].ps1 = (k2);                   \
        s_[1].ps0 = (k1);                   \
        s_[1].ps1 = (k3);                   \
    } while (0)

/* One pair from a (qr2), one each from b and c (qr7), summed, scaled by the setup pairs plus
 * k0, two pairs stored (qr2). t1..t3 are the original's register temporaries, unused here. */
#define psq27_merge2(d, a, b, c, k0, t1, t2, t3)                                  \
    do {                                                                          \
        FzgxPs p1_ = FZGX_PSQ_LU2(a, 2);                                          \
        FzgxPs p3_ = FZGX_PSQ_LU2(c, 7);                                          \
        FzgxPs p2_ = FZGX_PSQ_LU2(b, 7);                                          \
        const FzgxPs *k_ = fzgx_psq27_scales();                                   \
        FzgxPs s3_, s1_, o1_, o3_;                                                \
        (void)(t1);                                                               \
        (void)(t2);                                                               \
        (void)(t3);                                                               \
        s3_.ps0 = p3_.ps0 + p3_.ps1; /* ps_sum0 t3, t3, t1, t3 */                 \
        s3_.ps1 = p1_.ps1;                                                        \
        s1_.ps0 = p1_.ps0;           /* ps_sum1 t1, t2, t1, t2 */                 \
        s1_.ps1 = p2_.ps0 + p2_.ps1;                                              \
        o3_.ps0 = s3_.ps1;           /* ps_merge10 t3, t3, t3 */                  \
        o3_.ps1 = s3_.ps0;                                                        \
        o1_.ps0 = fzgx_fmaf(s1_.ps0, k_[0].ps0, (k0));                            \
        o1_.ps1 = fzgx_fmaf(s1_.ps1, k_[0].ps1, (k0));                            \
        o3_.ps0 = fzgx_fmaf(o3_.ps0, k_[1].ps0, (k0));                            \
        o3_.ps1 = fzgx_fmaf(o3_.ps1, k_[1].ps1, (k0));                            \
        FZGX_PSQ_STU2(d, o1_, 2);                                                 \
        FZGX_PSQ_STU2(d, o3_, 2);                                                 \
    } while (0)

#endif  // PSVEC_QUANT_H
