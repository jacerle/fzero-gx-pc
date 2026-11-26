# F-Zero GX Native PC Port (`fzero-gx-pc`) — Session Handoff Document

**Date:** 2026-11-26  
**Workspace:** `/home/erlend/Projects/fzero-gx-pc`  
**Status:** Milestones 1, 2 & 3 complete and verified (including live `.rvz` `.arc.lz` / `.gma.lz` / `.tpl.lz` parsing & GX-to-Vulkan 1.3 3D backend). Ready to begin **Milestone 4** (or DOL/boot integration).

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

- **Local repo:** `/home/erlend/Projects/fzero-gx-pc` initialized on branch `main`.
  - **No commits have been made yet** and **no GitHub remote is configured yet** for `fzero-gx-pc` (only `.gitmodules` and `external/fzero-gx-decomp` are staged; project source files are untracked ready for an initial commit when desired).
  - `.gitignore` excludes `build/`, `roms/`, `orig/`, `*.iso`, `*.gcm`, `*.rvz`, `*.wia`, `*.gci`, `*.raw`, `*.dol`, `*.rel`, `*.sfd`, `*.adx`, `librashader.so`, `librashader.dll`, `fzero-gx-pc.json`, `.cache/`, `compile_commands.json`.
- **Submodule (`external/fzero-gx-decomp`):**
  - Points to `https://github.com/jacerle/fzero-gx-decomp.git` (`main`), synced from `karamzov123/fzero-gx-decomp` up to commit `c8b427c04` (`main_rel: _prolog.c is two TUs; alloc/camera map data ranges follow the splits`).
  - Upstream decomp matches 16/16 retail `GFZE01` binaries byte-for-byte (`7,351 / 7,351` functions, `98.79%` authored C), and is actively working through R1–R7 recompilation readiness (`external/fzero-gx-decomp/docs/RECOMPILATION_SCOPE.md`).

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
- `tests/test_main.cpp`
- `README.md`, `docs/ARCHITECTURE.md`, `docs/HANDOFF.md`

### Verification Summary (as of 2026-11-26)
- **Clean build:** 0 warnings across all targets (`fzgx_core`, `fzgx_video_ui`, `fzgx_tests`, `fzero-gx-pc`).
- **Unit tests (`ctest --preset linux-debug`):** `1/1` passed (including Milestone 3 & Option 1 tests: `test_avlz_decompress`, `test_u8_arc_parser`, `test_gx_texture_decoders`, `test_gma_and_gx_display_list`, `test_gx_hor_plus_projection`, `test_gx_tev_evaluator_and_fifo`, `test_gma_multilayer_tev_generation`).
- **Headless smoke (`--headless-smoke`):** Passed (`RVZ Zstandard: v1.6.0`, `FFmpeg Sofdec: Enabled`, `Vulkan loader OK (3 device(s), primary: AMD Radeon RX 7900 XT (RADV NAVI31))`).
- **Real `.rvz` asset inspection (`--inspect-asset`):** Verified live decompression & parsing of `vehicle/vehicle_sel.arc.lz` (124 files, all `.gma` models and `.tpl` texture packs), `vehicle_parts/parts_all.arc.lz`, and `stage/st01.gma.lz` / `stage/st01.tpl.lz`.

---

## 5. Known Gaps & Deferred Items

- **Git housekeeping:** Repo has no initial commit or remote yet.
- **RVZ compression:** Supports `None (0)` and `Zstandard (5)` (standard Dolphin RVZ); `Bzip2`, `LZMA`, and `LZMA2` return an explicit unsupported error.
- **`librashader` binary:** Loaded dynamically at runtime (`librashader.so` / `librashader.dll`); user must place the prebuilt binary beside the executable or set its path in `Shaders & Post-FX` to use `.slangp` presets (otherwise the built-in Vulkan CRT shader works out of the box).
- **WUP-028 USB Adapter:** `GcUsbAdapterProvider` is a stub pending `libusb` integration.
- **AMD FSR:** Deferred to a later milestone.
- **Upstream decomp portability:** `external/fzero-gx-decomp` still has DOL TU splits, data imports, direct GX FIFO writes, and 32-bit pointer casts being retired upstream.

---

## 6. Recommended Next Steps (Milestone 3 Options)

When resuming, choose between these two Milestone 3 tracks (and optionally commit the current baseline to git first):

1. **Option A (Recommended): GX-to-Vulkan 3D Backend + Real F-Zero GX `.lz` / `.tpl` / `.gma` / `.arc` Rendering**
   - Implement Amusement Vision's `.lz` LZSS decompressor (mirroring `external/fzero-gx-decomp/tools/avlz.py`) and U8 `.arc` parser.
   - Implement GameCube tiled texture decoders (`CMPR`/DXT1, `RGBA8`, `RGB5A3`, `RGB565`, `I4`, `I8`, `IA4`, `IA8`) and `.tpl` / `.gma` model parsers.
   - Build the core **GX-to-Vulkan 1.3** 3D renderer: depth buffer, GX vertex-descriptor/display-list execution, GX projection/modelview matrices, lighting, and TEV combiner shaders — allowing immediate 3D rendering and inspection of real F-Zero GX machines and stages from your `.rvz` with Hor+ widescreen, internal scaling, and CRT shaders.

2. **Option B: Start Compiling & Booting `main.dol`**
   - Begin compiling clean `main.dol` translation units (`OS`, `DVD`, `VI`, `PAD`, `CARD`, `mathutil`, `arc`) against `include/fzgx_port/mwcc_shim.h` and `include/fzgx_port/platform_api.h`, resolving upstream portability blockers as needed.
