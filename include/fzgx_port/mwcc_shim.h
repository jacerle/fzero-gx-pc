#ifndef FZGX_PORT_MWCC_SHIM_H
#define FZGX_PORT_MWCC_SHIM_H

/*
 * Portable 64-bit C11/C++23 compatibility shim for Metrowerks CodeWarrior (MWCC)
 * constructs still present in external/fzero-gx-decomp during upstream R1-R7 cleanup.
 *
 * Key difference from decomp's original types.h:
 *   On 64-bit LP64 Linux, `long` is 64-bit, whereas GameCube expects 32-bit s32/u32.
 *   Compiling decomp TUs with this header pre-included keeps s32/u32 exact 32-bit widths.
 *
 * Force-included into every decomp TU, so it pulls in only the freestanding <stddef.h> and
 * <stdint.h>: decomp TUs declare the MSL library functions they use themselves (memset with a
 * u32 size, strcmp on void pointers, ...), which would conflict with a hosted libc's headers.
 * Math used here goes through compiler builtins on GCC/Clang for the same reason.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus) || !(defined(__GNUC__) || defined(__clang__))
#include <math.h>
#define FZGX_LIBM(fn) fn
#else
#define FZGX_LIBM(fn) __builtin_##fn
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef TYPES_H
#define TYPES_H
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef float f32;
typedef double f64;
typedef int32_t BOOL;
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#endif /* TYPES_H */

/* MWCC storage-class / pragma / qualifier compatibility */
#ifndef __MWERKS__
#ifndef register
#define register
#endif
/* __declspec(section ".data") etc. place objects for the retail link; the host ignores them.
 * MSVC has its own __declspec, so it keeps the keyword (decomp data does not build there yet). */
#if !defined(__cplusplus) && !defined(_MSC_VER)
#define __declspec(x)
#endif
#endif

/* Host libm through FZGX_LIBM (see the top of this file). */
static inline float fzgx_fmaf(float a, float b, float c) {
    return FZGX_LIBM(fmaf)(a, b, c);
}
static inline double fzgx_floor(double x) {
    return FZGX_LIBM(floor)(x);
}
static inline double fzgx_trunc(double x) {
    return FZGX_LIBM(trunc)(x);
}
static inline float fzgx_truncf(float x) {
    return FZGX_LIBM(truncf)(x);
}
static inline float fzgx_ldexpf(float x, int e) {
    return FZGX_LIBM(ldexpf)(x, e);
}
static inline double fzgx_sqrt(double x) {
    return FZGX_LIBM(sqrt)(x);
}

/* Byte copy for the kernels' unaligned/aliasing loads, without <string.h>. */
static inline void fzgx_copy_bytes(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n-- != 0) {
        *d++ = *s++;
    }
}

/* Endian helpers (host is little-endian on x86_64; disc/card/GX data is big-endian) */
static inline uint16_t fzgx_bswap16(uint16_t v) {
#if defined(_MSC_VER)
    return _byteswap_ushort(v);
#else
    return __builtin_bswap16(v);
#endif
}

static inline uint32_t fzgx_bswap32(uint32_t v) {
#if defined(_MSC_VER)
    return _byteswap_ulong(v);
#else
    return __builtin_bswap32(v);
#endif
}

static inline uint64_t fzgx_bswap64(uint64_t v) {
#if defined(_MSC_VER)
    return _byteswap_uint64(v);
#else
    return __builtin_bswap64(v);
#endif
}

static inline uint16_t fzgx_be16(const void* p) {
    const uint8_t* b = (const uint8_t*)p;
    return (uint16_t)(((uint16_t)b[0] << 8) | (uint16_t)b[1]);
}

static inline uint32_t fzgx_be32(const void* p) {
    const uint8_t* b = (const uint8_t*)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

/*
 * MWCC / Gekko intrinsics used by decomp C (inventory 2026-11-26: __fabs, __rlwnm, __fmadds,
 * __abs, __rlwinm, __cntlzw, __lwbrx, __fabsf, __frsqrte, __sync, __dcbz, __rlwimi), with the
 * instruction's semantics and MWCC's prototypes, so a TU's own `extern int __cntlzw(unsigned int);`
 * still compiles (an extern redeclaration of a static function keeps internal linkage).
 * The MWCC names are object-like macros for C only: glibc's math.h already declares __fabs,
 * and C++ standard headers must not see these names.
 */
static inline uint32_t fzgx_rotl32(uint32_t val, uint32_t shift) {
    shift &= 31u;
    return (shift == 0u) ? val : ((val << shift) | (val >> (32u - shift)));
}

/* MASK(mb, me) of rlwinm/rlwnm/rlwimi: bits mb..me set (IBM numbering), wrapping if mb > me. */
static inline uint32_t fzgx_ppc_mask(uint32_t mb, uint32_t me) {
    const uint32_t from_mb = 0xFFFFFFFFu >> (mb & 31u);
    const uint32_t to_me = 0xFFFFFFFFu << (31u - (me & 31u));
    return ((mb & 31u) <= (me & 31u)) ? (from_mb & to_me) : (from_mb | to_me);
}

static inline int fzgx_rlwinm(int rs, int sh, int mb, int me) {
    return (int)(fzgx_rotl32((uint32_t)rs, (uint32_t)sh) & fzgx_ppc_mask((uint32_t)mb, (uint32_t)me));
}

/* rlwnm is rlwinm with the rotate amount from a register (low 5 bits). */
static inline int fzgx_rlwnm(int rs, int rb, int mb, int me) {
    return fzgx_rlwinm(rs, rb & 31, mb, me);
}

static inline int fzgx_rlwimi(int ra, int rs, int sh, int mb, int me) {
    const uint32_t mask = fzgx_ppc_mask((uint32_t)mb, (uint32_t)me);
    return (int)((fzgx_rotl32((uint32_t)rs, (uint32_t)sh) & mask) | ((uint32_t)ra & ~mask));
}

static inline int fzgx_cntlzw(unsigned int x) {
    int n = 0;
    if (x == 0u) {
        return 32;
    }
    while ((x & 0x80000000u) == 0u) {
        x <<= 1;
        n++;
    }
    return n;
}

/* lwbrx: the word at base + offset read as little-endian bytes (byte semantics, host independent). */
static inline unsigned int fzgx_lwbrx(void* base, int offset) {
    const uint8_t* b = (const uint8_t*)base + offset;
    return (unsigned int)b[0] | ((unsigned int)b[1] << 8) | ((unsigned int)b[2] << 16) |
           ((unsigned int)b[3] << 24);
}

static inline double fzgx_fabs(double x) {
    return FZGX_LIBM(fabs)(x);
}

static inline float fzgx_fabsf(float x) {
    return FZGX_LIBM(fabsf)(x);
}

static inline int fzgx_abs(int x) {
    return x < 0 ? -x : x;
}

/* fmadds: a * b + c, fused, rounded to single. */
static inline float fzgx_fmadds(float a, float b, float c) {
    return fzgx_fmaf(a, b, c);
}

/* frsqrte: reciprocal square root estimate (exact here; Gekko's estimate has 1/4096 precision). */
static inline double fzgx_frsqrte(double x) {
    return 1.0 / fzgx_sqrt(x);
}

/* dcbz: zeroes the 32-byte cache line containing base + offset. */
static inline void fzgx_dcbz(void* base, int offset) {
    uint8_t* line = (uint8_t*)((uintptr_t)((uint8_t*)base + offset) & ~(uintptr_t)31u);
    for (int i = 0; i < 32; i++) {
        line[i] = 0;
    }
}

/* sync orders memory against other bus masters (GX, DSP, DMA); the host has none. */
static inline void fzgx_sync(void) {
}

#ifndef __cplusplus
#define __fabs fzgx_fabs
#define __fabsf fzgx_fabsf
#define __abs fzgx_abs
#define __fmadds fzgx_fmadds
#define __frsqrte fzgx_frsqrte
#define __cntlzw fzgx_cntlzw
#define __rlwinm fzgx_rlwinm
#define __rlwnm fzgx_rlwnm
#define __rlwimi fzgx_rlwimi
#define __lwbrx fzgx_lwbrx
#define __dcbz fzgx_dcbz
#define __sync fzgx_sync
#endif

/* GX FIFO write routing hooks (wired to fzgx_gx_fifo_* in platform_api.h) */
void fzgx_gx_fifo_u8(uint8_t v);
void fzgx_gx_fifo_u16(uint16_t v);
void fzgx_gx_fifo_u32(uint32_t v);
void fzgx_gx_fifo_f32(float v);

#ifdef __cplusplus
}
#endif

#endif /* FZGX_PORT_MWCC_SHIM_H */
