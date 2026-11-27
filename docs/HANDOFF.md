# F-Zero GX Native PC Port (`fzero-gx-pc`) — Session Handoff Document

**Date:** 2026-11-27  
**Workspace:** `/home/erlend/Projects/fzero-gx-pc`  
**Status:** Milestones 1, 2 & 3 complete (committed in `f8e62ed`), plus **Option B (native Dolphin SDK, boot path, decomp header layer)** committed in `ce23de3` and pushed to `origin/main`. Active focus is now shifting upstream to `karamzov123/fzero-gx-decomp` (R1–R7 recompilation readiness) via `/home/erlend/Projects/fzero-gx-decomp` before resuming downstream `main_rel` per-TU work.

---

## 1. Project Overview & Agreed Architecture

All core architectural decisions were resolved during the initial `/grill-me` design interview:

| Area | Locked-In Decision |
|---|---|
| **Repository Layout** | Separate downstream native C++23 / C11 PC port at `/home/erlend/Projects/fzero-gx-pc`, referencing the user's synced fork (`https://github.com/jacerle/fzero-gx-decomp.git`) as a git submodule at `external/fzero-gx-decomp`. |
| **Portability Bridge** | 64-bit native (`x86_64`) with a thin downstream compatibility shim (`include/fzgx_port/mwcc_shim.h` + `include/fzgx_port/platform_api.h`) enforcing 32-bit `u32`/`s32` widths and bridging MWCC intrinsics, paired-singles, GX FIFO macros, and endianness; shims shrink as upstream lands R1–R7. |
| **Graphics Backend** | Direct **Vulkan 1.3** (`volk` + `VK_KHR_dynamic_rendering`, header-only `Vulkan-Headers`) with a custom GX-to-Vulkan backend. |
| **Shaders & Post-FX** | Configurable internal resolution scaling (`1x`–`8x` of 480p), **480p CRT feed toggle**, **AMD FSR 1.0 (EASU + RCAS)** upscaling & sharpness, built-in Vulkan CRT fallback shader, and dynamic-loaded **`librashader` v0.12.0** (`.slangp` presets + live parameter sliders via `librashader_ld.h`, no Rust toolchain required to build). |
| **Widescreen & HUD** | Arbitrary Hor+ aspect ratios (`Auto`, `4:3`, `16:9`, `16:10`, `21:9`) with widened 3D frustum culling, clean pillarboxing for 2D/FMVs, and configurable race HUD anchoring (`Safe Centered` vs `Edge Anchored`). |
| **Framerate & VSync** | Fixed 60 Hz game physics + render-frame interpolation for >60 FPS (`60`, `120`, `144`, `240`, `Uncapped`) and Vulkan present modes (`Off`/Immediate, `FIFO`, `Mailbox`). |
| **Disc Intake & Region** | Runtime disc-image mounting (`.rvz` / `.wia` / `.iso` / `.gcm`) with GameCube magic (`0xC2339F3D`), GameID (`GFZE01`), logical SHA-1 verification, and optional mod override directory. USA `GFZE01` targeted first. Zero copyrighted assets in repo. |
| **Saves / Memory Card** | Dolphin-compatible `.gci` folder virtual Memory Cards (Slots A & B under XDG / AppData). |
| **Audio & FMV** | **SDL3 Audio** (`48 kHz` stereo float32) mixing built-in CRI ADX v3/v4 music + **FFmpeg** CRI Sofdec (`.sfd`) FMV video/audio rendered through the Vulkan + CRT pipeline. |
| **Launcher & Overlay** | Single unified executable using **Dear ImGui** (`v1.92.9`, SDL3 + Vulkan backends, `NavEnableKeyboard | NavEnableGamepad`), opening with a first-run ROM setup wizard when unconfigured and toggling the 6-tab settings overlay via `F1`, `Guide`, or `Back + Start`. |
| **Input Stack** | `IInputProvider` abstraction with 4 GameCube ports via **SDL3 Gamepad API + Keyboard** fallback (hot-plug, rumble test, radial stick deadzone/outer-gate, dual-stage analog + digital-click L/R trigger threshold calibration) plus a `GcUsbAdapterProvider` (`WUP-028`) stub. |
| **Platforms & CI** | Linux (GCC/Clang) & Windows (MSVC/Clang-CL) via CMake 3.25+ / Ninja (`CMakePresets.json` + `.github/workflows/ci.yml`). |

---

## 2. Current Repository & Git State

- **Local repo:** `/home/erlend/Projects/fzero-gx-pc` on branch `main`, remote `origin` = `https://github.com/jacerle/fzero-gx-pc.git` (public).
  - Commits pushed to `origin/main`:
    - `f8e62ed` — Milestones 1–3: RVZ/ISO/FST, ADX/Sofdec AV, asset pipeline (`.lz`/`.arc`/`.tpl`/`.gma`), GX-to-Vulkan 16-stage TEV/EFB backend, Hor+, AMD FSR 1.0 (EASU + RCAS).
    - `ce23de3` — Option B: native Dolphin SDK (`fzgx_sdk`), boot path (`fzgx_boot`), decomp header override layer, `main_rel_status` burn-down target.
  - `.gitignore` excludes `build/`, `roms/`, `orig/`, `*.iso`, `*.gcm`, `*.rvz`, `*.wia`, `*.gci`, `*.raw`, `*.dol`, `*.rel`, `*.sfd`, `*.adx`, `librashader.so`, `librashader.dll`, `fzero-gx-pc.json`, `imgui.ini`, `.cache/`, `compile_commands.json`.
- **Submodule (`external/fzero-gx-decomp`) & Standalone Decomp Workspace:**
  - Submodule currently pins `https://github.com/jacerle/fzero-gx-decomp.git` at `c8b427c04` (`main_rel: _prolog.c is two TUs; alloc/camera map data ranges follow the splits`).
  - **2026-11-27 update:** `jacerle/fzero-gx-decomp` (`main`) has been synced with upstream `karamzov123/fzero-gx-decomp` up to **`daa95daa3`** and cloned as a standalone workspace at `/home/erlend/Projects/fzero-gx-decomp` (with `upstream` remote configured and a local `HANDOFF.md` excluded via `.git/info/exclude`) for upstream R1–R7 PR work.
  - Upstream decomp matches 16/16 retail `GFZE01` binaries byte-for-byte (`7,351 / 7,351` functions, `98.79%` authored C), and is actively working through R1–R7 recompilation readiness (`external/fzero-gx-decomp/docs/RECOMPILATION_SCOPE.md`).
  - The submodule checkout also has the retail targets in `orig/GFZE01/` (gitignored, extracted from the RVZ) and the CodeWarrior toolchain in `build/`, so upstream's matching check runs locally (`python3 configure.py && ninja`, `uv run tools/fzgx.py lint`, `uv run tools/fzgx.py check <fn>`).
  - **Upstream PR [karamzov123/fzero-gx-decomp#438](https://github.com/karamzov123/fzero-gx-decomp/pull/438)** (`include: share DVDFileInfo and PADStatus SDK types; C++-safe card.h`) was **MERGED on 2026-11-27** (included in `daa95daa3`). **When bumping `external/fzero-gx-decomp` from `c8b427c04` to `daa95daa3+`, remove the duplicate `DVDFileInfo`/`DVDCallback`/`PADStatus` definitions from `include/fzgx_port/dolphin_sdk.h`** (redefinition errors otherwise) and re-run `main_rel_status` + `ctest`.
  - Not upstreamed: the `arc.c:159` pointer fix. Every pointer spelling (`p + len`, `len + p`, `p += len`, `&p[len]`, `&len[p]`) changes one `add` operand order in `fn_8006A554` (retail `add r25, r28, r25`), breaking `main.dol`. A matching portable spelling needs a pointer-sized integer typedef (`u32` under MWCC) that upstream does not have yet; the port keeps `patches/decomp/src/dol/arc.c.patch`.

---

## 3. Host Environment & Build Commands

- **Host:** Bazzite Linux (`x86_64`), GCC `16.2.1`, CMake, Ninja, Vulkan `1.4.350` (`AMD Radeon RX 7900 XT (RADV NAVI31)`).
- **Homebrew dependencies installed** (in `/home/linuxbrew/.linuxbrew`, picked up automatically by `CMakeLists.txt`):
  - `sdl3` (`3.4.20`)
  - `glslang` (`16.6.0`)
  - `zstd` (`1.6.0`)
  - `ffmpeg` (`9.1` — `libavformat 63.16.100`, `libavcodec 63.24.100`, `libavutil 61.19.100`, `libswscale 10.5.100`, `libswresample 7.6.100`)
- **Fetched dependencies (via CMake `FetchContent` in `build/linux-debug/_deps`):**
  - `Vulkan-Headers` `v1.4.369`, `volk` `vulkan-sdk-1.4.363.0`, `nlohmann_json` `v3.11.3`, `imgui` `v1.92.9`.

### Common Commands
```bash
cd /home/erlend/Projects/fzero-gx-pc

# Configure & build
cmake --preset linux-debug
cmake --build --preset linux-debug

# Run unit tests
ctest --preset linux-debug --output-on-failure

# Headless smoke verification
./build/linux-debug/fzero-gx-pc --headless-smoke

# Launch GUI app
./build/linux-debug/fzero-gx-pc
```

---

## 4. Completed Milestones & File Inventory

### Build, Shaders, Shims & CI
- `CMakeLists.txt`, `CMakePresets.json`, `.github/workflows/ci.yml`, `cmake/embed_spv.py`
- `shaders/test_scene.vert`, `shaders/test_scene.frag`, `shaders/present_crt.vert`, `shaders/present_crt.frag` (compiled to SPIR-V via `glslangValidator` and embedded into `generated/embedded_shaders.hpp`)
- `third_party/librashader/librashader.h`, `third_party/librashader/librashader_ld.h`, `third_party/librashader/LICENSE.md` (MIT `v0.12.0`)
- `include/fzgx_port/mwcc_shim.h`, `include/fzgx_port/platform_api.h`, `src/platform/platform_bridge.cpp`

### Core, Disc, Executables, Audio, Save & Input (`fzgx_core`)
- `src/core/paths.hpp/.cpp`, `src/core/config.hpp/.cpp`
- `src/disc/sha1.hpp/.cpp`, `src/disc/disc_image.hpp/.cpp`, `src/disc/rvz_backend.hpp/.cpp`
- `src/game/dol_rel_loader.hpp/.cpp`
- `src/audio/adx_decoder.hpp/.cpp`, `src/audio/audio_engine.hpp/.cpp`
- `src/save/gci_card.hpp/.cpp`
- `src/input/input_manager.hpp/.cpp`

### Video, FMV & Overlay UI (`fzgx_video_ui` + `src/main.cpp`)
- `src/video/volk_impl.c`
- `src/video/librashader_bridge.hpp/.cpp`
- `src/video/sofdec_player.hpp/.cpp`
- `src/video/vulkan_renderer.hpp/.cpp`
- `src/ui/overlay.hpp/.cpp` (6 tabs: `General & ROM`, `Disc & Assets`, `Video & Aspect`, `Shaders & Post-FX`, `Controllers`, `Audio & FMV`)

### Tests & Docs
- `tests/test_main.cpp`, `tests/test_sdk.c` (native OS/DVD/VI/PAD + decomp `arc.c`/`PADClamp`), `tests/test_sdk_card.c` (native CARD over GCI folders)
- `README.md`, `docs/ARCHITECTURE.md`, `docs/HANDOFF.md`

### Option B: Native SDK & Decomp TUs (`fzgx_sdk`, `fzgx_decomp`)
- Decisions (user, 2026-11-26): the retail SDK TUs are **replaced**, not emulated (they program MMIO/EXI/SI and contain `asm`); decomp portability blockers are fixed by **downstream overlay patches**, never by editing the submodule.
- `patches/decomp/<path>.patch` + CMake `fzgx_decomp_sources()`: copies the TU to `build/<preset>/decomp_overlay/<path>` and `git apply`s the patch (GIT_CEILING_DIRECTORIES keeps git from seeing this repo). Current overlay: `src/dol/arc.c` (64-bit pointer truncation at the path walk; `fzgx_arc_to_host_order()` call in ARCInitHandle).
- `fzgx_decomp_env` (INTERFACE): decomp include dirs + `mwcc_shim.h` force-included for C; `-Werror` for pointer/int casts, int-conversion, implicit declarations, incompatible pointers.
- `include/fzgx_port/dolphin_sdk.h`: SDK surface (retail layouts for DVDFileInfo, PADStatus, VI callbacks; decomp's `dvd.h`/`card.h`/`OSTime.h` types). C only (decomp `card.h` uses `new` as a parameter name).
- `src/sdk/sdk_runtime.c`: deferred completion queue. Async DVD/CARD do host I/O immediately; callbacks run at `VIWaitForRetrace`, status polls, `OSGetTime/OSGetTick`, and interrupt re-enable; never while `OSDisableInterrupts` is in effect.
- `src/sdk/os.c` (arena 24 MiB, OSReport/OSPanic, OSGetTime = local time since 2000 at 40.5 MHz, OSTicksToCalendarTime, interrupts), `dvd.c` (paths with `.`/`..`/cwd, DVDOpen/FastOpen/Read*/ReadAbsAsyncPrio, mod overrides via `DiscImage::read_logical`), `vi.c` (retrace count + pre/post callbacks, pacing via `fzgx::set_vi_retrace_hook` or 59.94 Hz sleep), `pad.c` (InputManager ports, PADControlMotor rumble), `card.c` (GCI folders as a 2043-block card: stable fileNo per mount, raw 64-byte dentries round-trip through CARDGetStatus/SetStatus, current game's files filtered by `DVDGetCurrentDiskID()`; no FAT/system area, so CARDCheck never reports BROKEN/ENCODING).
- Game-side CARD externs in `memcard.c` (u8 channels, `void (*)(void)` callbacks) do not match retail signatures; overlay patches will be needed when that TU is compiled.
- `src/decomp_port/dol_data.c`: data stand-ins for the linked TUs (upstream data-import blobs are MWCC-only and drag in unrelated objects); `arc_host_order.c`: BE→host U8 header/node conversion.
- `src/platform/platform_services.hpp`: `bind_platform_services`, `set_vi_retrace_hook` (runs at every `VIWaitForRetrace`, before the 59.94 Hz pacing sleep).
- **Boot path** (`fzgx_boot`, `src/boot/boot.{h,c}`): `fzgx_boot_main(entry)` mirrors retail `game_main.c` (`fn_80005660`/`fn_800058D8`): `OSInit` → `DVDInit` → `VIInit` → `PADInit` → `PADSetAnalogMode(3)`, then calls the game entry. Retail goes through `fze.sample.rel`, whose `_prolog` decrypts and links `main_rel`; natively that stage is skipped and `main_rel`'s `_prolog` (the game main loop, which calls `CARDInit` itself) is the entry: `fzgx_main_module_entry`, NULL until `main_rel` compiles natively. Each boot resets the arena and drops pending completions, so re-boot is clean.
- Threading: the game owns the main thread. `src/main.cpp` `HostLoop::run_frame()` (events, input, overlay, render) runs from the idle loop and, while a game runs, from the retrace hook, so game and host never race. Quitting mid-game shuts down and `std::exit`s from the hook. Overlay `General & ROM` → **Boot Game** (or `--boot`) requests the boot; disc remount, ROM/mod picks and SHA-1 are locked while it runs.
- **Decomp header layer** (2026-11-26; scope chosen by the user: header layer + burn-down, no per-TU patches yet, stay 64-bit): `include/fzgx_port/decomp_overrides/` is searched before the decomp's `include/` and replaces every hardware/asm header `main_rel` uses, same names and include guards: `psvec*.h`, `fpscr*.h`, `gqr.h`, `ppc_ops.h`, `ppc_lc.h`, `lc_mtx.h`, `mathutil*.h`, `dolphin/hw_regs.h`, `dolphin/gx.h`, `dolphin/os/OSFastCast.h`. Gekko state (16 KiB locked cache = `LC_BASE`/mtxA, GQRs, quantized `psq_l`/`psq_st`) lives in `include/fzgx_port/ppc_state.h` + `src/sdk/ppc_state.c`; `OSInit` clears the GQRs like retail. Deliberately not provided, so code using them fails to compile until it moves to an SDK call: `GXWGFifo` direct stores, `GX_FIFO_BASE`, `VI_REG_LITERAL`, `sdk_addresses.h`. Not overridden (not used by `main_rel`): `sfxmc.h`, `adx_compiler_diff.h`, `sofdec/*_ppc.h`.
- `mwcc_shim.h` now provides the MWCC intrinsics used by decomp C with MWCC prototypes (`__cntlzw`, `__rlwinm`, `__rlwnm`, `__rlwimi`, `__lwbrx`, `__fabs`, `__fabsf`, `__abs`, `__fmadds`, `__frsqrte`, `__dcbz`, `__sync`), maps `__declspec(...)` away on GCC/Clang, and no longer includes `<math.h>`/`<stdbool.h>` into decomp TUs (they declare MSL functions themselves; libm goes through compiler builtins).
- **`main_rel` burn-down:** `cmake --build --preset linux-debug --target main_rel_status` rewrites `docs/MAIN_REL_STATUS.md` (`tools/decomp_status.py`). At decomp `c8b427c04`: **6 / 85 TUs compile** with the 64-bit hazards fatal, 23,727 errors. First errors: pointer↔`u32` casts in 50 TUs, declarations conflicting inside one TU (upstream mid TU-collapse) in 17. Largest classes: 7,770 pointer→int casts, 6,936 non-constant initializers (pointers in `u32` data tables, 12 TUs), 2,467 int→pointer casts. The clean TUs build in `fzgx_main_rel` (GCC/Clang only, not linked). Known residual risk in clean TUs: MSL prototypes with `u32` sizes (`memset(void *, int, u32)`) bind to the host libc (`-Wbuiltin-declaration-mismatch`).

### Verification Summary (as of 2026-11-26)
- **Clean build:** 0 warnings in the port's own targets (`fzgx_core`, `fzgx_video_ui`, `fzgx_sdk`, `fzgx_boot`, `fzgx_tests`, `fzero-gx-pc`); decomp TUs in `fzgx_main_rel` warn about their MSL prototypes (`-Wbuiltin-declaration-mismatch`) and are not restyled.
- **Unit tests (`ctest --preset linux-debug`):** `1/1` passed, including Option B: `test_native_sdk` (OS calendar/arena/interrupts, DVD paths + sync/async reads + mod override, VI retrace callbacks, PAD read/clamp, decomp `arc.c` on a big-endian archive), `test_native_sdk_card`, `test_native_boot`, and `fzgx_test_decomp_layer` (intrinsic semantics, FPSCR/fast-cast saturation, vector and mtxA kernels, GQR-quantized merge kernel).
- **Headless smoke (`--headless-smoke`):** Passed (`RVZ Zstandard: v1.6.0`, `FFmpeg Sofdec: Enabled`, `Vulkan loader OK (3 device(s), primary: AMD Radeon RX 7900 XT (RADV NAVI31))`).
- **Real `.rvz` asset inspection (`--inspect-asset`):** Verified live decompression & parsing of `vehicle/vehicle_sel.arc.lz` (124 files, all `.gma` models and `.tpl` texture packs), `vehicle_parts/parts_all.arc.lz`, and `stage/st01.gma.lz` / `stage/st01.tpl.lz`.

---

## 5. Known Gaps & Deferred Items

- **Submodule bump pending:** `external/fzero-gx-decomp` is still pinned at `c8b427c04`; upstream (`daa95daa3`) now includes merged PR #438 and Sofdec TU work. Bumping requires removing duplicate `DVDFileInfo`/`DVDCallback`/`PADStatus` from `include/fzgx_port/dolphin_sdk.h`.
- **RVZ compression:** Supports `None (0)` and `Zstandard (5)` (standard Dolphin RVZ); `Bzip2`, `LZMA`, and `LZMA2` return an explicit unsupported error.
- **`librashader` binary:** Loaded dynamically at runtime (`librashader.so` / `librashader.dll`); user must place the prebuilt binary beside the executable or set its path in `Shaders & Post-FX` to use `.slangp` presets (otherwise the built-in Vulkan CRT shader + AMD FSR 1.0 work out of the box).
- **WUP-028 USB Adapter:** `GcUsbAdapterProvider` is a stub pending `libusb` integration.
- **Upstream decomp portability:** `external/fzero-gx-decomp` still has DOL TU splits, data imports, direct GX FIFO writes, and 32-bit pointer casts being retired upstream.

---

## 6. Recommended Next Steps

1. **Active Priority — Contribute Upstream R1–R7 Fixes First (`/home/erlend/Projects/fzero-gx-decomp`):**
   - Work from the standalone synced fork `/home/erlend/Projects/fzero-gx-decomp` (`jacerle/fzero-gx-decomp`, tracking `karamzov123/fzero-gx-decomp:main`) on R1–R5 recompilation readiness: DOL TU map/collapse, data ownership, shared SDK headers, hardware-address removal, and 64-bit pointer↔`u32` fixes.
   - See `/home/erlend/Projects/fzero-gx-decomp/HANDOFF.md` for the full list of upstream blockers surfaced by `fzero-gx-pc`.

2. **When Returning to `fzero-gx-pc` (Submodule Bump + `main_rel` Bring-Up):**
   - Bump `external/fzero-gx-decomp` to the latest synced `jacerle/fzero-gx-decomp` commit, remove the now-upstream `DVDFileInfo`/`DVDCallback`/`PADStatus` duplicates in `include/fzgx_port/dolphin_sdk.h`, and re-run `main_rel_status` + `ctest`.
   - Continue per-TU bring-up from `docs/MAIN_REL_STATUS.md` (pointer↔`u32` casts, 2,651 data-import files, DOL code called by `main_rel`, per-module `_prolog` renaming at link time).
   - Known blockers (best resolved upstream via R4/R5): `mathutil.c` (53 paired-single `asm` entry points behind `void(void)` prototypes), `os/heap.c` / `os/OSArena.c` / `fstload.c` (`u32` pointer truncation), and ad-hoc SDK externs with `u32` pointer returns (`extern u32 OSGetArenaHi(void)` in 17 files, `DVDReadPrio(u32, ...)`).
