/*
 * The port's decomp environment: MWCC intrinsics (mwcc_shim.h) and the native replacements of
 * the decomp's asm/hardware headers (include/fzgx_port/decomp_overrides/), included here by
 * their decomp names exactly as game TUs include them.
 */

#include "dolphin/hw_regs.h"
#include "dolphin/os/OSFastCast.h"
#include "fpscr.h"
#include "fpscr_floorf.h"
#include "fpscr_trunc.h"
#include "fzgx_port/dolphin_sdk.h"
#include "game/vec3.h"
#include "gqr.h"
#include "mathutil_mtxA_translate_col.h"
#include "mathutil_vec_cross_prod.h"
#include "mathutil_vec_distance_sq.h"
#include "psvec.h"
#include "psvec_quant.h"

#include <assert.h>
#include <string.h>

void fzgx_test_decomp_layer(void);

static void test_intrinsics(void) {
    static const u8 le_word[4] = {0x11, 0x22, 0x33, 0x44};
    _Alignas(32) u8 lines[64];

    assert(__rlwinm(0x80000001u, 1, 30, 31) == 3);
    assert((u32)__rlwinm(0xFFFFFFFFu, 0, 30, 1) == 0xC0000003u); /* mb > me wraps the mask */
    assert((u32)__rlwnm(0x00000001u, 33, 0, 31) == 2u);           /* rotate uses the low 5 bits */
    assert((u32)__rlwimi(0xFFFF00FFu, 0x12u, 8, 16, 23) == 0xFFFF12FFu);
    assert(__cntlzw(0) == 32 && __cntlzw(1) == 31 && __cntlzw(0x80000000u) == 0);
    assert(__lwbrx((void *)le_word, 0) == 0x44332211u);
    assert(__fmadds(2.0f, 3.0f, 1.0f) == 7.0f);
    assert(__abs(-5) == 5 && __fabs(-2.5) == 2.5);

    memset(lines, 0xFF, sizeof(lines));
    __dcbz(lines, 40); /* zeroes the whole line 32..63, nothing before it */
    assert(lines[31] == 0xFF && lines[32] == 0 && lines[63] == 0);
}

static void test_fp_conversions(void) {
    assert(fpscr_floor(-1.5f) == -2 && fpscr_floor(2.75f) == 2);
    assert(fpscr_trunc(-1.5f) == -1 && fpscr_trunc(2.75f) == 2);
    assert(fpscr_floorf(-0.25f) == -1.0f);
    assert(fpscr_floor(3.0e10f) == 0x7FFFFFFF && fpscr_trunc(-3.0e10f) == (s32)0x80000000u);
    {
        volatile f32 zero = 0.0f;
        assert(fpscr_floor(zero / zero) == (s32)0x80000000u); /* NaN */
    }

    assert(__OSf32tou8(12.9f) == 12 && __OSf32tou8(300.0f) == 255 && __OSf32tou8(-5.0f) == 0);
    assert(__OSf32tos8(-12.9f) == -12 && __OSf32tos8(200.0f) == 127);
    assert(__OSf32tos16(-40000.0f) == -32768 && __OSf32tou16(70000.0f) == 65535);
}

static void test_vector_kernels(void) {
    Vec3 a = {1.0f, 2.0f, 3.0f};
    Vec3 b = {4.0f, 5.0f, 6.0f};
    Vec3 c;
    f32 *m = (f32 *)LC_BASE;

    psvec_sub(&b, &a, &c);
    assert(c.x == 3.0f && c.y == 3.0f && c.z == 3.0f);
    psvec_set(&c, 9.0f, 8.0f, 7.0f); /* parameters are z, y, x */
    assert(c.x == 7.0f && c.y == 8.0f && c.z == 9.0f);
    mathutil_vec_cross_prod(&a, &b, &a); /* output may alias an input */
    assert(a.x == -3.0f && a.y == 6.0f && a.z == -3.0f);
    assert(mathutil_vec_distance_sq(&b, &c) == 9.0f + 9.0f + 9.0f);

    /* mtxA lives at the start of the locked cache: translation += column 2 * d. */
    memset(m, 0, 12 * sizeof(f32));
    m[2] = 1.0f;
    m[6] = 2.0f;
    m[10] = 3.0f;
    m[3] = 10.0f;
    mathutil_mtxA_translate_z(0.5f);
    assert(m[3] == 10.5f && m[7] == 1.0f && m[11] == 1.5f);
}

static void test_quantized_kernels(void) {
    /* u8 load and store formats on GQR2: the merge kernel interleaves 2+2 bytes of a with
     * one pair each of b and c, all pre-incremented by 2. */
    u8 a_buf[8] = {0, 0, 10, 11, 12, 13, 0, 0};
    u8 b_buf[4] = {0, 0, 20, 21};
    u8 c_buf[4] = {0, 0, 30, 31};
    u8 d_buf[10] = {0};
    u8 *a = a_buf, *b = b_buf, *c = c_buf, *d = d_buf;
    const f32 k0 = 1.0f, k1 = 2.0f, k2 = 1.0f, k3 = 10.0f;

    gqr_put2(0x00040004u);
    assert(gqr_get2() == 0x00040004u);
    psq2_merge4(d, a, b, c, k0, k1, k2, k3);
    assert(a == a_buf + 4 && b == b_buf + 2 && c == c_buf + 2 && d == d_buf + 8);
    /* a * 2 + 1, b * 1 + 1, c * 10 + 1 saturated to u8 */
    assert(d_buf[2] == 21 && d_buf[3] == 21); /* a2, b2 */
    assert(d_buf[4] == 23 && d_buf[5] == 255); /* a3, c2 */
    assert(d_buf[6] == 25 && d_buf[7] == 22); /* a4, b3 */
    assert(d_buf[8] == 27 && d_buf[9] == 255); /* a5, c3 */

    OSInit(); /* retail OS init clears the GQRs */
    assert(gqr_get2() == 0);
}

void fzgx_test_decomp_layer(void) {
    test_intrinsics();
    test_fp_conversions();
    test_vector_kernels();
    test_quantized_kernels();
}
