# F-Zero GX Native PC Port — Architecture & Roadmap

## 1. Design Decisions

| Area | Choice | Rationale |
|---|---|---|
| **Repo Layout** | Downstream repo referencing `jacerle/fzero-gx-decomp` submodule | Keeps decomp matching commits cleanly separated from PC Vulkan/SDL3 runtime |
| **Target ABI** | 64-bit native (`x86_64`) + `mwcc_shim.h` | Enforces 32-bit `u32`/`s32` widths while bridging MWCC intrinsics & endianness |
| **Graphics** | Direct Vulkan 1.3 (`volk` + dynamic rendering) | Native `VkImage`/`VkCommandBuffer` control for GX translation, `librashader`, and ImGui |
| **Post-FX** | Internal resolution scaling + `librashader` + built-in CRT fallback | 480p CRT feed option for crisp scanlines; FSR 1.0 deferred cleanly to Milestone 2+ |
| **Widescreen** | Arbitrary Hor+ (`Auto`, `4:3`, `16:9`, `16:10`, `21:9`) | Widens 3D frustum culling & camera FOV; pillarboxes 2D/FMV; configurable race HUD |
| **Framerate** | Fixed 60 Hz physics + render interpolation | Preserves retail physics, drift mechanics, and ghost replay determinism |
| **Disc Intake** | Runtime mounting (`.iso`/`.gcm`, RVZ stub -> M2) + mod overrides | Avoids 1.4 GB extraction copies; validates GameID `GFZE01` & SHA-1 |
| **Saves** | Dolphin-compatible `.gci` folder (Slots A & B) | Portable saves/ghosts/custom machines with Dolphin without 16 MB card limits |
| **Audio / FMV** | SDL3 Audio (AX/DSP + ADX) + FFmpeg (`libavcodec`) for `.sfd` | Clean C boundary (`fzgx_av_*`) behind `include/fzgx_port/platform_api.h` |
| **Input** | `IInputProvider` (SDL3 Gamepad + Keyboard + WUP-028 stub) | Accurate stick outer-gate + dual-stage analog/digital L/R trigger calibration |

---

## 2. Repository Structure

```
fzero-gx-pc/
├── external/fzero-gx-decomp/   # Submodule synced with karamzov123/fzero-gx-decomp
├── third_party/librashader/    # MIT librashader v0.12.0 C dynamic-loader headers
├── include/fzgx_port/          # MWCC shims & C-callable platform boundary
├── shaders/                    # GLSL -> SPIR-V embedded calibration & CRT shaders
├── src/
│   ├── core/                   # XDG/AppData paths & JSON AppConfig
│   ├── disc/                   # Streaming SHA-1, GameCube ISO/GCM/RVZ + FST + mod overlay
│   ├── save/                   # Dolphin-compatible GCI virtual Memory Card manager
│   ├── input/                  # IInputProvider, stick/trigger math, SDL3 Gamepad
│   ├── platform/               # C bridge connecting decomp SDK calls to PC services
│   ├── video/                  # Volk Vulkan 1.3 renderer & librashader bridge
│   └── ui/                     # Dear ImGui launcher & 5-tab settings overlay
└── tests/                      # CTest unit suite (disc, FST, GCI, config, input, shims)
```

---

## 3. Milestone Roadmap

1. **Milestone 1 (Complete):** Repo/submodule setup, build & CI, config/disc/GCI/input core, `mwcc_shim.h` + `platform_api.h`, Vulkan 1.3 renderer, synthetic 480p/HD Hor+ calibration scene, `librashader` dynamic loader + built-in CRT shader, Dear ImGui launcher & overlay, unit tests.
2. **Milestone 2 (Complete):**
   - Full GameCube `.rvz`/`.wia` streaming backend (`RvzDiscBackend`: Zstandard chunk decompression, Lagged Fibonacci PRNG RVZ packing, 32-chunk LRU cache, logical ISO SHA-1).
   - `main.dol` and `.rel` executable header/section parsers + Disc & Asset Browser overlay tab.
   - Real SDL3 Audio mixer + built-in CRI ADX v3/v4 decoder + FFmpeg Sofdec (`.sfd`) FMV decoder feeding the Vulkan 1.3 + CRT shader pipeline.
3. **Milestone 3 — GX-to-Vulkan Backend (Complete):**
   - Amusement Vision `.lz` (AV-LZSS) decompressor, U8 `.arc` parser, GameCube tiled texture decoders (`I4`, `I8`, `IA4`, `IA8`, `RGB565`, `RGB5A3`, `RGBA8`, `CMPR`), Amusement Vision / Nintendo `.tpl` loaders, and `.gma` / `GCMF` + GX display-list / stitched-mesh parsers.
   - Core **GX-to-Vulkan 1.3** 3D renderer (`GxVulkanBackend` + `GxStateProcessor`) with depth buffer, pipeline cache, 16-stage TEV ubershader, alpha compare, fog, live GX FIFO command decoding, mid-frame EFB copy-to-texture, Hor+ widescreen projection, internal resolution scaling, AMD FSR 1.0 (EASU + RCAS), and CRT/`librashader` post-FX integration.
   - Interactive **3D GX Viewer** & **U8 Archive Explorer** / **TPL Inspector** integrated into the Dear ImGui overlay.
4. **Milestone 4 — Widescreen, High-FPS Interpolation & FSR:**
   - Patch/hook 3D projection matrices and frustum culling for arbitrary Hor+ aspect ratios (`16:9`, `21:9`) and toggleable HUD anchoring.
   - Add transform/camera render-frame interpolation for 120/144/240 FPS on top of fixed 60 Hz physics.
   - Add AMD FSR 1.0 (EASU + RCAS) pass slot before final output.
5. **Milestone 5 — Full Game Completion & Polish:**
   - All 15 `.rel` modules linked/loaded, Story Mode `.sfd` FMVs verified, direct `WUP-028` GameCube USB adapter support via `libusb`, PAL/NTSC-J multi-region verification.
