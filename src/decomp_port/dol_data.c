/*
 * main.dol data referenced by the natively compiled decomp TUs (CMake: FZGX_DECOMP_DOL_SOURCES).
 *
 * Upstream still defines these in generated data-import blobs (src/dol/data/) that bundle
 * unrelated objects from many TUs, use MWCC-only __declspec(section) syntax and drag in
 * undefined references. Until upstream R2 moves each object into its owning TU, the port
 * defines the few it links here, with the retail values from those data-import files.
 */

/* arc.c */
char lbl_80132788[34] = "ARCInitHandle: bad archive format";
char lbl_801327AC[70] = "Warning: ARCOpen(): file '%s' was not found under %s in the archive.\n";
char lbl_801A6610[6] = "arc.c";

/* ClampStick.c: PADClampRegion {minTrigger 30, maxTrigger 180, minStick 15, maxStick 72,
 * xyStick 40, minSubstick 15, maxSubstick 59, xySubstick 31, radStick 56, radSubstick 44}. */
const u8 lbl_8008FF40[16] = {0x1E, 0xB4, 0x0F, 0x48, 0x28, 0x0F, 0x3B, 0x1F, 0x38, 0x2C};

/* fn_8007ED90.c (MSL tolower): the "C" locale lower-case map, identity except 'A'..'Z'. */
#define FZGX_TOLOWER(c) ((c) >= 'A' && (c) <= 'Z' ? (c) + ('a' - 'A') : (c))
#define FZGX_TOLOWER_ROW(b)                                                                            \
    FZGX_TOLOWER((b) + 0x0), FZGX_TOLOWER((b) + 0x1), FZGX_TOLOWER((b) + 0x2), FZGX_TOLOWER((b) + 0x3), \
    FZGX_TOLOWER((b) + 0x4), FZGX_TOLOWER((b) + 0x5), FZGX_TOLOWER((b) + 0x6), FZGX_TOLOWER((b) + 0x7), \
    FZGX_TOLOWER((b) + 0x8), FZGX_TOLOWER((b) + 0x9), FZGX_TOLOWER((b) + 0xA), FZGX_TOLOWER((b) + 0xB), \
    FZGX_TOLOWER((b) + 0xC), FZGX_TOLOWER((b) + 0xD), FZGX_TOLOWER((b) + 0xE), FZGX_TOLOWER((b) + 0xF)
u8 lbl_8015B200[256] = {
    FZGX_TOLOWER_ROW(0x00), FZGX_TOLOWER_ROW(0x10), FZGX_TOLOWER_ROW(0x20), FZGX_TOLOWER_ROW(0x30),
    FZGX_TOLOWER_ROW(0x40), FZGX_TOLOWER_ROW(0x50), FZGX_TOLOWER_ROW(0x60), FZGX_TOLOWER_ROW(0x70),
    FZGX_TOLOWER_ROW(0x80), FZGX_TOLOWER_ROW(0x90), FZGX_TOLOWER_ROW(0xA0), FZGX_TOLOWER_ROW(0xB0),
    FZGX_TOLOWER_ROW(0xC0), FZGX_TOLOWER_ROW(0xD0), FZGX_TOLOWER_ROW(0xE0), FZGX_TOLOWER_ROW(0xF0),
};
