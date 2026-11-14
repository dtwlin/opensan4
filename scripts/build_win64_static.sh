#!/usr/bin/env bash
set -euo pipefail

# One-command Windows 11 x86_64 self-contained static builder:
#   1. Ensures a local MinGW-w64 sysroot (third_party/mingw64/usr if not installed).
#   2. Builds pinned static Win64 dependencies (zlib 1.3.2, libpng 1.6.59,
#      libx86emu 3.5, SDL2 2.30.12) as LLVM bitcode archives (or COFF archives
#      under --gcc).
#   3. Cross-compiles san4.exe (GUI + PerMonitorV2/UTF-8 manifest),
#      san4-hdpack-gen.exe (CUI), and san4_tests.exe with static C++/winpthread
#      runtimes (-static -static-libstdc++ -static-libgcc -pthread).
#   4. Strips unneeded symbol tables while keeping .rsrc (VERSIONINFO + RT_MANIFEST).
#   5. Verifies PE32+ headers, security flags, manifest, and zero non-inbox DLLs.
#   6. Generates THIRD_PARTY_LICENSES.txt and optional san4-win64-portable.zip.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${SAN4_WIN64_BUILD_DIR:-${ROOT_DIR}/build-win64}"
JOBS="${SAN4_STATIC_JOBS:-$(nproc)}"
PREFIX_OVERRIDE="${SAN4_STATIC_PREFIX:-}"
MINGW_SYSROOT_OVERRIDE="${SAN4_MINGW_SYSROOT:-}"

compiler_mode="clang-lto"
lto_mode="FULL"
clean_build=0
package_zip=0
run_tests=0
no_game=0
game_dir="${SAN4_GAME_DIR:-}"
embed_game_dir="${SAN4_EMBED_GAME_DIR:-}"
cmake_args=()

usage() {
  cat << 'EOF'
Usage: build_win64_static.sh [options] [extra cmake configure args...]

Builds self-contained Windows 11 x86_64 san4.exe and san4-hdpack-gen.exe binaries.

Options:
  --clang-lto            Build static deps & PE32+ binaries with Clang + LLD Full LTO (default)
  --thin-lto             Build static deps & PE32+ binaries with Clang + LLD ThinLTO
  --gcc                  Build static deps & PE32+ binaries with MinGW-w64 GCC (POSIX threads)
  --game-dir <DIR>       Path to original SAN4 DOS game directory (external BYOG, read-only)
  --embed-game-dir <DIR> Embed verified 43 SAN4 files into standalone san4.exe (personal use only)
  --no-game              Build without local game files (-DSAN4_CI_NO_GAME=ON)
  --build-dir <DIR>      Build directory (default: SAN4_WIN64_BUILD_DIR or ./build-win64)
  --prefix <DIR>         Static dependency prefix (overrides SAN4_STATIC_PREFIX)
  --mingw-sysroot <DIR>  MinGW-w64 usr sysroot directory (overrides SAN4_MINGW_SYSROOT)
  --clean                Remove the build directory before configuring
  --package-zip          Create <build-dir>/san4-win64-portable.zip
  --test                 Run ctest --output-on-failure after building
  -j, --jobs <N>         Parallel build jobs (default: SAN4_STATIC_JOBS or nproc)
  -h, --help             Show this help message and exit

Examples:
  ./scripts/build_win64_static.sh --clang-lto --clean --package-zip --test -DSAN4_GAME_DIR=/path/to/SAN4
  ./scripts/build_win64_static.sh --gcc --test -DSAN4_CI_NO_GAME=ON
EOF
}

log() { echo "[win64-build] $*"; }
die() { echo "[win64-build] ERROR: $*" >&2; exit 1; }

while (( $# > 0 )); do
  case "$1" in
    --clang-lto)
      compiler_mode="clang-lto"
      lto_mode="FULL"
      shift
      ;;
    --thin-lto)
      compiler_mode="clang-lto"
      lto_mode="THIN"
      shift
      ;;
    --gcc)
      compiler_mode="gcc"
      shift
      ;;
    --game-dir)
      [[ $# -ge 2 ]] || { echo "error: --game-dir requires an argument" >&2; exit 2; }
      game_dir="$2"
      shift 2
      ;;
    --game-dir=*)
      game_dir="${1#*=}"
      shift
      ;;
    --embed-game-dir)
      [[ $# -ge 2 ]] || { echo "error: --embed-game-dir requires an argument" >&2; exit 2; }
      embed_game_dir="$2"
      shift 2
      ;;
    --embed-game-dir=*)
      embed_game_dir="${1#*=}"
      shift
      ;;
    --no-game)
      no_game=1
      shift
      ;;
    --build-dir)
      [[ $# -ge 2 ]] || { echo "error: --build-dir requires an argument" >&2; exit 2; }
      BUILD_DIR="$2"
      shift 2
      ;;
    --build-dir=*)
      BUILD_DIR="${1#*=}"
      shift
      ;;
    --prefix)
      [[ $# -ge 2 ]] || { echo "error: --prefix requires an argument" >&2; exit 2; }
      PREFIX_OVERRIDE="$2"
      shift 2
      ;;
    --prefix=*)
      PREFIX_OVERRIDE="${1#*=}"
      shift
      ;;
    --mingw-sysroot)
      [[ $# -ge 2 ]] || { echo "error: --mingw-sysroot requires an argument" >&2; exit 2; }
      MINGW_SYSROOT_OVERRIDE="$2"
      shift 2
      ;;
    --mingw-sysroot=*)
      MINGW_SYSROOT_OVERRIDE="${1#*=}"
      shift
      ;;
    --clean)
      clean_build=1
      shift
      ;;
    --package-zip)
      package_zip=1
      shift
      ;;
    --test)
      run_tests=1
      shift
      ;;
    -j|--jobs)
      [[ $# -ge 2 ]] || { echo "error: $1 requires an argument" >&2; exit 2; }
      JOBS="$2"
      shift 2
      ;;
    --jobs=*)
      JOBS="${1#*=}"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      cmake_args+=("$1")
      shift
      ;;
  esac
done

has_cmake_game_dir=0
has_cmake_embed_game_dir=0
has_cmake_ci_no_game=0
for arg in "${cmake_args[@]}"; do
  case "${arg}" in
    -DSAN4_GAME_DIR=*|-DSAN4_GAME_DIR:PATH=*)
      has_cmake_game_dir=1
      [[ -n "${game_dir}" ]] || game_dir="${arg#*=}"
      ;;
    -DSAN4_EMBED_GAME_DIR=*|-DSAN4_EMBED_GAME_DIR:PATH=*)
      has_cmake_embed_game_dir=1
      [[ -n "${embed_game_dir}" ]] || embed_game_dir="${arg#*=}"
      ;;
    -DSAN4_CI_NO_GAME=*)
      has_cmake_ci_no_game=1
      val="${arg#*=}"
      case "${val^^}" in
        ON|TRUE|1|YES) no_game=1 ;;
      esac
      ;;
  esac
done

if (( no_game )) && (( ! has_cmake_ci_no_game )); then
  cmake_args+=("-DSAN4_CI_NO_GAME=ON")
fi
if (( ! no_game )) && [[ -n "${embed_game_dir}" ]] && (( ! has_cmake_embed_game_dir )) && (( ! has_cmake_ci_no_game )); then
  cmake_args+=("-DSAN4_EMBED_GAME_DIR=${embed_game_dir}")
fi
if (( ! no_game )) && [[ -n "${game_dir}" ]] && (( ! has_cmake_game_dir )) && (( ! has_cmake_ci_no_game )); then
  cmake_args+=("-DSAN4_GAME_DIR=${game_dir}")
fi

ensure_mode="clang"
if [[ "${compiler_mode}" == "gcc" ]]; then
  ensure_mode="gcc"
fi

if [[ -n "${MINGW_SYSROOT_OVERRIDE}" ]]; then
  MINGW_SYSROOT_USR="${MINGW_SYSROOT_OVERRIDE}"
else
  eval "$("${SCRIPT_DIR}/ensure_mingw64.sh" --mode "${ensure_mode}")"
fi

if [[ "${compiler_mode}" == "gcc" ]]; then
  export PATH="${MINGW_SYSROOT_USR}/bin:${MINGW_SYSROOT_USR}/x86_64-w64-mingw32/bin:${PATH}"
fi

if [[ -n "${PREFIX_OVERRIDE}" ]]; then
  PREFIX="${PREFIX_OVERRIDE}"
elif [[ "${compiler_mode}" == "clang-lto" ]]; then
  if [[ "${lto_mode}" == "THIN" ]]; then
    PREFIX="${ROOT_DIR}/third_party/static-win64-clang-thinlto"
  else
    PREFIX="${ROOT_DIR}/third_party/static-win64-clang-lto"
  fi
else
  PREFIX="${ROOT_DIR}/third_party/static-win64-gcc"
fi

deps_mode_args=()
if [[ "${compiler_mode}" == "clang-lto" ]]; then
  if [[ "${lto_mode}" == "THIN" ]]; then
    deps_mode_args+=(--thin-lto)
  else
    deps_mode_args+=(--clang-lto)
  fi
  TOOLCHAIN_FILE="${ROOT_DIR}/cmake/toolchains/win64-clang-mingw.cmake"
  cmake_lto_args=(
    -DSAN4_ENABLE_LTO=ON
    -DSAN4_LTO_MODE="${lto_mode}"
  )
else
  deps_mode_args+=(--gcc)
  TOOLCHAIN_FILE="${ROOT_DIR}/cmake/toolchains/win64-gcc-mingw.cmake"
  cmake_lto_args=(
    -DSAN4_ENABLE_LTO=OFF
  )
fi

SAN4_STATIC_PREFIX="${PREFIX}" SAN4_STATIC_JOBS="${JOBS}" \
  "${SCRIPT_DIR}/build_static_deps.sh" \
  --target win64 \
  --mingw-sysroot "${MINGW_SYSROOT_USR}" \
  --prefix "${PREFIX}" \
  --jobs "${JOBS}" \
  "${deps_mode_args[@]}"

if (( clean_build )); then
  rm -rf "${BUILD_DIR}"
fi

emulator_args=()
if command -v wine64 >/dev/null 2>&1; then
  emulator_args+=(-DCMAKE_CROSSCOMPILING_EMULATOR="$(command -v wine64)")
fi

log "Configuring Win64 static build in ${BUILD_DIR} (${compiler_mode})"
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
  -DSAN4_MINGW_SYSROOT="${MINGW_SYSROOT_USR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DSAN4_STATIC=ON \
  -DSAN4_STATIC_PREFIX="${PREFIX}" \
  "${cmake_lto_args[@]}" \
  "${emulator_args[@]}" \
  "${cmake_args[@]}"

cmake --build "${BUILD_DIR}" -j "${JOBS}"

# Strip unneeded symbols/debug sections from the release executables while
# preserving .rsrc (VERSIONINFO + RT_MANIFEST).
find_strip_tool() {
  for sfx in "" -23 -22 -21 -20 -19 -18 -17 -16 -15; do
    if command -v "llvm-strip${sfx}" >/dev/null 2>&1; then
      command -v "llvm-strip${sfx}"
      return 0
    fi
  done
  if command -v x86_64-w64-mingw32-strip >/dev/null 2>&1; then
    command -v x86_64-w64-mingw32-strip
    return 0
  fi
  return 1
}

if STRIP_BIN="$(find_strip_tool)"; then
  log "Stripping symbol and debug tables with ${STRIP_BIN} --strip-all"
  "${STRIP_BIN}" --strip-all "${BUILD_DIR}/san4.exe" "${BUILD_DIR}/san4-hdpack-gen.exe"
fi

verify_args=(
  --expect-gui "${BUILD_DIR}/san4.exe"
  --expect-manifest "${BUILD_DIR}/san4.exe"
  --expect-cui "${BUILD_DIR}/san4-hdpack-gen.exe"
  --expect-manifest "${BUILD_DIR}/san4-hdpack-gen.exe"
)
if [[ "${compiler_mode}" == "clang-lto" ]]; then
  verify_args+=(
    --verify-lto
    --build-dir "${BUILD_DIR}"
    --static-prefix "${PREFIX}"
  )
fi

"${SCRIPT_DIR}/verify_win64_binary.sh" \
  "${verify_args[@]}" \
  "${BUILD_DIR}/san4.exe" \
  "${BUILD_DIR}/san4-hdpack-gen.exe" \
  "${BUILD_DIR}/san4_tests.exe"

# Assemble combined third-party license notices (including MinGW-w64 / GCC runtime).
LICENSES_OUT="${BUILD_DIR}/THIRD_PARTY_LICENSES.txt"
{
  cat "${ROOT_DIR}/THIRD_PARTY_NOTICES.md"
  for lic in "${PREFIX}"/share/licenses/*/*; do
    if [[ -f "${lic}" ]]; then
      printf '\n\n===== %s =====\n\n' "$(basename "$(dirname "${lic}")")"
      cat "${lic}"
    fi
  done
  for sys_lic in \
    "${MINGW_SYSROOT_USR}/share/doc/mingw-w64-common/copyright" \
    "${MINGW_SYSROOT_USR}/share/doc/gcc-mingw-w64-base/copyright"; do
    if [[ -f "${sys_lic}" ]]; then
      printf '\n\n===== %s =====\n\n' "$(basename "$(dirname "${sys_lic}")")"
      cat "${sys_lic}"
    fi
  done
} > "${LICENSES_OUT}"

if (( package_zip )); then
  README_WIN="${BUILD_DIR}/README-WINDOWS.txt"
  if [[ -n "${embed_game_dir}" ]]; then
    cat > "${README_WIN}" << 'WINEOF'
OpenSan4 - Native Windows 11 x86_64 (Personal Standalone Single-EXE Build)
==========================================================================

IMPORTANT PERSONAL-USE NOTICE:
  This san4.exe was locally built with --embed-game-dir and embeds your own
  43 original DOS game files in memory. It is strictly for your personal local
  use — DO NOT redistribute san4.exe.

Quick Start (Windows Explorer Double-Click):
--------------------------------------------
1. Copy san4.exe (or extract this folder) anywhere on your PC.
2. Double-click san4.exe to play — no external SAN4 folder is needed!
   - Saves and custom officers (SAVEDAT.S4 / NBDATA.S4) are stored automatically
     in %LOCALAPPDATA%\SAN4\saves.
   - If an optional "hd_pack" folder is placed next to san4.exe, 4x HD graphics
     are loaded automatically.

Command-Line Options (PowerShell / cmd.exe):
--------------------------------------------
  .\san4.exe --hd-pack C:\path\to\hd_pack
  .\san4.exe --save-dir C:\path\to\saves
  .\san4.exe --help
WINEOF
  else
    cat > "${README_WIN}" << 'WINEOF'
OpenSan4 - Native Windows 11 x86_64 (Self-Contained Portable Build)
===================================================================

Quick Start (Windows Explorer Double-Click):
--------------------------------------------
1. Extract this folder anywhere on your PC.
2. Copy your original Romance of the Three Kingdoms IV (SAN4) DOS game folder
   next to san4.exe as a folder named "SAN4" (so that "SAN4\SAN486.COM" and
   "SAN4\MAIN.EXE" exist beside san4.exe):

     <folder>\
       san4.exe
       san4-hdpack-gen.exe
       SAN4\              <-- original DOS game files (SAN486.COM, MAIN.EXE, ...)
       hd_pack\           <-- optional HD asset pack (auto-detected if present)

   Alternatively, you can place san4.exe directly inside your SAN4 game folder,
   or inside a subfolder of your SAN4 game folder.
3. Double-click san4.exe to play!

Command-Line Options (PowerShell / cmd.exe):
--------------------------------------------
  .\san4.exe --game-dir C:\path\to\SAN4 --hd-pack C:\path\to\hd_pack
  .\san4.exe --help
  .\san4-hdpack-gen.exe --help
WINEOF
  fi

  ZIP_OUT="${BUILD_DIR}/san4-win64-portable.zip"
  ZIP_ARGS=(
    "${BUILD_DIR}/san4.exe"
    "${BUILD_DIR}/san4-hdpack-gen.exe"
    "${README_WIN}"
    "${ROOT_DIR}/README.md"
    "${ROOT_DIR}/LICENSE"
    "${LICENSES_OUT}"
  )
  if [[ -f "${ROOT_DIR}/README.zh-TW.md" ]]; then
    ZIP_ARGS+=("${ROOT_DIR}/README.zh-TW.md")
  fi
  python3 - "${ZIP_OUT}" "${ZIP_ARGS[@]}" << 'PYEOF'
import os
import sys
import zipfile

zip_path = sys.argv[1]
files = sorted(sys.argv[2:], key=lambda p: os.path.basename(p))
with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
    for fpath in files:
        zi = zipfile.ZipInfo(filename=os.path.basename(fpath), date_time=(2020, 1, 1, 0, 0, 0))
        zi.compress_type = zipfile.ZIP_DEFLATED
        zi.create_system = 0
        zi.external_attr = 0o644 << 16
        with open(fpath, "rb") as rf:
            zf.writestr(zi, rf.read(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
print(f"[win64-build] Packaged {zip_path} ({os.path.getsize(zip_path)} bytes)")
PYEOF
fi

if (( run_tests )); then
  ctest --test-dir "${BUILD_DIR}" --output-on-failure
fi

log "Portable Windows 11 x86_64 binaries: ${BUILD_DIR}/san4.exe ${BUILD_DIR}/san4-hdpack-gen.exe"
log "Ship together with: ${ROOT_DIR}/LICENSE ${LICENSES_OUT}"
