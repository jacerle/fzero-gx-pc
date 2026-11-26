#ifndef FZGX_PORT_PPC_STATE_H
#define FZGX_PORT_PPC_STATE_H

/*
 * Native model of the Gekko state the decomp's inline-asm kernels touch (include/fzgx_port/
 * decomp_overrides/): the locked cache, the graphics quantization registers (GQRs) and
 * paired-single quantized loads/stores. Defined in src/decomp_port/ppc_state.c.
 */

#include "fzgx_port/mwcc_shim.h"


/* The 16 KiB locked-cache scratchpad, mapped at 0xE0000000 on the console (LC_BASE). The
 * engine keeps its current matrix "mtxA" at its start. */
#define FZGX_LOCKED_CACHE_SIZE 0x4000u
extern u8 fzgx_locked_cache[FZGX_LOCKED_CACHE_SIZE];
/* mtxA: the 3x4 row-major f32 matrix at the start of the locked cache (translation in
 * column 3, elements 3, 7, 11). */
#define FZGX_MTXA ((f32 *)fzgx_locked_cache)

/* GQR0..7. Retail OS init clears them (float, no scaling); game code reprograms GQR2..7. */
extern u32 fzgx_gqr[8];

/* One paired-single register: two f32 lanes. */
typedef struct FzgxPs {
    f32 ps0;
    f32 ps1;
} FzgxPs;

/* GQR quantization types: 0 f32, 4 u8, 5 u16, 6 s8, 7 s16 (1-3 reserved, treated as f32). */
static inline u32 fzgx_ps_elem_size(u32 type) {
    switch (type) {
    case 4:
    case 6:
        return 1;
    case 5:
    case 7:
        return 2;
    default:
        return 4;
    }
}

/* 6-bit two's-complement GQR scale field. */
static inline int fzgx_ps_scale(u32 field) {
    return (field & 0x20u) ? (int)field - 64 : (int)field;
}

/* Quantized elements are read and written in host byte order: the kernels that use non-float
 * GQR types work on buffers produced natively (image decoding), not on raw disc data. */
static inline f32 fzgx_ps_load_elem(const u8 *p, u32 type, int scale) {
    switch (type) {
    case 4:
        return fzgx_ldexpf((f32)*p, -scale);
    case 6:
        return fzgx_ldexpf((f32)(s8)*p, -scale);
    case 5: {
        u16 v;
        fzgx_copy_bytes(&v, p, sizeof(v));
        return fzgx_ldexpf((f32)v, -scale);
    }
    case 7: {
        s16 v;
        fzgx_copy_bytes(&v, p, sizeof(v));
        return fzgx_ldexpf((f32)v, -scale);
    }
    default: {
        f32 v;
        fzgx_copy_bytes(&v, p, sizeof(v));
        return v;
    }
    }
}

/* Gekko converts toward zero and saturates to the integer type's range. */
static inline void fzgx_ps_store_elem(u8 *p, f32 value, u32 type, int scale) {
    if (type == 4 || type == 5 || type == 6 || type == 7) {
        static const f32 kMin[4] = {0.0f, 0.0f, -128.0f, -32768.0f};
        static const f32 kMax[4] = {255.0f, 65535.0f, 127.0f, 32767.0f};
        f32 v = fzgx_truncf(fzgx_ldexpf(value, scale));
        if (!(v >= kMin[type - 4])) {
            v = kMin[type - 4]; /* also NaN */
        } else if (v > kMax[type - 4]) {
            v = kMax[type - 4];
        }
        if (type == 4 || type == 6) {
            *p = (u8)(s32)v;
        } else {
            const u16 h = (u16)(s32)v;
            fzgx_copy_bytes(p, &h, sizeof(h));
        }
    } else {
        fzgx_copy_bytes(p, &value, sizeof(value));
    }
}

/* psq_l / psq_st (W = 0: both lanes) through GQR `qr`. Returns the pair's byte size, the
 * stride an update-form (psq_lu / psq_stu) kernel advances by. */
static inline FzgxPs fzgx_psq_l(const void *addr, u32 qr) {
    const u32 type = (fzgx_gqr[qr] >> 16) & 7u;
    const int scale = fzgx_ps_scale((fzgx_gqr[qr] >> 24) & 0x3Fu);
    const u8 *p = (const u8 *)addr;
    FzgxPs r;
    r.ps0 = fzgx_ps_load_elem(p, type, scale);
    r.ps1 = fzgx_ps_load_elem(p + fzgx_ps_elem_size(type), type, scale);
    return r;
}

static inline void fzgx_psq_st(void *addr, FzgxPs v, u32 qr) {
    const u32 type = fzgx_gqr[qr] & 7u;
    const int scale = fzgx_ps_scale((fzgx_gqr[qr] >> 8) & 0x3Fu);
    u8 *p = (u8 *)addr;
    fzgx_ps_store_elem(p, v.ps0, type, scale);
    fzgx_ps_store_elem(p + fzgx_ps_elem_size(type), v.ps1, type, scale);
}

/* ps_madd d = a * c + b, lane-wise, with scalar (both-lane) c and b as the kernels use them. */
static inline FzgxPs fzgx_ps_madd_ss(FzgxPs a, f32 c, f32 b) {
    FzgxPs r;
    r.ps0 = fzgx_fmaf(a.ps0, c, b);
    r.ps1 = fzgx_fmaf(a.ps1, c, b);
    return r;
}

#endif /* FZGX_PORT_PPC_STATE_H */
