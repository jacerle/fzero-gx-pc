# F-Zero GX PC (`fzero-gx-pc`)

A cross-platform (Linux & Windows) native PC port project built downstream of [`fzero-gx-decomp`](https://github.com/karamzov123/fzero-gx-decomp), powered by **SDL3**, **Vulkan 1.3**, **Dear ImGui**, and **`librashader`**.

> [!IMPORTANT]
> **No copyrighted game assets, binaries, or ROMs are included in this repository.**
> You must provide your own legally dumped GameCube **F-Zero GX USA (`GFZE01`)** disc image (`.iso` / `.gcm`).

## Current Milestone: Milestone 2 — RVZ Disc Streaming, Asset Browser, ADX Audio & Sofdec FMV

- **Direct `.rvz` / `.iso` / `.gcm` Mounting:** Full GameCube `.rvz` streaming decompression (Zstandard + Lagged Fibonacci PRNG RVZ packing + 32-chunk LRU cache), logical uncompressed ISO SHA-1 verification, `main.dol` & `.rel` inspection, and mod override layering.
- **Native CRI ADX Music & Sofdec `.sfd` FMV Playback:** Built-in CRI ADX v3/v4 decoder mixed on **SDL3 Audio** (`48 kHz` stereo) and **FFmpeg** `.sfd` FMV playback rendered directly through the **Vulkan 1.3** + `librashader` / Built-in CRT pipeline.
- **Unified Launcher & In-Game Overlay (Dear ImGui + SDL3 + Vulkan 1.3):**
  - First-run ROM picker, GameCube header & FST verifier, background logical SHA-1 hasher, and calibration mode entry.
  - Gamepad- and keyboard-navigable overlay (`F1`, `Guide`, or `Back + Start`) with 6 tabs:
    - **General & ROM:** Disc verification, mod override directory, Dolphin-compatible GCI virtual Memory Card folders (Slots A & B).
    - **Disc & Assets:** Interactive FST file browser, inline `.adx` music player, `.sfd` FMV player, `.rel`/`main.dol` section inspector, and asset extraction to mod directory.
    - **Video & Aspect:** Internal resolution scaling (`1x`–`8x`), Hor+ aspect ratio (`Auto`, `4:3`, `16:9`, `16:10`, `21:9`), Race HUD anchoring (`Safe Centered` vs `Edge Anchored`), target framerate (`60`–`240` / `Uncapped`), and VSync (`Off`, `FIFO`, `Mailbox`).
    - **Shaders & Post-FX:** Sharp mode, built-in Vulkan CRT shader (scanlines, phosphor mask, curvature, vignette), and dynamic-loaded `librashader` `.slangp` filter chains with live parameter sliders and 480p CRT feed toggle.
    - **Controllers:** 4 GameCube ports via SDL3 Gamepad + Keyboard fallback, stick deadzone/outer gate, dual-stage analog + digital-click L/R trigger threshold calibration, and rumble test.
    - **Audio & FMV:** Live SDL3 Audio volume controls, ADX track progress/loop controls, and FFmpeg Sofdec playback controls.
- **480p / HD Calibration Scene:** Renders an interactive F-Zero-inspired neon speedway & SMPTE/trigger calibration target right away so you can tune CRT shaders, aspect ratios, internal resolutions, and controller calibration live.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full technical design and Milestones 2–5 roadmap.

---

## Building

### Linux (Fedora / Bazzite / Ubuntu)

On immutable distros like **Bazzite**, install `sdl3`, `glslang`, `zstd`, and `ffmpeg` via Homebrew:
```bash
brew install sdl3 glslang zstd ffmpeg
```

On **Ubuntu / Debian**:
```bash
sudo apt install cmake ninja-build pkg-config glslang-tools libzstd-dev \
  libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev \
  libxi-dev libxss-dev libwayland-dev libxkbcommon-dev \
  libasound2-dev libpulse-dev libdbus-1-dev libudev-dev
```

Configure, build, and test:
```bash
git clone --recursive <repo-url> fzero-gx-pc
cd fzero-gx-pc
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug --output-on-failure
./build/linux-debug/fzero-gx-pc
```

### Windows (MSVC + Ninja)

Ensure `glslangValidator` (from the Vulkan SDK or `choco install glslang`) is on `PATH`:
```powershell
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc-debug --output-on-failure
```

---

## Using `librashader` CRT Shaders (Optional)

`fzero-gx-pc` includes a built-in Vulkan CRT shader out of the box and also supports full RetroArch `.slangp` shader presets via `librashader` without requiring Rust at build time:

1. Obtain `librashader.so` (Linux) or `librashader.dll` (Windows) from [SnowflakePowered/librashader](https://github.com/SnowflakePowered/librashader/releases) (or build the C API crate).
2. Place `librashader.so` / `librashader.dll` beside the `fzero-gx-pc` executable (or browse to it under **Shaders & Post-FX**).
3. Select a `.slangp` preset (e.g., `crt-royale.slangp` or `crt-lottes.slangp` from `libretro/slang-shaders`), toggle **Feed 480p Resolution to CRT Shader**, and tune any exposed shader parameters live in the overlay.
