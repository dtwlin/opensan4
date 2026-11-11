# Third-Party Notices

OpenSan4 (`san4-linux` / `san4.exe` and `san4-hdpack-gen` / `san4-hdpack-gen.exe`) is licensed under `GPL-2.0-or-later` (see
[LICENSE](LICENSE)). The default build links the libraries below dynamically
from the host system. A portable build (`-DSAN4_STATIC=ON`, see
[README](README.md#portable-static-build)) links them **statically** into the
executables, so redistributing such a binary also redistributes these
components. All of them are GPL-compatible permissive licenses.

| Component | Version (static build) | License (SPDX) | Source |
| :--- | :--- | :--- | :--- |
| DBOPL (DOSBox OPL emulator, `src/opl/`) | DOSBox 0.74-3 | `GPL-2.0-or-later` | <https://www.dosbox.com/> |
| SDL2 | 2.30.12 | `Zlib` | <https://github.com/libsdl-org/SDL> |
| libx86emu | 3.5 | `HPND-sell-variant` (X11-style permissive) | <https://github.com/wfeldt/libx86emu> |
| libpng | 1.6.59 | `libpng-2.0` | <http://www.libpng.org/pub/png/libpng.html> |
| zlib | 1.3.2 | `Zlib` | <https://zlib.net/> |
| GCC `libstdc++` / `libgcc` | toolchain | `GPL-3.0-or-later WITH GCC-exception-3.1` | <https://gcc.gnu.org/> |
| MinGW-w64 runtime / `libwinpthread` (Windows static build) | toolchain | `MIT` / `ZPL-2.1` / `BSD-3-Clause-Clear` | <https://www.mingw-w64.org/> |

Notes:

- libx86emu's license requires its copyright and permission notice to appear
  in all copies and supporting documentation. `scripts/build_static.sh` writes
  `build-static/THIRD_PARTY_LICENSES.txt` (and `scripts/build_win64_static.sh`
  writes `build-win64/THIRD_PARTY_LICENSES.txt`) containing this file followed
  by the verbatim upstream license texts of the statically linked components;
  ship it together with [LICENSE](LICENSE) alongside the binary.
- The GCC Runtime Library Exception permits static linking of `libstdc++` and
  `libgcc` into programs under any license.
- glibc and every platform library SDL loads at runtime (Mesa / GL / EGL,
  X11, Wayland, libdecor, xkbcommon, libdrm / gbm, PulseAudio, PipeWire, ALSA,
  D-Bus, udev) are **not** included in the binary; they are used from the host
  system.
