#ifndef FZGX_BOOT_H
#define FZGX_BOOT_H

/*
 * Native counterpart of the main.dol boot path (external/fzero-gx-decomp src/dol/av/game_main.c).
 *
 * Retail: main() -> fn_80005660 initialises the SDK (OSInit, DVDInit, VIInit, PADInit +
 * PADSetAnalogMode(3)) together with GX/mathutil/AV system setup, then fn_8000591C links
 * fze.sample.rel, whose _prolog decrypts and links the main game module (main_rel) and returns
 * its entry; main_rel's _prolog is the game's main loop and never returns.
 *
 * Natively the sample.rel loader stage has no purpose (main_rel is compiled into the
 * executable), so the boot initialises the SDK and calls main_rel's entry directly. The
 * entry blocks in VIWaitForRetrace once per game frame; the host runs its frame from the
 * retrace hook (fzgx::set_vi_retrace_hook), so game and host share one thread.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*FzgxGameEntry)(void);

typedef enum FzgxBootResult {
    FZGX_BOOT_ENTRY_RETURNED = 0, /* the entry ran and returned */
    FZGX_BOOT_NO_DISC = 1,        /* no disc image mounted */
    FZGX_BOOT_NO_ENTRY = 2        /* SDK initialised, but there is no entry to run */
} FzgxBootResult;

/* main_rel's native entry (its _prolog). NULL while main_rel does not compile natively. */
extern const FzgxGameEntry fzgx_main_module_entry;

/* Cold-boots the native SDK in retail order and runs `entry`. Re-bootable: every call starts
 * from a reset arena and an empty completion queue. */
FzgxBootResult fzgx_boot_main(FzgxGameEntry entry);

#ifdef __cplusplus
}
#endif

#endif /* FZGX_BOOT_H */
