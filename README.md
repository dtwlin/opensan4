# OpenSan4 — Native Linux & Windows 11 Runtime + 4x HD Asset Pack Generator for Romance of the Three Kingdoms IV (DOS)

**Language / 語言:** English | [繁體中文（臺灣）](README.zh-TW.md)

**OpenSan4** (`opensan4` — `san4-linux` on Linux, `san4.exe` on Windows 11, and `san4-hdpack-gen` / `san4-hdpack-gen.exe`) is a clean-room, open-source (`GPL-2.0-or-later`) native `x86_64` Linux and Windows 11 runtime and Bring-Your-Own-Game (BYOG) `4x` HD asset pack generator for the Traditional Chinese MS-DOS release of *Romance of the Three Kingdoms IV* (`三國志 IV` / `SAN4`). It runs the original 16-bit DOS game executables directly in a lightweight C++20 process—without DOSBox—while adding modern quality-of-life controls, smooth 24-bit crossfades, multi-threaded pixel upscaling, and full `4x` (`3200x2400`) 32-bit RGBA HD graphics across 100% of the game's archives.

## Legal & Bring-Your-Own-Game (BYOG) Notice

- **Zero Proprietary Assets Included**: This repository contains **only** clean-room engine and generator source code, build scripts, and synthetic unit tests. No Koei Co., Ltd. / Koei Tecmo Games Co., Ltd. executables (`.EXE`, `.COM`), scenarios/data (`.S4`), fonts (`.16P`), music, graphics, or save files are distributed in this repository or compiled into the default `san4-linux`, `san4.exe`, or `san4-hdpack-gen` binaries. (If you opt in to `--embed-game-dir` / `-DSAN4_EMBED_GAME_DIR` locally to pack your own `43` DOS files into a personal single-file executable, that compiled `san4` binary contains your game assets and is strictly for your personal local use—**never redistribute it**.)
- **Non-Affiliation**: *Romance of the Three Kingdoms* (`三國志`) is a trademark of Koei Tecmo Games Co., Ltd. This project is an independent interoperability and preservation runtime and is neither affiliated with nor endorsed by Koei Tecmo.
- **Bring Your Own Game**: To build the default local configuration, generate an HD asset pack locally, and run the game, you must supply your own legitimately owned copy of the Traditional Chinese DOS `SAN4` directory (`43` immutable files, validated by size and SHA-256 at CMake configure time and runtime startup).

## Key Features

- **Zero-DOSBox Native Architecture**:
  - `libx86emu` 16-bit CPU execution with zero-copy `x86emu_set_page()` 4 MB LIM EMS 4.0 (`INT 67h`) page-frame switching (`0xD0000..0xDFFFF`) for Microsoft C 7.0 `MOVE` overlays.
  - Native VESA `0x0102` (`800x600` 16-color 4-plane) VGA hardware state machine (`Write Modes 0..3`, `Read Modes 0..1`, `16`-bit `OUT DX, AX` port splitting, and atomic VSync/frame latching).
  - Case-insensitive Linux Virtual Filesystem (`INT 21h`) with optional separate `--save-dir` copy-on-write isolation so your original game folder stays untouched.
- **Complete `4x` (`3200x2400`) 32-bit RGBA HD Asset Engine & `san4-hdpack-gen`**:
  - **Clean-Room BYOG Generator (`san4-hdpack-gen`)**: Decodes all `SAN4` graphic formats (`LS11`, `KAODATA.S4`, `MONDATA.S4`, `NPK016`/`NPK16`, raw planar sprite sheets) from your local game folder and generates a complete `4x` 32-bit RGBA HD pack (`4,558` PNGs + binary indices covering `13,770` assets) in ~3 seconds across worker threads via Bayer/checkerboard de-dithering, multi-pass `xBR` vector upscaling, eye-catchlight preservation, bilateral shading, and unsharp luminance enhancement.
  - **Instant Startup & Bounded Memory**: Lazily indexes `hd_manifest.tsv` and binary lookup tables in `< 10 ms` at startup without decoding PNGs upfront, streaming textures on demand into a bounded `256 MB` LRU cache.
  - **Screen-Space `3200x2400` HD Canvas & Hardware-Exact Occlusion**: Supports unlimited simultaneous `16x16` HD strategic/city/battle/siege tiles (`MAINCG.S4`, `TOSHICG.S4`, `HEXGRP.S4`, `HKGRP.S4`), canonical source-hashed masked unit/duelist/effect sprites (`HUNIT.S4`, `HIGRP.S4`, `HWGRP.S4`, `HEGRP.S4`), dynamic 4-part `MONDATA.S4` portrait compositing (`8,192` faces from `320` parts), dynamic `GRAPH.S4` button compositing (`1,407` buttons), dynamic `tile_dict.bin` `16x16` cell compositing, `AH=0x10`/`AH=0x18` popup background restoration, overlap-safe `AH=0x17` map-scroll copies, hardware-exact `vram_write8` dialog occlusion, and crisp `4x` anti-aliased `.16P` Big5 Chinese glyphs (`AH=0x13`).
  - **Dynamic Scene-Palette CGs & Smooth 3D LUT Color Grading**: Automatically upgrades dynamic-palette `NPK016` illustrations (`OPENDAT.S4`, `ENDDAT.S4`, `PREGRP.S4`, `HIBACK.S4`, `HEGRP.S4`) to exact live scene palettes on fade-in and grades seasonal, battle, and smooth crossfade shifts via cached `17x17x17` trilinear 3D LUTs.
  - **Hardware OpenGL 3.3 Core GPU Pipeline with Automatic CPU Fallback**: On machines with a hardware OpenGL 3.3+ GPU (NVIDIA, AMD, Intel on Linux and Windows 11), `san4-linux` / `san4.exe` automatically offloads all 5 base pixel upscalers (`nearest`, `sharp2x`, `scale2x`, `xbr2x`, `xbr4x`), `3200x2400` HD overlay compositing, and `17x17x17` 3D LUT trilinear color grading onto integer-exact GLSL `#version 330 core` fragment shaders—with zero static OpenGL library dependencies (`SDL_GL_GetProcAddress` runtime loading) and automatic fallback to the multi-threaded CPU + `SDL_Renderer` path on machines without a hardware GPU or under software rasterizers (`llvmpipe`, `softpipe`, `swrast`, `lavapipe`, `SVGA3D`, `GDI Generic`, `Microsoft Basic Render Driver`, `WARP`).
- **Fluidity, Controls & Stereo OPL2 Audio**:
  - Unthrottled `6 ms` CPU burst scheduling with quiescent input/timer poll detection (`< 2%` idle CPU usage), 60–144 Hz 24-bit crossfades, and music-independent fast-forward (`F4` `1x / 2x / 4x` or hold `Tab` for `16x` Turbo).
  - `1:1` `800x600` zero-lag OS mouse tracking with DPI-scaled hardware color cursors, mouse-wheel list scrolling, middle-mouse / `WASD` map panning, and sample-accurate `44.1 kHz` stereo `DBOPL` YM3812 synthesis.

## Dependencies

On Debian / Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config libsdl2-dev libx86emu-dev libpng-dev
# Optional (recommended for Clang + LTO + PGO + BOLT release builds):
sudo apt-get install -y clang lld llvm bolt
```

SDL `2.0.12` or newer is required; with SDL older than `2.0.18`, VSync is not available and frames are always paced by the host.

If you are on a machine without root privileges where `libpng-dev` is present, run `./scripts/bootstrap_deps.sh` to fetch and unpack `libsdl2-dev`, `libx86emu`, and upstream `dosbox` `dbopl.{h,cpp}` locally into untracked `third_party/` and `src/opl/`.

## Building

### Standard Build (Bring Your Own Game)

Point `-DSAN4_GAME_DIR` at your original `SAN4` directory. CMake verifies all `43` original game files and builds both `san4-linux` and `san4-hdpack-gen`:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSAN4_WARNINGS_AS_ERRORS=ON -DSAN4_GAME_DIR=/path/to/SAN4
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### Clang + LTO + Profile-Guided Optimization (PGO) + `llvm-bolt` Build

`san4-linux` has first-class support for Clang Link-Time Optimization (`FULL` and `ThinLTO`), two-stage **LLVM IR Profile-Guided Optimization (PGO)**, and **`llvm-bolt` post-link binary optimization** (`ext-tsp` basic-block layout, `cdsort` cache-directed function reordering, hot/cold splitting, and identical code folding). On Debian/Ubuntu hosts where only versioned LLVM packages (e.g. `clang-21`, `lld-21`, `llvm-21`) are installed without unversioned symlinks, `-DSAN4_USE_CLANG=ON` automatically locates a matching `clang`/`clang++` pair, resolves its sibling `llvm-ar`/`llvm-ranlib`, `ld.lld`, `llvm-profdata`, and `llvm-strip`, and runs a configure-time bitcode archive + AVX2 target-multiversioning link probe. When `llvm-bolt` is not pre-installed on a Debian/Ubuntu host, `scripts/ensure_bolt.sh` automatically fetches and unpacks matching `bolt-<major>` and `libbolt-<major>-dev` archives rootlessly into `third_party/bolt-<major>/`:

```bash
# One-command Clang Full-LTO + IR PGO + llvm-bolt build (runs deterministic HD 4x,
# classic xBR, and synthetic conformance training into a temporary --save-dir):
./scripts/build_clang_lto.sh --pgo-bolt --clean --test \
  --game-dir /path/to/SAN4 --hd-pack /path/to/hd_pack -DSAN4_WARNINGS_AS_ERRORS=ON

# Standard single-stage Clang Full-LTO build (pass --thin for ThinLTO):
./scripts/build_clang_lto.sh --test -DSAN4_WARNINGS_AS_ERRORS=ON -DSAN4_GAME_DIR=/path/to/SAN4

# Equivalent direct CMake invocation (or: cmake --preset clang-lto -DSAN4_GAME_DIR=/path/to/SAN4,
# or cmake --preset clang-lto-bolt-relocs for -Wl,--emit-relocs):
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSAN4_USE_CLANG=ON -DSAN4_ENABLE_LTO=ON -DSAN4_LTO_MODE=FULL -DSAN4_GAME_DIR=/path/to/SAN4
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### Public CI / Engine-Only Build (No Game Files Needed)

Contributors and CI runners can compile `san4-linux`, `san4-hdpack-gen`, and `san4_tests` (`-Wall -Wextra -Wpedantic -Werror`) and run all `75` synthetic conformance, codec, upscaler, GPU parity, emulation, concurrency, CLI/host, LTO/SIMD, security, and hygiene test suites under AddressSanitizer + UndefinedBehaviorSanitizer, ThreadSanitizer (TSan), and Clang `llvm-cov` source-based code coverage without owning the game files:

```bash
# AddressSanitizer + UndefinedBehaviorSanitizer
cmake -S . -B build-asan -DSAN4_CI_NO_GAME=ON -DSAN4_ENABLE_SANITIZERS=ON -DSAN4_WARNINGS_AS_ERRORS=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure

# ThreadSanitizer
cmake -S . -B build-tsan -DSAN4_CI_NO_GAME=ON -DSAN4_ENABLE_TSAN=ON -DSAN4_WARNINGS_AS_ERRORS=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure

# Clang source-based code coverage report (llvm-cov)
./scripts/run_coverage.sh --min-line-pct 90
```

### CMake Options

- `-DSAN4_GAME_DIR=/path/to/SAN4`: Path to user-supplied DOS `SAN4` directory (verified at configure time unless `SAN4_CI_NO_GAME=ON` or `SAN4_EMBED_GAME_DIR` is used).
- `-DSAN4_EMBED_GAME_DIR=/path/to/SAN4`: Optional personal-use-only build mode (`""` by default). Verifies all `43` original DOS `SAN4` files, compresses them with `zlib` into `${CMAKE_BINARY_DIR}/generated/embedded_game_data.cpp` (never touching the source tree), and links an in-memory VFS directly into `san4-linux` / `san4.exe` so no external `SAN4` folder is needed at runtime. **Do not redistribute binaries built with this option.**
- `-DSAN4_CI_NO_GAME=ON`: Build engine, generator, and synthetic test suite without local game files (`OFF` by default).
- `-DSAN4_USE_CLANG=ON`: Auto-discover matching `clang` and `clang++` (unversioned first, then `clang-23` down to `clang-15`) when `CMAKE_C_COMPILER` / `CMAKE_CXX_COMPILER` are not explicitly specified (`OFF` by default).
- `-DSAN4_ENABLE_LTO=ON|OFF`: Enable Link-Time Optimization. Defaults to `ON` when building with Clang in non-`Debug`, non-sanitizer configurations, and `OFF` otherwise (can also be explicitly set `ON` with GCC for `-flto=auto`, or `OFF` on Clang). Automatically skipped with a warning if combined with sanitizer flags.
- `-DSAN4_LTO_MODE=FULL|THIN`: Select full (`-flto`, default) or thin (`-flto=thin`) LTO when `SAN4_ENABLE_LTO=ON` on Clang.
- `-DSAN4_PGO_MODE=OFF|GENERATE|USE`: Select Clang LLVM IR Profile-Guided Optimization stage (`OFF` by default; mutually exclusive with sanitizer options and requires Clang + `llvm-profdata`). Controlled via `SAN4_PGO_PROFILE_DIR` (raw `.profraw` directory in `GENERATE`) and `SAN4_PGO_PROFILE_FILE` (merged `.profdata` file in `USE`).
- `-DSAN4_BOLT_RELOCS=ON|OFF`: Emit post-link relocation sections (`-Wl,--emit-relocs`) on `san4-linux` for `llvm-bolt` binary optimization (`OFF` by default).
- `-DSAN4_ENABLE_AVX2=ON|OFF`: Compatibility toggle (`OFF` by default). AVX2 compositor kernels in `simd_blend.cpp` are always compiled on `x86_64` with runtime CPU feature dispatch (`__builtin_cpu_supports("avx2")`), while global `-mavx2` is intentionally omitted so binaries remain portable across baseline `x86_64` CPUs.
- `-DSAN4_WARNINGS_AS_ERRORS=ON`: Treat compiler warnings as errors (`-Werror`).
- `-DSAN4_ENABLE_SANITIZERS=ON`: Instrument targets with AddressSanitizer + UndefinedBehaviorSanitizer (mutually exclusive with `SAN4_ENABLE_TSAN`).
- `-DSAN4_ENABLE_TSAN=ON`: Instrument targets with ThreadSanitizer (mutually exclusive with `SAN4_ENABLE_SANITIZERS`).
- `-DSAN4_ENABLE_COVERAGE=ON`: Instrument targets with Clang source-based code coverage (`-fprofile-instr-generate -fcoverage-mapping`; requires Clang and is mutually exclusive with sanitizers, PGO, and `SAN4_STATIC`).
- `-DSAN4_STATIC=ON`: Portable build that links SDL2, libx86emu, libpng, zlib, `libstdc++` and `libgcc` statically from `SAN4_STATIC_PREFIX` (`OFF` by default; incompatible with the sanitizer options). See [Portable Static Build](#portable-static-build).
- `-DSAN4_STATIC_PREFIX=<DIR>`: Prefix populated by `scripts/build_static_deps.sh` (default `third_party/static`, or `third_party/static-clang-lto` under `./scripts/build_static.sh --clang-lto`). Configuration fails with the list of missing headers / archives if it is incomplete.

### Portable Static Build

Build `san4-linux` and `san4-hdpack-gen` once and copy them to any `x86_64` Linux machine with a new enough glibc. No SDL2, libx86emu, libpng, zlib or C++ runtime needs to be installed there, and no `RPATH` or `LD_LIBRARY_PATH` is used:

```bash
# Clang + Full-LTO + IR PGO (including libx86emu.a) + llvm-bolt portable static build:
./scripts/build_static.sh --clang-lto --pgo-bolt --clean --test \
  --game-dir /path/to/SAN4 --hd-pack /path/to/hd_pack -DSAN4_WARNINGS_AS_ERRORS=ON

# Single-stage Clang + Full-LTO portable static build:
./scripts/build_static.sh --clang-lto --test -DSAN4_GAME_DIR=/path/to/SAN4   # or -DSAN4_CI_NO_GAME=ON

# Default system compiler (GCC) portable static build:
./scripts/build_static.sh --test -DSAN4_GAME_DIR=/path/to/SAN4
# => build-static/san4-linux, build-static/san4-hdpack-gen, build-static/THIRD_PARTY_LICENSES.txt
```

`scripts/build_static_deps.sh` (run automatically, rootless and idempotent) downloads pinned, SHA-256-verified upstream releases into `third_party/static/` and builds them as static PIC archives: SDL `2.30.12`, libx86emu `3.5`, libpng `1.6.59`, zlib `1.3.2` (the latter three match the versions the default Debian build links, so screenshots are byte-identical to the dynamic build). Passing `--clang-lto` (or `--clang-lto --thin-lto`) builds LLVM bitcode `.a` archives (`llvm-ar` + `-flto`, with `-fno-strict-aliasing` on `libx86emu`) into an isolated `third_party/static-clang-lto/` prefix—reusing the shared tarball cache without overwriting GCC's ELF archives in `third_party/static/`—so `ld.lld` performs whole-program LTO across `san4-linux` and `libx86emu`. Distro `-dev` packages for SDL's backends are fetched with `apt-get download` into a temporary sysroot purely for compile time. `scripts/build_static.sh` then configures `build-static/` with `-DSAN4_STATIC=ON`, builds, and runs `scripts/verify_static_binary.sh`, which fails if anything other than glibc core libraries appears in `DT_NEEDED` or an `RPATH` / `RUNPATH` is present. The static build's `ctest` adds the same check plus an empty-environment launch test.

What is and is not bundled:

- **Bundled:** SDL2, libx86emu, libpng, zlib, `libstdc++`, `libgcc`. The resulting executables need only `libc.so.6`, `libm.so.6` and the `ld-linux` loader.
- **Loaded from the host at runtime (never bundled):** the display, GPU and audio stacks. SDL `dlopen()`s X11 (`libX11` + `Xext`, `Xcursor`, `Xi`, `Xfixes`, `Xrandr`, `Xss`), Wayland (`wayland-client` / `-egl` / `-cursor`, `xkbcommon`, `libdecor`), KMSDRM (`libdrm`, `gbm`), OpenGL / GLES through GLX / EGL (Mesa or the vendor driver), PulseAudio, PipeWire, ALSA, D-Bus and udev, whichever exist. These stacks are tied to the local kernel drivers, compositor and sound server and must match the target machine, so they cannot be statically linked. Headless (`--headless-script`) runs use SDL's built-in `dummy` driver and need none of them.
- **glibc:** linked dynamically. A binary built on a given host requires at least that host's glibc version (`scripts/verify_static_binary.sh` prints the exact minimum `GLIBC_` symbol version). To support older distributions, run `scripts/build_static.sh` inside an older distribution's container or chroot (for example `debian:bookworm` for glibc `2.36`, or `ubuntu:20.04` for `2.31`); the scripts only need `cmake`, `gcc`/`g++` (or `clang`/`lld`), `make`, `curl`, `python3` and `apt-get`.
- **Why not fully `-static`:** a fully static glibc executable cannot safely `dlopen()` the host's X11, Mesa or audio libraries (they would pull in a second, mismatched `libc.so.6`). In testing, a `-static` link of `san4-linux` passed headless replays but crashed immediately when opening a window, so it is intentionally not offered.

Redistributing a static binary redistributes the bundled components: ship [LICENSE](LICENSE) and `build-static/THIRD_PARTY_LICENSES.txt` with it (see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). All bundled licenses are permissive and GPL-compatible.

### Portable Windows 11 Self-Contained Build (`san4.exe` & `san4-hdpack-gen.exe`)

Cross-compile (or natively build under MSYS2 MinGW-w64) self-contained `x86_64` Windows 11 PE32+ executables (`san4.exe` and `san4-hdpack-gen.exe`) with **zero non-inbox DLL dependencies**—statically bundling `SDL2 2.30.12`, `libx86emu 3.5`, `libpng 1.6.59`, `zlib 1.3.2`, `libstdc++`, `libgcc`, and `libwinpthread` into the `.exe` files:

```bash
# One-command Clang + LLD Full-LTO Windows 11 static cross-build + portable .zip:
./scripts/build_win64_static.sh --clang-lto --clean --package-zip --test \
  -DSAN4_GAME_DIR=/path/to/SAN4 -DSAN4_WARNINGS_AS_ERRORS=ON

# Or engine-only / CI build without local game files (also supports --thin-lto or --gcc):
./scripts/build_win64_static.sh --clang-lto --test -DSAN4_CI_NO_GAME=ON -DSAN4_WARNINGS_AS_ERRORS=ON
# => build-win64/san4.exe, build-win64/san4-hdpack-gen.exe, build-win64/THIRD_PARTY_LICENSES.txt,
#    build-win64/san4-win64-portable.zip
```

- **Rootless MinGW-w64 Bootstrap**: If a system-wide `/usr/x86_64-w64-mingw32` sysroot is not installed, `scripts/ensure_mingw64.sh` automatically fetches and extracts the Debian/Ubuntu `mingw-w64-x86-64-dev`, `gcc-mingw-w64-x86-64-posix`, and `g++-mingw-w64-x86-64-posix` archives rootlessly into untracked `third_party/mingw64/`.
- **Windows 11 Subsystem, Manifest & Security Hardening**:
  - `san4.exe` is linked as `IMAGE_SUBSYSTEM_WINDOWS_GUI` (no console window flash when double-clicked in File Explorer) while automatically attaching to a parent `wt.exe` / PowerShell / `cmd.exe` console when invoked with `--help` or CLI flags.
  - Both `san4.exe` and `san4-hdpack-gen.exe` embed a Windows 11 application manifest (`src/win32/san4.manifest`) declaring `PerMonitorV2` DPI awareness and `<activeCodePage>UTF-8</activeCodePage>`, parse wide `CommandLineToArgvW` arguments into UTF-8, and link with deterministic PE32+ security flags (`HIGH_ENTROPY_VA`, `DYNAMIC_BASE`, `NX_COMPAT`, `--no-insert-timestamp`, `16 MB` stack reserve).
  - Defaults to SDL2's `direct3d11` hardware renderer and `WASAPI` audio (with hotplug recovery) on Windows 11, Unicode-safe `std::filesystem::path` I/O for Traditional Chinese directory names (e.g. `D:\遊戲\三國志IV\SAN4`), and automatic `%LOCALAPPDATA%\SAN4\saves` fallback (`$XDG_DATA_HOME/san4/saves` or `~/.local/share/san4/saves` on Linux) when `SAN4` or its save files are read-only.
  - `scripts/verify_win64_binary.sh` (also wired into `ctest`) parses the PE32+ headers, `.rsrc` manifest tree, LLVM bitcode archives (`100%` bitcode across all 4 static dependencies and internal libraries), and COFF import tables, failing immediately if any non-OS DLL (`SDL2.dll`, `libpng16-16.dll`, `zlib1.dll`, `libstdc++-6.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll`) is imported.
- **Portable Windows 11 Folder Layout**: Extract `san4-win64-portable.zip` next to your `SAN4\` folder (and optional `hd_pack\` folder) and double-click `san4.exe`:
  ```text
  san4-win64\
  ├── san4.exe                  (Self-contained Windows 11 GUI runtime)
  ├── san4-hdpack-gen.exe       (Self-contained Windows 11 CLI HD pack generator)
  ├── SAN4\                     (User-supplied 43 original DOS files; or SAN4_GAME_DIR / --game-dir)
  ├── hd_pack\                  (Auto-loaded when present next to san4.exe; or --hd-pack <DIR>)
  ├── LICENSE
  ├── THIRD_PARTY_LICENSES.txt
  ├── README.md
  └── README.zh-TW.md
  ```

### Personal Single-File Standalone Build (No External `SAN4` Folder Needed)

If you want a single self-contained `san4.exe` (Windows 11) or `san4-linux` (Linux) that you can copy anywhere on your own PC and run directly—without keeping a `SAN4` game folder beside it—pass `--embed-game-dir /path/to/SAN4` (or `-DSAN4_EMBED_GAME_DIR=/path/to/SAN4`):

```bash
# Windows 11 single-EXE standalone build (copy ONLY build-win64/san4.exe anywhere and double-click):
./scripts/build_win64_static.sh --clang-lto --clean --package-zip --test \
  --embed-game-dir /path/to/SAN4 -DSAN4_WARNINGS_AS_ERRORS=ON

# Linux portable static single-binary standalone build:
./scripts/build_static.sh --clang-lto --clean --test \
  --embed-game-dir /path/to/SAN4 -DSAN4_WARNINGS_AS_ERRORS=ON

# Direct CMake build:
cmake -S . -B build-standalone -DCMAKE_BUILD_TYPE=Release \
  -DSAN4_EMBED_GAME_DIR=/path/to/SAN4 -DSAN4_WARNINGS_AS_ERRORS=ON
cmake --build build-standalone -j
```

- **How It Works**: At build time, `cmake/generate_embedded_vfs.py` verifies all `43` official DOS files (ignoring `SAVEDAT.S4` and any extra files), packs them with `zlib` (`~3.6 MB` compressed) into `${CMAKE_BINARY_DIR}/generated/embedded_game_data.cpp` with zero host path strings, and links them into `san4.exe` / `san4-linux` (`san4-hdpack-gen` and `san4_tests` remain unembedded).
- **Persistent Saves & Custom Officers**: At startup, the archive decompresses once in memory (`< 5 ms`, IEEE CRC-32 verified). Campaign saves (`SAVEDAT.S4`) and custom officer edits (`NBDATA.S4`, copy-on-write materialized on first write) are saved automatically to `%LOCALAPPDATA%\SAN4\saves` on Windows (`$XDG_DATA_HOME/san4/saves` or `~/.local/share/san4/saves` on Linux), overriding the embedded factory defaults. Placing an optional `hd_pack` folder next to `san4.exe` / `san4-linux` (or passing `--hd-pack`) still enables `4x` HD graphics automatically.
- **Personal Use Only**: Because a binary compiled with `--embed-game-dir` / `-DSAN4_EMBED_GAME_DIR` embeds your proprietary DOS game files, it is strictly for your own personal local use and **must not be redistributed**.

## Generating & Using the Complete `4x` HD Asset Pack

Generate the complete `4x` (`3200x2400`) 32-bit RGBA HD pack from your own `SAN4` directory into an output folder outside the repository:

```bash
./build/san4-hdpack-gen --game-dir /path/to/SAN4 --out-dir /path/to/hd_pack
```

- `--jobs <1..128>`: Override worker thread count (defaults to hardware concurrency).
- Launch with `--hd-pack /path/to/hd_pack` (automatically selects `--scale xbr4x` `3200x2400` unless `--scale` is explicitly specified; pass `--no-hd-pack` to disable).
- Custom Artist Overrides: Drop any custom `img_<W>x<H>_<HASH16>.png` (`1x`, `2x`, or `4x` 32-bit RGBA, e.g. from `--dump-sprites` / `F9`) into the top level of `/path/to/hd_pack/` and press `F8` to hot-reload live—loose overrides automatically take precedence over generated `hd_manifest.tsv` entries.

## Running & Controls

```bash
./build/san4-linux --game-dir /path/to/SAN4 --hd-pack /path/to/hd_pack
```

| Input / Hotkey | Action |
| :--- | :--- |
| **Left / Right Click** | Confirm / Cancel (zero-lag OS cursor + trackpad tap latch) |
| **Mouse Wheel** | Scroll lists `Up` / `Down` (`Shift + Wheel` = `PgUp` / `PgDn`) |
| **Middle-Mouse Drag / `WASD`** | Pan strategic and tactical maps |
| **`F4`** | Cycle gameplay speed (`1x` / `2x` / `4x`; FM music stays `1.0x`) |
| **Hold `Tab`** | `16x` Turbo fast-forward while held |
| **`F5`** | Cycle upscaler (`nearest` -> `sharp2x` -> `scale2x` -> `xbr2x` -> `xbr4x`) |
| **`F8`** | Hot-reload 32-bit RGBA `--hd-pack` directory |
| **`F9`** | Toggle live 4-bit sprite PNG dumping (`--dump-sprites`) |
| **`F11` / `Alt+Enter`** | Toggle borderless desktop fullscreen |

### CLI Options

All value-taking flags accept both `--flag <VALUE>` and `--flag=<VALUE>` forms:

- `--game-dir <DIR>`: Path to your `SAN4` DOS game directory (can also be set via `SAN4_GAME_DIR`).
- `--save-dir <DIR>`: Keep `SAVEDAT.S4` writes in a separate directory so `--game-dir` stays read-only.
- `--hd-pack <DIR>` / `--no-hd-pack`: Enable (or explicitly disable) 32-bit RGBA HD pack rendering. When `--hd-pack` is active and `--scale` is omitted, defaults to `xbr4x` (`3200x2400`), except on a software (CPU-rasterized) renderer with a drawable height of at most `1440` pixels, where it defaults to `xbr2x` (`1600x1200`, HD detail kept through `4x`→`2x` supersampling). This automatic choice is re-evaluated on window resize until you pick a mode with `F5`.
- `--scale <nearest|sharp2x|scale2x|xbr2x|xbr4x>`: Choose initial pixel upscaler (`sharp2x` default without HD pack; see `--hd-pack` for the HD default). An explicit `--scale` always wins.
- `--renderer <auto|gpu|cpu>`: Rendering backend and present profile. `auto` (default) probes an OpenGL 3.3 Core context (`GL_MAX_TEXTURE_SIZE >= 4096`) on Linux and Windows 11 and enables the two-pass GLSL GPU upscaler + HD/3D-LUT compositor (`backend=opengl33`) when backed by a hardware GPU; if unavailable or backed by a software GL rasterizer (`llvmpipe`, `softpipe`, `swrast`, `lavapipe`, `SVGA3D`, `GDI Generic`, `Microsoft Basic Render Driver`, `WARP`), it falls back automatically to the multi-threaded CPU upscaler/compositor + `SDL_Renderer` (`backend=sdl`, preferring `direct3d11` on Windows 11 and `opengles2` on Linux). `gpu` requests the OpenGL 3.3 Core backend (even on software GL for testing, falling back to `SDL_Renderer` with `profile=hw` if OpenGL 3.3 is unavailable); `cpu` skips the OpenGL probe and forces the CPU + `SDL_Renderer` software profile. VSync is used only on the hardware profile with a display refresh of at least `55 Hz`; otherwise frames are paced to a `60 Hz` deadline. Startup logs one line: `[san4-render] backend=<opengl33|sdl> driver=<name> gl_renderer=<str> profile=<hw|sw> vsync=<0|1> scale=<mode> simd=<avx2|scalar>`. Headless runs keep their plain SDL renderer.
- `--window-scale <1..8>`: Choose initial `4:3` window multiplier (`2` -> `1600x1200` default).
- `--speed <1|2|4>`: Choose initial gameplay speed multiplier.
- `--fullscreen` / `--windowed`: Start in borderless desktop fullscreen or windowed mode (`--windowed` default).
- `--skip-intro`: Skip `OPEN.EXE` intro sequence directly to the `MAIN.EXE` title screen.
- `--dump-sprites <DIR>`: Dump unique blitted sprites as PNGs for custom override authoring.
- `--no-mouse-warp`: Disable automatic OS cursor snapping when the game calls `INT 33h AX=0004h`.
- `--profile-frames <TSV>`: Write per-frame timings (`frame, guest_instrs, run_frame_ms, budget_capped, yield_reason, upscale_ms, composite_ms, upload_ms, present_ms, dirty_tiles, dirty_px`) and print a statistical `GRP600C` (`INT F1h`) call census at exit. Interactive runs write one row per frame the main thread acquires from the emulation thread; frames it never acquired are absent, and a frame with nothing to upload or redraw is not presented and reports `present_ms = 0`. Headless runs write one row per emulated (always presented) frame.
- `--threads <0..64>`: Size of the shared worker pool used for upscaling, HD compositing, and (interactive runs only) background re-upscaling of dynamic-palette CGs. `0` (default) picks `clamp(hardware threads, 4, 16)`. Headless runs also use the pool for upscaling and compositing; the output is bit-identical to a single-threaded run, so screenshots do not depend on the thread count.
- `--deterministic` (requires `--headless-script`): Make headless runs replay bit-identically. Each frame's guest burst is capped by instruction count (`budget_sec × 13.2M`, i.e. `198000` instructions for the headless `15 ms` budget) instead of wall-clock time, and DOS `INT 21h AH=2Ch` time of day is derived from the guest BIOS tick count instead of the host clock. Two runs of the same script then produce byte-identical screenshots, which makes before/after pixel comparisons exact. Off by default; without it, headless timing (and therefore animation phase in screenshots) depends on host speed.
- `--headless-script <S>`: Run `;`-separated automation commands and exit (end scripts with `quit`). Headless runs use SDL's `dummy` video driver, so no window opens even when a display is available (set `SDL_VIDEODRIVER` to override), take input only from the script (host mouse, keyboard, and window-close events are ignored), and leave `SIGINT` / `SIGTERM` at their default terminating action.

## License

OpenSan4 (`san4-linux` / `san4.exe` and `san4-hdpack-gen` / `san4-hdpack-gen.exe`) is licensed under the **GNU General Public License v2.0 or later (`GPL-2.0-or-later`)**; see [LICENSE](LICENSE) for full terms and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for third-party components (`DBOPL` from DOSBox `GPL-2.0-or-later`, `libx86emu` `HPND-sell-variant`, `SDL2` `Zlib`, `libpng` `libpng-2.0`, `zlib` `Zlib`).
