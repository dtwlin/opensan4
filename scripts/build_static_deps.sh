#!/usr/bin/env bash
set -euo pipefail

# Rootless, idempotent builder for the self-contained static dependency set
# consumed by `cmake -DSAN4_STATIC=ON`:
#   SDL2 (static, PIC, every Linux backend runtime-dlopen'ed), libx86emu,
#   libpng and zlib, installed into SAN4_STATIC_PREFIX.
#
# Modes:
#   default       GCC ELF static archives -> third_party/static
#   --clang-lto   Clang + Full LTO bitcode archives -> third_party/static-clang-lto
#   --thin-lto    Clang + ThinLTO bitcode archives  -> third_party/static-clang-thinlto
#   --lto         GCC + -flto=auto archives         -> third_party/static-gcc-lto
#
# Tarball downloads are shared across all modes in third_party/downloads (and
# fall back to any existing third_party/static/downloads cache without
# re-downloading).

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
JOBS="${SAN4_STATIC_JOBS:-$(nproc)}"
MULTIARCH="$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || echo x86_64-linux-gnu)"

USE_CLANG=0
USE_LTO=0
LTO_MODE="FULL"
TARGET_OS="linux"
MINGW_SYSROOT_OVERRIDE="${SAN4_MINGW_SYSROOT:-}"
CLEAN_PREFIX=0
ONLY_X86EMU=0
EXTRA_CFLAGS=""
OUTPUT_LIB=""
PREFIX_OVERRIDE="${SAN4_STATIC_PREFIX:-}"

usage() {
  cat << 'EOF'
Usage: build_static_deps.sh [options]

Builds pinned static SDL2, libx86emu, libpng, and zlib archives for SAN4_STATIC=ON.

Options:
  --target <linux|win64> Target OS (default: linux)
  --mingw-sysroot <DIR>  MinGW-w64 usr sysroot for --target win64
  --clang-lto            Compile static archives with Clang + Full LTO (-flto)
                         into third_party/static[-win64]-clang-lto (default prefix)
  --thin-lto             Compile static archives with Clang + ThinLTO (-flto=thin)
                         into third_party/static[-win64]-clang-thinlto (default prefix)
  --lto                  Enable LTO (Clang LTO when paired with --clang-lto, or
                         GCC -flto=auto into third_party/static-gcc-lto)
  --gcc                  Use GCC (or MinGW-w64 GCC when --target win64)
  --only-x86emu          Compile only libx86emu.a (skips zlib, libpng, and SDL2)
  --extra-cflags <FLG>   Append extra C compiler flags (e.g. PGO flags)
  --output-lib <FILE>    With --only-x86emu: write archive directly to <FILE>
                         without modifying SAN4_STATIC_PREFIX
  --prefix <DIR>         Override output prefix (or set SAN4_STATIC_PREFIX)
  --clean                Clean installed artifacts/stamps in the target prefix
  -j, --jobs <N>         Parallel build jobs (default: SAN4_STATIC_JOBS or nproc)
  -h, --help             Show this help message and exit
EOF
}

while (( $# > 0 )); do
  case "$1" in
    --target)
      [[ $# -ge 2 ]] || { echo "error: --target requires linux or win64" >&2; exit 2; }
      TARGET_OS="$2"
      shift 2
      ;;
    --target=*)
      TARGET_OS="${1#*=}"
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
    --clang-lto)
      USE_CLANG=1
      USE_LTO=1
      shift
      ;;
    --thin-lto)
      USE_CLANG=1
      USE_LTO=1
      LTO_MODE="THIN"
      shift
      ;;
    --lto)
      USE_LTO=1
      shift
      ;;
    --gcc)
      USE_CLANG=0
      shift
      ;;
    --only-x86emu)
      ONLY_X86EMU=1
      shift
      ;;
    --extra-cflags)
      [[ $# -ge 2 ]] || { echo "error: --extra-cflags requires an argument" >&2; exit 2; }
      EXTRA_CFLAGS="$2"
      shift 2
      ;;
    --extra-cflags=*)
      EXTRA_CFLAGS="${1#*=}"
      shift
      ;;
    --output-lib)
      [[ $# -ge 2 ]] || { echo "error: --output-lib requires an argument" >&2; exit 2; }
      OUTPUT_LIB="$2"
      shift 2
      ;;
    --output-lib=*)
      OUTPUT_LIB="${1#*=}"
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
    --clean)
      CLEAN_PREFIX=1
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
      echo "[static-deps] ERROR: unknown argument '$1'" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ "${TARGET_OS}" != "linux" && "${TARGET_OS}" != "win64" ]]; then
  echo "[static-deps] ERROR: invalid --target '${TARGET_OS}' (expected linux or win64)" >&2
  exit 2
fi

if [[ -n "${PREFIX_OVERRIDE}" ]]; then
  PREFIX="${PREFIX_OVERRIDE}"
elif [[ "${TARGET_OS}" == "win64" ]]; then
  if (( USE_CLANG )); then
    if [[ "${LTO_MODE}" == "THIN" ]]; then
      PREFIX="${ROOT_DIR}/third_party/static-win64-clang-thinlto"
    else
      PREFIX="${ROOT_DIR}/third_party/static-win64-clang-lto"
    fi
  elif (( USE_LTO )); then
    PREFIX="${ROOT_DIR}/third_party/static-win64-gcc-lto"
  else
    PREFIX="${ROOT_DIR}/third_party/static-win64-gcc"
  fi
elif (( USE_CLANG )); then
  if [[ "${LTO_MODE}" == "THIN" ]]; then
    PREFIX="${ROOT_DIR}/third_party/static-clang-thinlto"
  else
    PREFIX="${ROOT_DIR}/third_party/static-clang-lto"
  fi
elif (( USE_LTO )); then
  PREFIX="${ROOT_DIR}/third_party/static-gcc-lto"
else
  PREFIX="${ROOT_DIR}/third_party/static"
fi

DOWNLOAD_DIR="${SAN4_STATIC_DOWNLOAD_DIR:-${ROOT_DIR}/third_party/downloads}"

# Bump when build flags change so existing stamps are invalidated.
RECIPE_REV=2

ZLIB_VERSION=1.3.2
ZLIB_URL="https://github.com/madler/zlib/releases/download/v${ZLIB_VERSION}/zlib-${ZLIB_VERSION}.tar.gz"
ZLIB_SHA256=bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16

LIBPNG_VERSION=1.6.59
LIBPNG_URL="https://download.sourceforge.net/libpng/libpng-${LIBPNG_VERSION}.tar.xz"
LIBPNG_SHA256=d80dd2a38a37f803cb9b6ac7b14bd6e74ddc3b654780a8380bdf93523fdb4389

X86EMU_VERSION=3.5
X86EMU_URL="https://github.com/wfeldt/libx86emu/archive/refs/tags/${X86EMU_VERSION}.tar.gz"
X86EMU_SHA256=91da55f5da55017d5a80e2364de30f9520aa8df2744ff587a09ba58d6e3536c8

SDL2_VERSION=2.30.12
SDL2_URL="https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-${SDL2_VERSION}.tar.gz"
SDL2_SHA256=ac356ea55e8b9dd0b2d1fa27da40ef7e238267ccf9324704850d5d47375b48ea

# Compile-time-only headers for SDL's dlopen backends. Missing packages only
# disable the matching optional backend; X11 is mandatory.
SYSROOT_DEV_PACKAGES=(
  x11proto-dev libx11-dev libxext-dev libxrandr-dev libxrender-dev
  libxcursor-dev libxi-dev libxfixes-dev libxss-dev libxkbcommon-dev
  libwayland-dev libwayland-bin wayland-protocols libdecor-0-dev libegl-dev
  libdrm-dev libgbm-dev libpulse-dev libpipewire-0.3-dev libspa-0.2-dev
  libasound2-dev libdbus-1-dev libudev-dev
)

log() { echo "[static-deps] $*"; }
die() { echo "[static-deps] ERROR: $*" >&2; exit 1; }

for tool in curl sha256sum tar make cmake python3; do
  command -v "${tool}" >/dev/null || die "required tool '${tool}' not found"
done

CC_BIN=""
CXX_BIN=""
AR_BIN=""
RANLIB_BIN=""
NM_BIN=""
RC_BIN=""
RC_FLAGS=""
LTO_FLAG=""
LTO_LDFLAGS=""
TOOLCHAIN_STAMP=""
MINGW_SYSROOT_USR=""
MINGW_GCC_LIBDIR=""
MINGW_CXX_INCDIR=""
WIN64_CMAKE_COMMON_ARGS=()

if [[ "${TARGET_OS}" == "win64" ]]; then
  if (( USE_CLANG )); then
    eval "$("${SCRIPT_DIR}/ensure_mingw64.sh" --mode clang ${MINGW_SYSROOT_OVERRIDE:+--sysroot "${MINGW_SYSROOT_OVERRIDE}"})"
  else
    eval "$("${SCRIPT_DIR}/ensure_mingw64.sh" --mode gcc ${MINGW_SYSROOT_OVERRIDE:+--sysroot "${MINGW_SYSROOT_OVERRIDE}"})"
  fi
fi

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT

PREFIX_MAP_FLAGS="-ffile-prefix-map=${WORK_DIR}=. -ffile-prefix-map=${PREFIX}=. -ffile-prefix-map=${ROOT_DIR}=."
if [[ "${TARGET_OS}" == "win64" && -n "${MINGW_SYSROOT_USR}" ]]; then
  PREFIX_MAP_FLAGS="${PREFIX_MAP_FLAGS} -ffile-prefix-map=${MINGW_SYSROOT_USR}=."
fi

if (( USE_CLANG )); then
  if [[ -n "${CC:-}" && -n "${CXX:-}" ]] && "${CXX}" --version 2>/dev/null | head -n1 | python3 -c 'import sys; sys.exit(0 if "clang" in sys.stdin.read().lower() else 1)'; then
    CC_BIN="$(command -v "${CC}")"
    CXX_BIN="$(command -v "${CXX}")"
  else
    for sfx in "" -23 -22 -21 -20 -19 -18 -17 -16 -15; do
      if command -v "clang${sfx}" >/dev/null 2>&1 && command -v "clang++${sfx}" >/dev/null 2>&1; then
        CC_BIN="$(command -v "clang${sfx}")"
        CXX_BIN="$(command -v "clang++${sfx}")"
        break
      fi
    done
  fi
  [[ -n "${CC_BIN}" && -n "${CXX_BIN}" ]] ||
    die "--clang-lto requested, but no matching clang/clang++ (clang or clang-23..15) was found in PATH"

  CLANG_REAL="$(readlink -f "${CXX_BIN}")"
  LLVM_BINDIR="$(dirname "${CLANG_REAL}")"
  LLVM_LIBDIR="$(dirname "${LLVM_BINDIR}")/lib"
  CLANG_VER="$("${CXX_BIN}" -dumpversion)"
  CLANG_MAJOR="${CLANG_VER%%.*}"

  for cand in "${LLVM_BINDIR}/llvm-ar" "llvm-ar-${CLANG_MAJOR}" "llvm-ar"; do
    if command -v "${cand}" >/dev/null 2>&1; then
      AR_BIN="$(command -v "${cand}")"
      break
    fi
  done
  for cand in "${LLVM_BINDIR}/llvm-ranlib" "llvm-ranlib-${CLANG_MAJOR}" "llvm-ranlib"; do
    if command -v "${cand}" >/dev/null 2>&1; then
      RANLIB_BIN="$(command -v "${cand}")"
      break
    fi
  done
  for cand in "${LLVM_BINDIR}/llvm-nm" "llvm-nm-${CLANG_MAJOR}" "llvm-nm"; do
    if command -v "${cand}" >/dev/null 2>&1; then
      NM_BIN="$(command -v "${cand}")"
      break
    fi
  done
  for cand in "${LLVM_BINDIR}/llvm-windres" "llvm-windres-${CLANG_MAJOR}" "llvm-windres" "x86_64-w64-mingw32-windres" "windres"; do
    if command -v "${cand}" >/dev/null 2>&1; then
      RC_BIN="$(command -v "${cand}")"
      break
    fi
  done
  [[ -n "${AR_BIN}" && -n "${RANLIB_BIN}" ]] ||
    die "llvm-ar / llvm-ranlib for Clang ${CLANG_MAJOR} not found (install llvm-${CLANG_MAJOR})"

  LLD_BIN=""
  for cand in "${LLVM_BINDIR}/ld.lld" "ld.lld-${CLANG_MAJOR}" "ld.lld"; do
    if command -v "${cand}" >/dev/null 2>&1; then
      LLD_BIN="$(command -v "${cand}")"
      break
    fi
  done
  if [[ "${TARGET_OS}" == "win64" ]]; then
    [[ -n "${LLD_BIN}" ]] || die "Clang win64 cross-build requires ld.lld-${CLANG_MAJOR} (install lld-${CLANG_MAJOR})"
    [[ -n "${RC_BIN}" ]] || die "Clang win64 cross-build requires llvm-windres (install llvm-${CLANG_MAJOR})"
    LTO_LDFLAGS="-fuse-ld=lld --ld-path=${LLD_BIN}"
  elif [[ -n "${LLD_BIN}" ]]; then
    LTO_LDFLAGS="--ld-path=${LLD_BIN}"
  elif [[ -f "${LLVM_LIBDIR}/LLVMgold.so" ]]; then
    LTO_LDFLAGS=""
  else
    die "Clang LTO requires ld.lld-${CLANG_MAJOR} (lld-${CLANG_MAJOR}) or ${LLVM_LIBDIR}/LLVMgold.so"
  fi

  if [[ "${LTO_MODE}" == "THIN" ]]; then
    LTO_FLAG="-flto=thin"
  else
    LTO_FLAG="-flto"
  fi
  X86EMU_EXTRA_CFLAGS="-fno-strict-aliasing"
  if [[ "${TARGET_OS}" == "win64" ]]; then
    WIN64_TARGET_FLAGS="--target=x86_64-w64-mingw32 --sysroot=${MINGW_SYSROOT_USR}"
    COMMON_CFLAGS="-O3 ${WIN64_TARGET_FLAGS} ${PREFIX_MAP_FLAGS} ${LTO_FLAG}"
    COMMON_CXXFLAGS="${COMMON_CFLAGS}"
    COMMON_LDFLAGS="${WIN64_TARGET_FLAGS} ${LTO_FLAG} ${LTO_LDFLAGS}"
    RC_FLAGS="--target=pe-x86-64 -I${MINGW_SYSROOT_USR}/x86_64-w64-mingw32/include"
    TOOLCHAIN_STAMP=" win64-clang-${CLANG_VER} ${LTO_FLAG}"
    log "Toolchain (win64): ${CC_BIN} (${CLANG_VER}), LTO=${LTO_MODE} (${LTO_FLAG}), sysroot=${MINGW_SYSROOT_USR}, ar=${AR_BIN}, linker=${LLD_BIN}"
  else
    COMMON_CFLAGS="-O3 -fPIC ${PREFIX_MAP_FLAGS} ${LTO_FLAG}"
    COMMON_CXXFLAGS="${COMMON_CFLAGS}"
    COMMON_LDFLAGS="${LTO_FLAG}${LTO_LDFLAGS:+ ${LTO_LDFLAGS}}"
    TOOLCHAIN_STAMP=" clang-${CLANG_VER} ${LTO_FLAG}"
    log "Toolchain: ${CC_BIN} (${CLANG_VER}), LTO=${LTO_MODE} (${LTO_FLAG}), ar=${AR_BIN}, linker=${LLD_BIN:-system-ld+LLVMgold.so}"
  fi
elif [[ "${TARGET_OS}" == "win64" ]]; then
  export PATH="${MINGW_SYSROOT_USR}/bin:${MINGW_SYSROOT_USR}/x86_64-w64-mingw32/bin:${PATH}"
  CC_BIN="${MINGW_CC_BIN}"
  CXX_BIN="${MINGW_CXX_BIN}"
  AR_BIN="${MINGW_AR_BIN}"
  RANLIB_BIN="${MINGW_RANLIB_BIN}"
  RC_BIN="${MINGW_WINDRES_BIN}"
  for cand in llvm-windres-23 llvm-windres-22 llvm-windres-21 llvm-windres-20 llvm-windres-19 llvm-windres; do
    if command -v "${cand}" >/dev/null 2>&1; then
      RC_BIN="$(command -v "${cand}")"
      break
    fi
  done
  X86EMU_EXTRA_CFLAGS="-fno-strict-aliasing"
  WIN64_RELOC_CFLAGS=""
  WIN64_RELOC_CXXFLAGS=""
  if [[ "${MINGW_SYSROOT_USR}" != "/usr" && "${MINGW_SYSROOT_USR}" != "/mingw64" ]]; then
    WIN64_RELOC_CFLAGS="--sysroot=${MINGW_SYSROOT_USR} -B${MINGW_SYSROOT_USR}/bin -B${MINGW_SYSROOT_USR}/x86_64-w64-mingw32/bin -B${MINGW_SYSROOT_USR}/x86_64-w64-mingw32/lib -B${MINGW_GCC_LIBDIR} -L${MINGW_SYSROOT_USR}/x86_64-w64-mingw32/lib -L${MINGW_GCC_LIBDIR} -isystem ${MINGW_SYSROOT_USR}/x86_64-w64-mingw32/include -isystem ${MINGW_GCC_LIBDIR}/include -isystem ${MINGW_GCC_LIBDIR}/include-fixed"
    WIN64_RELOC_CXXFLAGS="${WIN64_RELOC_CFLAGS} -isystem ${MINGW_CXX_INCDIR} -isystem ${MINGW_CXX_INCDIR}/x86_64-w64-mingw32 -isystem ${MINGW_CXX_INCDIR}/backward"
  fi
  COMMON_CFLAGS="-O2 ${PREFIX_MAP_FLAGS}${WIN64_RELOC_CFLAGS:+ ${WIN64_RELOC_CFLAGS}}"
  COMMON_CXXFLAGS="-O2 ${PREFIX_MAP_FLAGS}${WIN64_RELOC_CXXFLAGS:+ ${WIN64_RELOC_CXXFLAGS}}"
  COMMON_LDFLAGS="${WIN64_RELOC_CFLAGS}"
  RC_FLAGS="--target=pe-x86-64 -I${MINGW_SYSROOT_USR}/x86_64-w64-mingw32/include"
  TOOLCHAIN_STAMP=" win64-gcc"
  log "Toolchain (win64-gcc): ${CC_BIN}, sysroot=${MINGW_SYSROOT_USR}, ar=${AR_BIN}"
else
  for tool in gcc g++ ar ranlib; do
    command -v "${tool}" >/dev/null || die "required tool '${tool}' not found"
  done
  CC_BIN="$(command -v "${CC:-gcc}")"
  CXX_BIN="$(command -v "${CXX:-g++}")"
  AR_BIN="$(command -v ar)"
  RANLIB_BIN="$(command -v ranlib)"
  NM_BIN="$(command -v nm || true)"
  X86EMU_EXTRA_CFLAGS=""
  if (( USE_LTO )); then
    command -v gcc-ar >/dev/null && AR_BIN="$(command -v gcc-ar)"
    command -v gcc-ranlib >/dev/null && RANLIB_BIN="$(command -v gcc-ranlib)"
    LTO_FLAG="-flto=auto"
    COMMON_CFLAGS="-O2 -fPIC ${PREFIX_MAP_FLAGS} ${LTO_FLAG}"
    COMMON_CXXFLAGS="${COMMON_CFLAGS}"
    COMMON_LDFLAGS="${LTO_FLAG}"
    TOOLCHAIN_STAMP=" gcc-lto"
    log "Toolchain: ${CC_BIN}, LTO=ON (${LTO_FLAG}), ar=${AR_BIN}"
  else
    COMMON_CFLAGS="-O2 -fPIC ${PREFIX_MAP_FLAGS}"
    COMMON_CXXFLAGS="${COMMON_CFLAGS}"
    COMMON_LDFLAGS=""
    TOOLCHAIN_STAMP=""
  fi
fi

if [[ "${TARGET_OS}" == "win64" ]]; then
  WIN64_CMAKE_COMMON_ARGS=(
    "-DCMAKE_SYSTEM_NAME=Windows"
    "-DCMAKE_SYSTEM_PROCESSOR=x86_64"
    "-DCMAKE_SYSROOT=${MINGW_SYSROOT_USR}"
    "-DCMAKE_C_COMPILER=${CC_BIN}"
    "-DCMAKE_CXX_COMPILER=${CXX_BIN}"
    "-DCMAKE_AR=${AR_BIN}"
    "-DCMAKE_RANLIB=${RANLIB_BIN}"
    "-DCMAKE_C_COMPILER_AR=${AR_BIN}"
    "-DCMAKE_C_COMPILER_RANLIB=${RANLIB_BIN}"
    "-DCMAKE_CXX_COMPILER_AR=${AR_BIN}"
    "-DCMAKE_CXX_COMPILER_RANLIB=${RANLIB_BIN}"
    "-DCMAKE_RC_COMPILER=${RC_BIN}"
    "-DCMAKE_RC_FLAGS=${RC_FLAGS}"
    "-DCMAKE_C_FLAGS=${COMMON_CFLAGS}"
    "-DCMAKE_CXX_FLAGS=${COMMON_CXXFLAGS}"
    "-DCMAKE_EXE_LINKER_FLAGS=${COMMON_LDFLAGS}"
    "-DCMAKE_SHARED_LINKER_FLAGS=${COMMON_LDFLAGS}"
    "-DCMAKE_C_LINK_DEPENDS_USE_LINKER=FALSE"
    "-DCMAKE_CXX_LINK_DEPENDS_USE_LINKER=FALSE"
    "-DCMAKE_FIND_ROOT_PATH=${MINGW_SYSROOT_USR}/x86_64-w64-mingw32;${MINGW_SYSROOT_USR}"
    "-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER"
    "-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY"
    "-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY"
    "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY"
  )
  if (( USE_CLANG )); then
    WIN64_CMAKE_COMMON_ARGS+=(
      "-DCMAKE_C_COMPILER_TARGET=x86_64-w64-mingw32"
      "-DCMAKE_CXX_COMPILER_TARGET=x86_64-w64-mingw32"
    )
  fi
  if [[ -n "${NM_BIN}" ]]; then
    WIN64_CMAKE_COMMON_ARGS+=("-DCMAKE_NM=${NM_BIN}")
  fi
fi

if [[ -n "${OUTPUT_LIB}" ]] && (( ! ONLY_X86EMU )); then
  die "--output-lib requires --only-x86emu"
fi

if (( CLEAN_PREFIX )) && [[ -z "${OUTPUT_LIB}" ]] && [[ -d "${PREFIX}" ]]; then
  log "Cleaning ${PREFIX} (preserving downloads if present)"
  rm -rf "${PREFIX}/include" "${PREFIX}/lib" "${PREFIX}/bin" "${PREFIX}/share" "${PREFIX}/.stamps"
fi

mkdir -p "${DOWNLOAD_DIR}"
if [[ -z "${OUTPUT_LIB}" ]]; then
  mkdir -p "${PREFIX}/downloads" "${PREFIX}/.stamps" "${PREFIX}/share/licenses"
fi

FETCHED_TARBALL=""

# fetch <url> <sha256> <file>: reuse any verified local tarball cache before downloading.
fetch() {
  local url="$1" sha="$2" fname="$3"
  local cand
  for cand in \
    "${PREFIX}/downloads/${fname}" \
    "${DOWNLOAD_DIR}/${fname}" \
    "${ROOT_DIR}/third_party/static/downloads/${fname}"; do
    if [[ -f "${cand}" ]] && echo "${sha}  ${cand}" | sha256sum -c --status; then
      FETCHED_TARBALL="${cand}"
      return
    fi
  done
  local out="${DOWNLOAD_DIR}/${fname}"
  log "Downloading ${url}"
  curl -fsSL --retry 3 -o "${out}.part" "${url}"
  if ! echo "${sha}  ${out}.part" | sha256sum -c --status; then
    rm -f "${out}.part"
    die "SHA-256 mismatch for ${url} (expected ${sha})"
  fi
  mv "${out}.part" "${out}"
  FETCHED_TARBALL="${out}"
}

# unpack <archive_name> -> prints extracted top-level source dir.
unpack() {
  local dest="${WORK_DIR}/src/$1"
  mkdir -p "${dest}"
  tar -xf "${FETCHED_TARBALL}" -C "${dest}"
  local dirs=("${dest}"/*/)
  echo "${dirs[0]%/}"
}

stamp_ok() { [[ -f "${PREFIX}/.stamps/$1" && "$(cat "${PREFIX}/.stamps/$1")" == "$2" ]]; }
stamp_set() { echo "$2" > "${PREFIX}/.stamps/$1"; }

install_license() {
  mkdir -p "${PREFIX}/share/licenses/$1"
  cp "$2" "${PREFIX}/share/licenses/$1/"
}

build_zlib() {
  local id="${ZLIB_VERSION} ${ZLIB_SHA256} r${RECIPE_REV}${TOOLCHAIN_STAMP}"
  stamp_ok zlib "${id}" && { log "zlib ${ZLIB_VERSION} up to date"; return; }
  fetch "${ZLIB_URL}" "${ZLIB_SHA256}" "zlib-${ZLIB_VERSION}.tar.gz"
  local src
  src="$(unpack "zlib-${ZLIB_VERSION}.tar.gz")"
  log "Building zlib ${ZLIB_VERSION} (${TARGET_OS})"
  if [[ "${TARGET_OS}" == "win64" ]]; then
    cmake -S "${src}" -B "${WORK_DIR}/zlib_build" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
      -DCMAKE_INSTALL_LIBDIR=lib \
      "${WIN64_CMAKE_COMMON_ARGS[@]}" \
      -DZLIB_BUILD_TESTING=OFF \
      -DZLIB_BUILD_SHARED=OFF \
      -DZLIB_BUILD_STATIC=ON \
      > "${WORK_DIR}/zlib_configure.log" 2>&1 ||
      { cat "${WORK_DIR}/zlib_configure.log" >&2; die "zlib win64 configure failed"; }
    cmake --build "${WORK_DIR}/zlib_build" -j"${JOBS}" > "${WORK_DIR}/zlib_build.log" 2>&1 ||
      { tail -50 "${WORK_DIR}/zlib_build.log" >&2; die "zlib win64 build failed"; }
    cmake --install "${WORK_DIR}/zlib_build" >/dev/null
    if [[ -f "${PREFIX}/lib/libzs.a" ]]; then
      mv -f "${PREFIX}/lib/libzs.a" "${PREFIX}/lib/libz.a"
    elif [[ -f "${PREFIX}/lib/libzlibstatic.a" ]]; then
      mv -f "${PREFIX}/lib/libzlibstatic.a" "${PREFIX}/lib/libz.a"
    fi
    [[ -f "${PREFIX}/lib/libz.a" ]] || die "zlib win64 install did not produce ${PREFIX}/lib/libz.a"
  else
    (cd "${src}" &&
      CC="${CC_BIN}" AR="${AR_BIN}" RANLIB="${RANLIB_BIN}" \
      CFLAGS="${COMMON_CFLAGS}" LDFLAGS="${COMMON_LDFLAGS}" \
      ./configure --static --prefix="${PREFIX}" --libdir="${PREFIX}/lib" >/dev/null &&
      make -j"${JOBS}" >/dev/null && make install >/dev/null)
  fi
  install_license zlib "${src}/LICENSE"
  stamp_set zlib "${id}"
}

build_libpng() {
  local id="${LIBPNG_VERSION} ${LIBPNG_SHA256} r${RECIPE_REV} zlib-${ZLIB_VERSION}${TOOLCHAIN_STAMP}"
  stamp_ok libpng "${id}" && { log "libpng ${LIBPNG_VERSION} up to date"; return; }
  fetch "${LIBPNG_URL}" "${LIBPNG_SHA256}" "libpng-${LIBPNG_VERSION}.tar.xz"
  local src
  src="$(unpack "libpng-${LIBPNG_VERSION}.tar.xz")"
  log "Building libpng ${LIBPNG_VERSION} (${TARGET_OS})"
  if [[ "${TARGET_OS}" == "win64" ]]; then
    cmake -S "${src}" -B "${WORK_DIR}/libpng_build" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
      -DCMAKE_INSTALL_LIBDIR=lib \
      -DCMAKE_PREFIX_PATH="${PREFIX}" \
      "${WIN64_CMAKE_COMMON_ARGS[@]}" \
      -DPNG_SHARED=OFF \
      -DPNG_STATIC=ON \
      -DPNG_TESTS=OFF \
      -DPNG_TOOLS=OFF \
      -DZLIB_INCLUDE_DIR="${PREFIX}/include" \
      -DZLIB_LIBRARY="${PREFIX}/lib/libz.a" \
      > "${WORK_DIR}/libpng_configure.log" 2>&1 ||
      { cat "${WORK_DIR}/libpng_configure.log" >&2; die "libpng win64 configure failed"; }
    cmake --build "${WORK_DIR}/libpng_build" -j"${JOBS}" > "${WORK_DIR}/libpng_build.log" 2>&1 ||
      { tail -50 "${WORK_DIR}/libpng_build.log" >&2; die "libpng win64 build failed"; }
    cmake --install "${WORK_DIR}/libpng_build" >/dev/null
    if [[ ! -f "${PREFIX}/lib/libpng16.a" && -f "${PREFIX}/lib/libpng16_static.a" ]]; then
      mv -f "${PREFIX}/lib/libpng16_static.a" "${PREFIX}/lib/libpng16.a"
    fi
    [[ -f "${PREFIX}/lib/libpng16.a" ]] || die "libpng win64 install did not produce ${PREFIX}/lib/libpng16.a"
  else
    (cd "${src}" &&
      env CC="${CC_BIN}" AR="${AR_BIN}" RANLIB="${RANLIB_BIN}" ${NM_BIN:+NM="${NM_BIN}"} \
        ./configure --prefix="${PREFIX}" --libdir="${PREFIX}/lib" \
        --disable-shared --enable-static --with-pic --disable-tools \
        CFLAGS="${COMMON_CFLAGS}" CPPFLAGS="-I${PREFIX}/include" \
        LDFLAGS="-L${PREFIX}/lib ${COMMON_LDFLAGS}" >/dev/null &&
      make -j"${JOBS}" >/dev/null && make install >/dev/null)
  fi
  install_license libpng "${src}/LICENSE"
  stamp_set libpng "${id}"
}

build_x86emu() {
  if [[ -z "${OUTPUT_LIB}" && -z "${EXTRA_CFLAGS}" ]]; then
    local id="${X86EMU_VERSION} ${X86EMU_SHA256} r${RECIPE_REV}${TOOLCHAIN_STAMP}"
    stamp_ok x86emu "${id}" && { log "libx86emu ${X86EMU_VERSION} up to date"; return; }
  fi
  fetch "${X86EMU_URL}" "${X86EMU_SHA256}" "libx86emu-${X86EMU_VERSION}.tar.gz"
  local src
  src="$(unpack "libx86emu-${X86EMU_VERSION}.tar.gz")"
  log "Building libx86emu ${X86EMU_VERSION} (${TARGET_OS})${OUTPUT_LIB:+ -> ${OUTPUT_LIB}}"

  python3 - "${src}" "${TARGET_OS}" << 'PYEOF'
import os, re, sys
src = sys.argv[1]
target_os = sys.argv[2]

def must_replace(text: str, old: str, new: str, label: str) -> str:
    assert old in text, f"libx86emu patch target missing ({label})"
    return text.replace(old, new, 1)

if target_os == "win64":
    hdr_path = os.path.join(src, "include", "x86emu_int.h")
    hdr = open(hdr_path, "r", encoding="utf-8").read()
    hdr = must_replace(
        hdr,
        '#define API_SYM\t\t\t__attribute__((visibility("default")))',
        '#ifdef _WIN32\n#define API_SYM\n#else\n#define API_SYM\t\t\t__attribute__((visibility("default")))\n#endif',
        "x86emu_int.h API_SYM",
    )
    open(hdr_path, "w", encoding="utf-8").write(hdr)

mem_path = os.path.join(src, "mem.c")
mem = open(mem_path, "r", encoding="utf-8").read()
if target_os == "win64":
    mem = must_replace(
        mem,
        "#include <sys/io.h>",
        """#ifdef _WIN32
static inline unsigned char inb(unsigned short port) { (void)port; return 0xff; }
static inline unsigned short inw(unsigned short port) { (void)port; return 0xffff; }
static inline unsigned int inl(unsigned short port) { (void)port; return 0xffffffffu; }
static inline void outb(unsigned char val, unsigned short port) { (void)val; (void)port; }
static inline void outw(unsigned short val, unsigned short port) { (void)val; (void)port; }
static inline void outl(unsigned int val, unsigned short port) { (void)val; (void)port; }
#else
#include <sys/io.h>
#endif""",
        "mem.c sys/io.h",
    )

mem = must_replace(
    mem,
    "#define PERM16(a)\t((a) + ((a) << 8))\n#define PERM32(a)\t(PERM16(a) + (PERM16(a) << 16))\n\n// avoid unaligned memory accesses\n#define STRICT_ALIGN\t0",
    "#define PERM16(a)\t((u32)(a) + ((u32)(a) << 8))\n#define PERM32(a)\t(PERM16(a) + (PERM16(a) << 16))\n\n// avoid unaligned memory accesses\n#ifndef STRICT_ALIGN\n#define STRICT_ALIGN\t0\n#endif",
    "mem.c STRICT_ALIGN & PERM32",
)

# Make vm_r_word, vm_r_dword, vm_x_dword, vm_w_word, vm_w_dword, vm_i_dword
# 100% alignment-safe and free of signed 8-bit -> int << 24 overflow.
old_rw = mem[mem.index("\nunsigned vm_r_word(x86emu_mem_t *mem") + 1:mem.index("\nunsigned vm_i_byte(x86emu_t *emu") + 1]
new_rw = """unsigned vm_r_word(x86emu_mem_t *mem, unsigned addr)
{
  mem2_page_t *page;
  unsigned val, page_idx = addr & (X86EMU_PAGE_SIZE - 1);
  unsigned char req = X86EMU_PERM_R | X86EMU_PERM_VALID;

  page = vm_get_page(mem, addr, 1);

  if(
    page_idx >= X86EMU_PAGE_SIZE - 1 ||
    (page->attr[page_idx] & req) != req ||
    (page->attr[page_idx + 1] & req) != req
  ) {
    val = vm_r_byte(mem, addr);
    val += vm_r_byte(mem, addr + 1) << 8;

    return val;
  }

  page->attr[page_idx] |= X86EMU_ACC_R;
  page->attr[page_idx + 1] |= X86EMU_ACC_R;

  val = (unsigned)page->data[page_idx] + ((unsigned)page->data[page_idx + 1] << 8);

  return val;
}


unsigned vm_r_dword(x86emu_mem_t *mem, unsigned addr)
{
  mem2_page_t *page;
  unsigned val, page_idx = addr & (X86EMU_PAGE_SIZE - 1);
  unsigned char req = X86EMU_PERM_R | X86EMU_PERM_VALID;

  page = vm_get_page(mem, addr, 1);

  if(
    page_idx >= X86EMU_PAGE_SIZE - 3 ||
    (page->attr[page_idx] & req) != req ||
    (page->attr[page_idx + 1] & req) != req ||
    (page->attr[page_idx + 2] & req) != req ||
    (page->attr[page_idx + 3] & req) != req
  ) {
    val = vm_r_byte(mem, addr);
    val += vm_r_byte(mem, addr + 1) << 8;
    val += vm_r_byte(mem, addr + 2) << 16;
    val += (unsigned)vm_r_byte(mem, addr + 3) << 24;

    return val;
  }

  page->attr[page_idx] |= X86EMU_ACC_R;
  page->attr[page_idx + 1] |= X86EMU_ACC_R;
  page->attr[page_idx + 2] |= X86EMU_ACC_R;
  page->attr[page_idx + 3] |= X86EMU_ACC_R;

  val = (unsigned)page->data[page_idx] +
    ((unsigned)page->data[page_idx + 1] << 8) +
    ((unsigned)page->data[page_idx + 2] << 16) +
    ((unsigned)page->data[page_idx + 3] << 24);

  return val;
}


unsigned vm_x_byte(x86emu_mem_t *mem, unsigned addr)
{
  mem2_page_t *page;
  unsigned page_idx = addr & (X86EMU_PAGE_SIZE - 1);
  unsigned char *attr;

  page = vm_get_page(mem, addr, 1);
  attr = page->attr + page_idx;

  if((*attr & (X86EMU_PERM_X | X86EMU_PERM_VALID)) == (X86EMU_PERM_X | X86EMU_PERM_VALID)) {
    *attr |= X86EMU_ACC_X;

    return page->data[page_idx];
  }
  else {
    *attr |= X86EMU_ACC_INVALID;
  }

  mem->invalid = 1;

  return 0xff;
}


unsigned vm_x_word(x86emu_mem_t *mem, unsigned addr)
{
  return vm_x_byte(mem, addr) + (vm_x_byte(mem, addr + 1) << 8);
}


unsigned vm_x_dword(x86emu_mem_t *mem, unsigned addr)
{
  return vm_x_word(mem, addr) + ((unsigned)vm_x_word(mem, addr + 2) << 16);
}


void vm_w_byte(x86emu_mem_t *mem, unsigned addr, unsigned val)
{
  mem2_page_t *page;
  unsigned page_idx = addr & (X86EMU_PAGE_SIZE - 1);
  unsigned char *attr;

  page = vm_get_page(mem, addr, 1);
  attr = page->attr + page_idx;

  if(*attr & X86EMU_PERM_W) {
    *attr |= X86EMU_PERM_VALID | X86EMU_ACC_W;
    page->data[page_idx] = val;
  }
  else {
    *attr |= X86EMU_ACC_INVALID;

    mem->invalid = 1;
  }
}


void vm_w_byte_noperm(x86emu_mem_t *mem, unsigned addr, unsigned val)
{
  mem2_page_t *page;
  unsigned page_idx = addr & (X86EMU_PAGE_SIZE - 1);
  unsigned char *attr;

  page = vm_get_page(mem, addr, 1);
  attr = page->attr + page_idx;

  *attr |= X86EMU_PERM_VALID | X86EMU_ACC_W;
  page->data[page_idx] = val;
}


void vm_w_word(x86emu_mem_t *mem, unsigned addr, unsigned val)
{
  mem2_page_t *page;
  unsigned page_idx = addr & (X86EMU_PAGE_SIZE - 1);

  page = vm_get_page(mem, addr, 1);

  if(
    page_idx >= X86EMU_PAGE_SIZE - 1 ||
    !(page->attr[page_idx] & X86EMU_PERM_W) ||
    !(page->attr[page_idx + 1] & X86EMU_PERM_W)
  ) {
    vm_w_byte(mem, addr, val);
    vm_w_byte(mem, addr + 1, val >> 8);

    return;
  }

  page->attr[page_idx] |= X86EMU_PERM_VALID | X86EMU_ACC_W;
  page->attr[page_idx + 1] |= X86EMU_PERM_VALID | X86EMU_ACC_W;

  page->data[page_idx] = val;
  page->data[page_idx + 1] = val >> 8;
}


void vm_w_dword(x86emu_mem_t *mem, unsigned addr, unsigned val)
{
  mem2_page_t *page;
  unsigned page_idx = addr & (X86EMU_PAGE_SIZE - 1);

  page = vm_get_page(mem, addr, 1);

  if(
    page_idx >= X86EMU_PAGE_SIZE - 3 ||
    !(page->attr[page_idx] & X86EMU_PERM_W) ||
    !(page->attr[page_idx + 1] & X86EMU_PERM_W) ||
    !(page->attr[page_idx + 2] & X86EMU_PERM_W) ||
    !(page->attr[page_idx + 3] & X86EMU_PERM_W)
  ) {
    vm_w_byte(mem, addr, val);
    vm_w_byte(mem, addr + 1, val >> 8);
    vm_w_byte(mem, addr + 2, val >> 16);
    vm_w_byte(mem, addr + 3, val >> 24);

    return;
  }

  page->attr[page_idx] |= X86EMU_PERM_VALID | X86EMU_ACC_W;
  page->attr[page_idx + 1] |= X86EMU_PERM_VALID | X86EMU_ACC_W;
  page->attr[page_idx + 2] |= X86EMU_PERM_VALID | X86EMU_ACC_W;
  page->attr[page_idx + 3] |= X86EMU_PERM_VALID | X86EMU_ACC_W;

  page->data[page_idx] = val;
  page->data[page_idx + 1] = val >> 8;
  page->data[page_idx + 2] = val >> 16;
  page->data[page_idx + 3] = val >> 24;
}


"""
mem = must_replace(mem, old_rw, new_rw, "mem.c vm_r/x/w block")
mem = must_replace(
    mem,
    "val += (vm_i_byte(emu, addr + 3) << 24);",
    "val += ((unsigned)vm_i_byte(emu, addr + 3) << 24);",
    "mem.c vm_i_dword",
)
open(mem_path, "w", encoding="utf-8").write(mem)

# Patch prim_ops.c: eliminate all signed-int promotion shift overflows and >=32 shift exponents
prim_path = os.path.join(src, "prim_ops.c")
prim = open(prim_path, "r", encoding="utf-8").read()
start_marker = "u8 rcl_byte(x86emu_t *emu, u8 d, u8 s)"
end_marker = "/****************************************************************************\nREMARKS:\nImplements the SBB instruction and side effects."
assert start_marker in prim and end_marker in prim, "prim_ops.c shift/rotate region not found"
old_shifts = prim[prim.index(start_marker):prim.index(end_marker)]
new_shifts = """u8 rcl_byte(x86emu_t *emu, u8 d, u8 s)
{
	u32 res = d, cnt, mask, cf;
	if ((cnt = s % 9u) != 0u) {
		cf = ((u32)d >> (8u - cnt)) & 0x1u;
		res = ((u32)d << cnt) & 0xffu;
		mask = (1u << (cnt - 1u)) - 1u;
		res |= ((u32)d >> (9u - cnt)) & mask;
		if (ACCESS_FLAG(F_CF)) {
			res |= 1u << (cnt - 1u);
		}
		CONDITIONAL_SET_FLAG(cf, F_CF);
		CONDITIONAL_SET_FLAG(cnt == 1u && XOR2(cf + ((res >> 6u) & 0x2u)), F_OF);
	}
	return (u8)res;
}

u16 rcl_word(x86emu_t *emu, u16 d, u8 s)
{
	u32 res = d, cnt, mask, cf;
	if ((cnt = s % 17u) != 0u) {
		cf = ((u32)d >> (16u - cnt)) & 0x1u;
		res = ((u32)d << cnt) & 0xffffu;
		mask = (1u << (cnt - 1u)) - 1u;
		res |= ((u32)d >> (17u - cnt)) & mask;
		if (ACCESS_FLAG(F_CF)) {
			res |= 1u << (cnt - 1u);
		}
		CONDITIONAL_SET_FLAG(cf, F_CF);
		CONDITIONAL_SET_FLAG(cnt == 1u && XOR2(cf + ((res >> 14u) & 0x2u)), F_OF);
	}
	return (u16)res;
}

u32 rcl_long(x86emu_t *emu, u32 d, u8 s)
{
	u32 res = d, cnt, mask, cf;
	if ((cnt = s % 33u) != 0u) {
		cf = (d >> (32u - cnt)) & 0x1u;
		res = (cnt < 32u) ? (d << cnt) : 0u;
		mask = (1u << (cnt - 1u)) - 1u;
		if (cnt > 1u) {
			res |= (d >> (33u - cnt)) & mask;
		}
		if (ACCESS_FLAG(F_CF)) {
			res |= 1u << (cnt - 1u);
		}
		CONDITIONAL_SET_FLAG(cf, F_CF);
		CONDITIONAL_SET_FLAG(cnt == 1u && XOR2(cf + ((res >> 30u) & 0x2u)), F_OF);
	}
	return res;
}

u8 rcr_byte(x86emu_t *emu, u8 d, u8 s)
{
	u32 res = d, cnt, mask, cf, ocf = 0u;
	if ((cnt = s % 9u) != 0u) {
		if (cnt == 1u) {
			cf = (u32)d & 0x1u;
			ocf = ACCESS_FLAG(F_CF) != 0;
		} else {
			cf = ((u32)d >> (cnt - 1u)) & 0x1u;
		}
		mask = (1u << (8u - cnt)) - 1u;
		res = ((u32)d >> cnt) & mask;
		res |= ((u32)d << (9u - cnt));
		if (ACCESS_FLAG(F_CF)) {
			res |= 1u << (8u - cnt);
		}
		CONDITIONAL_SET_FLAG(cf, F_CF);
		if (cnt == 1u) {
			CONDITIONAL_SET_FLAG(XOR2(ocf + (((u32)d >> 6u) & 0x2u)), F_OF);
		}
	}
	return (u8)res;
}

u16 rcr_word(x86emu_t *emu, u16 d, u8 s)
{
	u32 res = d, cnt, mask, cf, ocf = 0u;
	if ((cnt = s % 17u) != 0u) {
		if (cnt == 1u) {
			cf = (u32)d & 0x1u;
			ocf = ACCESS_FLAG(F_CF) != 0;
		} else {
			cf = ((u32)d >> (cnt - 1u)) & 0x1u;
		}
		mask = (1u << (16u - cnt)) - 1u;
		res = ((u32)d >> cnt) & mask;
		res |= ((u32)d << (17u - cnt));
		if (ACCESS_FLAG(F_CF)) {
			res |= 1u << (16u - cnt);
		}
		CONDITIONAL_SET_FLAG(cf, F_CF);
		if (cnt == 1u) {
			CONDITIONAL_SET_FLAG(XOR2(ocf + (((u32)d >> 14u) & 0x2u)), F_OF);
		}
	}
	return (u16)res;
}

u32 rcr_long(x86emu_t *emu, u32 d, u8 s)
{
	u32 res = d, cnt, mask, cf, ocf = 0u;
	if ((cnt = s % 33u) != 0u) {
		if (cnt == 1u) {
			cf = d & 0x1u;
			ocf = ACCESS_FLAG(F_CF) != 0;
		} else {
			cf = (d >> (cnt - 1u)) & 0x1u;
		}
		mask = (1u << (32u - cnt)) - 1u;
		res = (cnt < 32u) ? ((d >> cnt) & mask) : 0u;
		if (cnt != 1u) {
			res |= (d << (33u - cnt));
		}
		if (ACCESS_FLAG(F_CF)) {
			res |= 1u << (32u - cnt);
		}
		CONDITIONAL_SET_FLAG(cf, F_CF);
		if (cnt == 1u) {
			CONDITIONAL_SET_FLAG(XOR2(ocf + ((d >> 30u) & 0x2u)), F_OF);
		}
	}
	return res;
}

u8 rol_byte(x86emu_t *emu, u8 d, u8 s)
{
	u32 cnt, v = d;
	if (!s) return d;
	if ((cnt = s % 8u) != 0u) {
		v = ((v << cnt) + ((v >> (8u - cnt)) & ((1u << cnt) - 1u))) & 0xffu;
	}
	if (s == 1u) {
		CONDITIONAL_SET_FLAG((v + (v >> 7u)) & 1u, F_OF);
	}
	CONDITIONAL_SET_FLAG(v & 0x1u, F_CF);
	return (u8)v;
}

u16 rol_word(x86emu_t *emu, u16 d, u8 s)
{
	u32 cnt, v = d;
	if (!s) return d;
	if ((cnt = s % 16u) != 0u) {
		v = ((v << cnt) + ((v >> (16u - cnt)) & ((1u << cnt) - 1u))) & 0xffffu;
	}
	if (s == 1u) {
		CONDITIONAL_SET_FLAG((v + (v >> 15u)) & 1u, F_OF);
	}
	CONDITIONAL_SET_FLAG(v & 0x1u, F_CF);
	return (u16)v;
}

u32 rol_long(x86emu_t *emu, u32 d, u8 s)
{
	u32 cnt;
	if (!s) return d;
	if ((cnt = s % 32u) != 0u) {
		d = (d << cnt) + ((d >> (32u - cnt)) & ((1u << cnt) - 1u));
	}
	if (s == 1u) {
		CONDITIONAL_SET_FLAG((d + (d >> 31u)) & 1u, F_OF);
	}
	CONDITIONAL_SET_FLAG(d & 0x1u, F_CF);
	return d;
}

u8 ror_byte(x86emu_t *emu, u8 d, u8 s)
{
	u32 cnt, v = d;
	if (!s) return d;
	if ((cnt = s % 8u) != 0u) {
		v = ((v << (8u - cnt)) + ((v >> cnt) & ((1u << (8u - cnt)) - 1u))) & 0xffu;
	}
	if (s == 1u) {
		CONDITIONAL_SET_FLAG(XOR2(v >> 6u), F_OF);
	}
	CONDITIONAL_SET_FLAG(v & (1u << 7u), F_CF);
	return (u8)v;
}

u16 ror_word(x86emu_t *emu, u16 d, u8 s)
{
	u32 cnt, v = d;
	if (!s) return d;
	if ((cnt = s % 16u) != 0u) {
		v = ((v << (16u - cnt)) + ((v >> cnt) & ((1u << (16u - cnt)) - 1u))) & 0xffffu;
	}
	if (s == 1u) {
		CONDITIONAL_SET_FLAG(XOR2(v >> 14u), F_OF);
	}
	CONDITIONAL_SET_FLAG(v & (1u << 15u), F_CF);
	return (u16)v;
}

u32 ror_long(x86emu_t *emu, u32 d, u8 s)
{
	u32 cnt;
	if (!s) return d;
	if ((cnt = s % 32u) != 0u) {
		d = (d << (32u - cnt)) + ((d >> cnt) & ((1u << (32u - cnt)) - 1u));
	}
	if (s == 1u) {
		CONDITIONAL_SET_FLAG(XOR2(d >> 30u), F_OF);
	}
	CONDITIONAL_SET_FLAG(d & (1u << 31u), F_CF);
	return d;
}

u8 shl_byte(x86emu_t *emu, u8 d, u8 s)
{
	u32 cnt, res, cf;
	if (s < 8u) {
		cnt = s % 8u;
		if (cnt > 0u) {
			res = (u32)d << cnt;
			cf = (u32)d & (1u << (8u - cnt));
			CONDITIONAL_SET_FLAG(cf, F_CF);
			CONDITIONAL_SET_FLAG((res & 0xffu) == 0u, F_ZF);
			CONDITIONAL_SET_FLAG(res & 0x80u, F_SF);
			CONDITIONAL_SET_FLAG(PARITY(res & 0xffu), F_PF);
		} else {
			res = (u8)d;
		}
		if (cnt == 1u) {
			CONDITIONAL_SET_FLAG((((res & 0x80u) == 0x80u) ^ (ACCESS_FLAG(F_CF) != 0)), F_OF);
		} else {
			CLEAR_FLAG(F_OF);
		}
	} else {
		res = 0u;
		CONDITIONAL_SET_FLAG((s == 8u) ? (((u32)d << 7u) & 0x80u) : 0u, F_CF);
		CLEAR_FLAG(F_OF);
		CLEAR_FLAG(F_SF);
		SET_FLAG(F_PF);
		SET_FLAG(F_ZF);
	}
	return (u8)res;
}

u16 shl_word(x86emu_t *emu, u16 d, u8 s)
{
	u32 cnt, res, cf;
	if (s < 16u) {
		cnt = s % 16u;
		if (cnt > 0u) {
			res = (u32)d << cnt;
			cf = (u32)d & (1u << (16u - cnt));
			CONDITIONAL_SET_FLAG(cf, F_CF);
			CONDITIONAL_SET_FLAG((res & 0xffffu) == 0u, F_ZF);
			CONDITIONAL_SET_FLAG(res & 0x8000u, F_SF);
			CONDITIONAL_SET_FLAG(PARITY(res & 0xffu), F_PF);
		} else {
			res = (u16)d;
		}
		if (cnt == 1u) {
			CONDITIONAL_SET_FLAG((((res & 0x8000u) == 0x8000u) ^ (ACCESS_FLAG(F_CF) != 0)), F_OF);
		} else {
			CLEAR_FLAG(F_OF);
		}
	} else {
		res = 0u;
		CONDITIONAL_SET_FLAG((s == 16u) ? (((u32)d << 15u) & 0x8000u) : 0u, F_CF);
		CLEAR_FLAG(F_OF);
		CLEAR_FLAG(F_SF);
		SET_FLAG(F_PF);
		SET_FLAG(F_ZF);
	}
	return (u16)res;
}

u32 shl_long(x86emu_t *emu, u32 d, u8 s)
{
	u32 cnt, res, cf;
	if (s < 32u) {
		cnt = s % 32u;
		if (cnt > 0u) {
			res = d << cnt;
			cf = d & (1u << (32u - cnt));
			CONDITIONAL_SET_FLAG(cf, F_CF);
			CONDITIONAL_SET_FLAG((res & 0xffffffffu) == 0u, F_ZF);
			CONDITIONAL_SET_FLAG(res & 0x80000000u, F_SF);
			CONDITIONAL_SET_FLAG(PARITY(res & 0xffu), F_PF);
		} else {
			res = d;
		}
		if (cnt == 1u) {
			CONDITIONAL_SET_FLAG((((res & 0x80000000u) == 0x80000000u) ^ (ACCESS_FLAG(F_CF) != 0)), F_OF);
		} else {
			CLEAR_FLAG(F_OF);
		}
	} else {
		res = 0u;
		CONDITIONAL_SET_FLAG((s == 32u) ? (d & 1u) : 0u, F_CF);
		CLEAR_FLAG(F_OF);
		CLEAR_FLAG(F_SF);
		SET_FLAG(F_PF);
		SET_FLAG(F_ZF);
	}
	return res;
}

u8 shr_byte(x86emu_t *emu, u8 d, u8 s)
{
	u32 cnt, res, cf;
	if (s < 8u) {
		cnt = s % 8u;
		if (cnt > 0u) {
			cf = (u32)d & (1u << (cnt - 1u));
			res = (u32)d >> cnt;
			CONDITIONAL_SET_FLAG(cf, F_CF);
			CONDITIONAL_SET_FLAG((res & 0xffu) == 0u, F_ZF);
			CONDITIONAL_SET_FLAG(res & 0x80u, F_SF);
			CONDITIONAL_SET_FLAG(PARITY(res & 0xffu), F_PF);
		} else {
			res = (u8)d;
		}
		if (cnt == 1u) {
			CONDITIONAL_SET_FLAG(XOR2(res >> 6u), F_OF);
		} else {
			CLEAR_FLAG(F_OF);
		}
	} else {
		res = 0u;
		CONDITIONAL_SET_FLAG((s == 8u) ? (((u32)d >> 7u) & 0x1u) : 0u, F_CF);
		CLEAR_FLAG(F_OF);
		CLEAR_FLAG(F_SF);
		SET_FLAG(F_PF);
		SET_FLAG(F_ZF);
	}
	return (u8)res;
}

u16 shr_word(x86emu_t *emu, u16 d, u8 s)
{
	u32 cnt, res, cf;
	if (s < 16u) {
		cnt = s % 16u;
		if (cnt > 0u) {
			cf = (u32)d & (1u << (cnt - 1u));
			res = (u32)d >> cnt;
			CONDITIONAL_SET_FLAG(cf, F_CF);
			CONDITIONAL_SET_FLAG((res & 0xffffu) == 0u, F_ZF);
			CONDITIONAL_SET_FLAG(res & 0x8000u, F_SF);
			CONDITIONAL_SET_FLAG(PARITY(res & 0xffu), F_PF);
		} else {
			res = d;
		}
		if (cnt == 1u) {
			CONDITIONAL_SET_FLAG(XOR2(res >> 14u), F_OF);
		} else {
			CLEAR_FLAG(F_OF);
		}
	} else {
		res = 0u;
		CLEAR_FLAG(F_CF);
		CLEAR_FLAG(F_OF);
		SET_FLAG(F_ZF);
		CLEAR_FLAG(F_SF);
		CLEAR_FLAG(F_PF);
	}
	return (u16)res;
}

u32 shr_long(x86emu_t *emu, u32 d, u8 s)
{
	u32 cnt, res, cf;
	if (s < 32u) {
		cnt = s % 32u;
		if (cnt > 0u) {
			cf = d & (1u << (cnt - 1u));
			res = d >> cnt;
			CONDITIONAL_SET_FLAG(cf, F_CF);
			CONDITIONAL_SET_FLAG((res & 0xffffffffu) == 0u, F_ZF);
			CONDITIONAL_SET_FLAG(res & 0x80000000u, F_SF);
			CONDITIONAL_SET_FLAG(PARITY(res & 0xffu), F_PF);
		} else {
			res = d;
		}
		if (cnt == 1u) {
			CONDITIONAL_SET_FLAG(XOR2(res >> 30u), F_OF);
		} else {
			CLEAR_FLAG(F_OF);
		}
	} else {
		res = 0u;
		CLEAR_FLAG(F_CF);
		CLEAR_FLAG(F_OF);
		SET_FLAG(F_ZF);
		CLEAR_FLAG(F_SF);
		CLEAR_FLAG(F_PF);
	}
	return res;
}

u8 sar_byte(x86emu_t *emu, u8 d, u8 s)
{
	u32 cnt, res = d, cf, mask, sf = (u32)d & 0x80u;
	cnt = s % 8u;
	if (cnt > 0u && cnt < 8u) {
		mask = (1u << (8u - cnt)) - 1u;
		cf = (u32)d & (1u << (cnt - 1u));
		res = ((u32)d >> cnt) & mask;
		CONDITIONAL_SET_FLAG(cf, F_CF);
		if (sf) {
			res |= ~mask;
		}
		CONDITIONAL_SET_FLAG((res & 0xffu) == 0u, F_ZF);
		CONDITIONAL_SET_FLAG(PARITY(res & 0xffu), F_PF);
		CONDITIONAL_SET_FLAG(res & 0x80u, F_SF);
	}
	return (u8)res;
}

u16 sar_word(x86emu_t *emu, u16 d, u8 s)
{
	u32 cnt, res = d, cf, mask, sf = (u32)d & 0x8000u;
	cnt = s % 16u;
	if (cnt > 0u && cnt < 16u) {
		mask = (1u << (16u - cnt)) - 1u;
		cf = (u32)d & (1u << (cnt - 1u));
		res = ((u32)d >> cnt) & mask;
		CONDITIONAL_SET_FLAG(cf, F_CF);
		if (sf) {
			res |= ~mask;
		}
		CONDITIONAL_SET_FLAG((res & 0xffffu) == 0u, F_ZF);
		CONDITIONAL_SET_FLAG(res & 0x8000u, F_SF);
		CONDITIONAL_SET_FLAG(PARITY(res & 0xffu), F_PF);
	}
	return (u16)res;
}

u32 sar_long(x86emu_t *emu, u32 d, u8 s)
{
	u32 cnt, res = d, cf, mask, sf = d & 0x80000000u;
	cnt = s % 32u;
	if (cnt > 0u && cnt < 32u) {
		mask = (1u << (32u - cnt)) - 1u;
		cf = d & (1u << (cnt - 1u));
		res = (d >> cnt) & mask;
		CONDITIONAL_SET_FLAG(cf, F_CF);
		if (sf) {
			res |= ~mask;
		}
		CONDITIONAL_SET_FLAG((res & 0xffffffffu) == 0u, F_ZF);
		CONDITIONAL_SET_FLAG(res & 0x80000000u, F_SF);
		CONDITIONAL_SET_FLAG(PARITY(res & 0xffu), F_PF);
	}
	return res;
}

u16 shld_word (x86emu_t *emu, u16 d, u16 fill, u8 s)
{
	u32 cnt, res, cf;
	if (s < 16u) {
		cnt = s % 16u;
		if (cnt > 0u) {
			res = ((u32)d << cnt) | ((u32)fill >> (16u - cnt));
			cf = (u32)d & (1u << (16u - cnt));
			CONDITIONAL_SET_FLAG(cf, F_CF);
			CONDITIONAL_SET_FLAG((res & 0xffffu) == 0u, F_ZF);
			CONDITIONAL_SET_FLAG(res & 0x8000u, F_SF);
			CONDITIONAL_SET_FLAG(PARITY(res & 0xffu), F_PF);
		} else {
			res = d;
		}
		if (cnt == 1u) {
			CONDITIONAL_SET_FLAG((((res & 0x8000u) == 0x8000u) ^ (ACCESS_FLAG(F_CF) != 0)), F_OF);
		} else {
			CLEAR_FLAG(F_OF);
		}
	} else {
		res = 0u;
		CONDITIONAL_SET_FLAG((s == 16u) ? (((u32)d << 15u) & 0x8000u) : 0u, F_CF);
		CLEAR_FLAG(F_OF);
		CLEAR_FLAG(F_SF);
		SET_FLAG(F_PF);
		SET_FLAG(F_ZF);
	}
	return (u16)res;
}

u32 shld_long (x86emu_t *emu, u32 d, u32 fill, u8 s)
{
	u32 cnt, res, cf;
	if (s < 32u) {
		cnt = s % 32u;
		if (cnt > 0u) {
			res = (d << cnt) | (fill >> (32u - cnt));
			cf = d & (1u << (32u - cnt));
			CONDITIONAL_SET_FLAG(cf, F_CF);
			CONDITIONAL_SET_FLAG((res & 0xffffffffu) == 0u, F_ZF);
			CONDITIONAL_SET_FLAG(res & 0x80000000u, F_SF);
			CONDITIONAL_SET_FLAG(PARITY(res & 0xffu), F_PF);
		} else {
			res = d;
		}
		if (cnt == 1u) {
			CONDITIONAL_SET_FLAG((((res & 0x80000000u) == 0x80000000u) ^ (ACCESS_FLAG(F_CF) != 0)), F_OF);
		} else {
			CLEAR_FLAG(F_OF);
		}
	} else {
		res = 0u;
		CONDITIONAL_SET_FLAG((s == 32u) ? (d & 1u) : 0u, F_CF);
		CLEAR_FLAG(F_OF);
		CLEAR_FLAG(F_SF);
		SET_FLAG(F_PF);
		SET_FLAG(F_ZF);
	}
	return res;
}

u16 shrd_word (x86emu_t *emu, u16 d, u16 fill, u8 s)
{
	u32 cnt, res, cf;
	if (s < 16u) {
		cnt = s % 16u;
		if (cnt > 0u) {
			cf = (u32)d & (1u << (cnt - 1u));
			res = ((u32)d >> cnt) | ((u32)fill << (16u - cnt));
			CONDITIONAL_SET_FLAG(cf, F_CF);
			CONDITIONAL_SET_FLAG((res & 0xffffu) == 0u, F_ZF);
			CONDITIONAL_SET_FLAG(res & 0x8000u, F_SF);
			CONDITIONAL_SET_FLAG(PARITY(res & 0xffu), F_PF);
		} else {
			res = d;
		}
		if (cnt == 1u) {
			CONDITIONAL_SET_FLAG(XOR2(res >> 14u), F_OF);
		} else {
			CLEAR_FLAG(F_OF);
		}
	} else {
		res = 0u;
		CLEAR_FLAG(F_CF);
		CLEAR_FLAG(F_OF);
		SET_FLAG(F_ZF);
		CLEAR_FLAG(F_SF);
		CLEAR_FLAG(F_PF);
	}
	return (u16)res;
}

u32 shrd_long (x86emu_t *emu, u32 d, u32 fill, u8 s)
{
	u32 cnt, res, cf;
	if (s < 32u) {
		cnt = s % 32u;
		if (cnt > 0u) {
			cf = d & (1u << (cnt - 1u));
			res = (d >> cnt) | (fill << (32u - cnt));
			CONDITIONAL_SET_FLAG(cf, F_CF);
			CONDITIONAL_SET_FLAG((res & 0xffffffffu) == 0u, F_ZF);
			CONDITIONAL_SET_FLAG(res & 0x80000000u, F_SF);
			CONDITIONAL_SET_FLAG(PARITY(res & 0xffu), F_PF);
		} else {
			res = d;
		}
		if (cnt == 1u) {
			CONDITIONAL_SET_FLAG(XOR2(res >> 30u), F_OF);
		} else {
			CLEAR_FLAG(F_OF);
		}
	} else {
		res = 0u;
		CLEAR_FLAG(F_CF);
		CLEAR_FLAG(F_OF);
		SET_FLAG(F_ZF);
		CLEAR_FLAG(F_SF);
		CLEAR_FLAG(F_PF);
	}
	return res;
}

"""
prim = must_replace(prim, old_shifts, new_shifts, "prim_ops.c shift/rotate functions")
open(prim_path, "w", encoding="utf-8").write(prim)

# Patch ops2.c: 1 << (... & 0x1f) -> 1u << (... & 0x1f) in BT/BTS/BTR/BTC
ops2_path = os.path.join(src, "ops2.c")
ops2 = open(ops2_path, "r", encoding="utf-8").read()
assert "mask = 1 << (" in ops2, "ops2.c mask shift pattern not found"
ops2 = ops2.replace("mask = 1 << (", "mask = 1u << (")
open(ops2_path, "w", encoding="utf-8").write(ops2)

# Patch decode.c: SIB negative signed index << scale
dec_path = os.path.join(src, "decode.c")
dec = open(dec_path, "r", encoding="utf-8").read()
dec = must_replace(
    dec,
    "return base + (i << scale);",
    "return base + (int)((u32)i << scale);",
    "decode.c decode_sib",
)
open(dec_path, "w", encoding="utf-8").write(dec)
PYEOF

  # Upstream's Makefile only produces a shared object; compile the same
  # translation units with our configured C compiler/LTO flags and archive them.
  local obj_dir="${WORK_DIR}/x86emu_obj"
  mkdir -p "${obj_dir}"
  local cc_flags=()
  local arr=()
  if [[ -n "${COMMON_CFLAGS}" ]]; then
    read -r -a arr <<< "${COMMON_CFLAGS}"
    cc_flags+=("${arr[@]}")
  fi
  if [[ -n "${X86EMU_EXTRA_CFLAGS}" ]]; then
    read -r -a arr <<< "${X86EMU_EXTRA_CFLAGS}"
    cc_flags+=("${arr[@]}")
  fi
  if [[ -n "${EXTRA_CFLAGS}" ]]; then
    read -r -a arr <<< "${EXTRA_CFLAGS}"
    cc_flags+=("${arr[@]}")
  fi
  if [[ "${TARGET_OS}" != "win64" ]]; then
    cc_flags+=("-fvisibility=hidden")
  fi
  cc_flags+=("-DSTRICT_ALIGN=1" "-fomit-frame-pointer" "-Wall" "-w" "-I${src}/include")

  shopt -s nullglob
  local c_files=("${src}"/*.c)
  shopt -u nullglob
  (( ${#c_files[@]} > 0 )) || die "no .c files found in ${src}"

  local c
  for c in "${c_files[@]}"; do
    "${CC_BIN}" -c "${cc_flags[@]}" "${c}" -o "${obj_dir}/$(basename "${c%.c}").o"
  done

  shopt -s nullglob
  local o_files=("${obj_dir}"/*.o)
  shopt -u nullglob
  (( ${#o_files[@]} > 0 )) || die "no .o files produced in ${obj_dir}"

  if [[ -n "${OUTPUT_LIB}" ]]; then
    mkdir -p "$(dirname "${OUTPUT_LIB}")"
    rm -f "${OUTPUT_LIB}"
    "${AR_BIN}" rcs "${OUTPUT_LIB}" "${o_files[@]}"
    "${RANLIB_BIN}" "${OUTPUT_LIB}"
    return
  fi
  mkdir -p "${PREFIX}/lib" "${PREFIX}/include"
  rm -f "${PREFIX}/lib/libx86emu.a"
  "${AR_BIN}" rcs "${PREFIX}/lib/libx86emu.a" "${o_files[@]}"
  "${RANLIB_BIN}" "${PREFIX}/lib/libx86emu.a"
  install -m 644 "${src}/include/x86emu.h" "${PREFIX}/include/x86emu.h"
  install_license libx86emu "${src}/LICENSE"
  if [[ -z "${EXTRA_CFLAGS}" ]]; then
    local id="${X86EMU_VERSION} ${X86EMU_SHA256} r${RECIPE_REV}${TOOLCHAIN_STAMP}"
    stamp_set x86emu "${id}"
  fi
}

# Populates ${WORK_DIR}/sysroot with -dev packages plus the runtime packages
# they depend on (needed so SDL's CMake can resolve each library's soname).
prepare_sysroot() {
  local sysroot="${WORK_DIR}/sysroot" debs="${WORK_DIR}/debs"
  mkdir -p "${sysroot}" "${debs}"
  command -v apt-get >/dev/null && command -v dpkg-deb >/dev/null ||
    die "apt-get/dpkg-deb are required to fetch compile-time SDL headers"
  log "Fetching compile-time headers into a temporary sysroot"
  local pkg
  for pkg in "${SYSROOT_DEV_PACKAGES[@]}"; do
    (cd "${debs}" && apt-get download "${pkg}" >/dev/null 2>&1) ||
      log "  package ${pkg} unavailable; its SDL backend will be disabled"
  done
  shopt -s nullglob
  local deb_files=("${debs}"/*.deb)
  shopt -u nullglob
  (( ${#deb_files[@]} > 0 )) || die "no .deb packages could be downloaded into ${debs}"

  local runtime_pkgs=()
  mapfile -t runtime_pkgs < <(
    for deb in "${deb_files[@]}"; do
      [[ -f "${deb}" ]] && dpkg-deb -f "${deb}" Depends
    done |
    tr ',' '\n' | sed -E 's/\|.*//; s/\(.*\)//; s/:any//; s/^[[:space:]]+//; s/[[:space:]]+$//' |
    awk '/^lib/ && !/-dev$/ && !/^libc6/' | sort -u
  )
  for pkg in "${runtime_pkgs[@]}"; do
    [[ -n "${pkg}" ]] || continue
    (cd "${debs}" && apt-get download "${pkg}" >/dev/null 2>&1) || true
  done
  shopt -s nullglob
  deb_files=("${debs}"/*.deb)
  shopt -u nullglob
  local deb
  for deb in "${deb_files[@]}"; do
    [[ -f "${deb}" ]] && dpkg-deb -x "${deb}" "${sysroot}"
  done
  # Re-root pkg-config metadata so it points into the sysroot.
  local pc
  while IFS= read -r -d '' pc; do
    sed -i -E "s#^([A-Za-z_]+)=/usr#\\1=${sysroot}/usr#" "${pc}"
  done < <(python3 - "${sysroot}" << 'PYEOF'
import os, sys
for d, _, fs in os.walk(sys.argv[1]):
    for f in fs:
        if f.endswith(".pc"):
            sys.stdout.write(os.path.join(d, f) + "\0")
PYEOF
)
}

build_sdl2() {
  local id="${SDL2_VERSION} ${SDL2_SHA256} r${RECIPE_REV}${TOOLCHAIN_STAMP}"
  stamp_ok sdl2 "${id}" && { log "SDL2 ${SDL2_VERSION} up to date"; return; }
  fetch "${SDL2_URL}" "${SDL2_SHA256}" "SDL2-${SDL2_VERSION}.tar.gz"
  local src
  if [[ "${TARGET_OS}" == "win64" ]]; then
    src="$(unpack "SDL2-${SDL2_VERSION}.tar.gz")"
    log "Building SDL2 ${SDL2_VERSION} (win64 static: Direct3D 11/12, OpenGL, WASAPI, DirectSound, WinMM)"
    cmake -S "${src}" -B "${WORK_DIR}/sdl2_build" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
      -DCMAKE_INSTALL_LIBDIR=lib \
      "${WIN64_CMAKE_COMMON_ARGS[@]}" \
      -DSDL_SHARED=OFF -DSDL_STATIC=ON \
      -DSDL_TEST=OFF -DSDL_TESTS=OFF \
      -DSDL_LIBC=ON \
      > "${WORK_DIR}/sdl2_configure.log" 2>&1 ||
      { cat "${WORK_DIR}/sdl2_configure.log" >&2; die "SDL2 win64 configure failed"; }
    cmake --build "${WORK_DIR}/sdl2_build" -j"${JOBS}" > "${WORK_DIR}/sdl2_build.log" 2>&1 ||
      { tail -50 "${WORK_DIR}/sdl2_build.log" >&2; die "SDL2 win64 build failed"; }
    cmake --install "${WORK_DIR}/sdl2_build" >/dev/null
    local cfg="${PREFIX}/include/SDL2/SDL_config.h"
    python3 - "${cfg}" << 'PYEOF' || die "SDL2 win64 was built without Windows/D3D11/WASAPI support"
import re, sys
txt = open(sys.argv[1]).read()
for sym in ("SDL_VIDEO_DRIVER_WINDOWS", "SDL_VIDEO_RENDER_D3D11", "SDL_AUDIO_DRIVER_WASAPI"):
    if not re.search(rf"#define\s+{sym}\s+1\b", txt):
        sys.exit(1)
sys.exit(0)
PYEOF
    install_license SDL2 "${src}/LICENSE.txt"
    stamp_set sdl2 "${id}"
    return
  fi

  prepare_sysroot
  local sysroot="${WORK_DIR}/sysroot"
  src="$(unpack "SDL2-${SDL2_VERSION}.tar.gz")"
  log "Building SDL2 ${SDL2_VERSION} (static, dlopen backends)"
  local pc_path="${sysroot}/usr/lib/${MULTIARCH}/pkgconfig:${sysroot}/usr/share/pkgconfig"
  # PipeWire >= 1.4 headers type pw_node_enum_params() on struct pw_node while
  # SDL 2.30 passes the (layout-identical) struct pw_proxy; GCC 14+ and Clang 16+
  # promote that mismatch to an error by default.
  local sdl_cflags="${COMMON_CFLAGS} -Wno-error=incompatible-pointer-types"
  local sdl_cmake_extra=(
    "-DCMAKE_C_COMPILER=${CC_BIN}"
    "-DCMAKE_CXX_COMPILER=${CXX_BIN}"
    "-DCMAKE_AR=${AR_BIN}"
    "-DCMAKE_RANLIB=${RANLIB_BIN}"
    "-DCMAKE_C_COMPILER_AR=${AR_BIN}"
    "-DCMAKE_C_COMPILER_RANLIB=${RANLIB_BIN}"
    "-DCMAKE_CXX_COMPILER_AR=${AR_BIN}"
    "-DCMAKE_CXX_COMPILER_RANLIB=${RANLIB_BIN}"
  )
  if [[ -n "${NM_BIN}" ]]; then
    sdl_cmake_extra+=("-DCMAKE_NM=${NM_BIN}")
  fi
  if [[ -n "${COMMON_LDFLAGS}" ]]; then
    sdl_cmake_extra+=(
      "-DCMAKE_EXE_LINKER_FLAGS=${COMMON_LDFLAGS}"
      "-DCMAKE_SHARED_LINKER_FLAGS=${COMMON_LDFLAGS}"
    )
  fi
  PATH="${sysroot}/usr/bin:${PATH}" \
  PKG_CONFIG_PATH="${pc_path}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}" \
  CFLAGS="${sdl_cflags} -I${sysroot}/usr/include -I${sysroot}/usr/include/${MULTIARCH}" \
    cmake -S "${src}" -B "${WORK_DIR}/sdl2_build" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
      -DCMAKE_INSTALL_LIBDIR=lib \
      -DCMAKE_PREFIX_PATH="${sysroot}/usr" \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      "${sdl_cmake_extra[@]}" \
      -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_STATIC_PIC=ON \
      -DSDL_TEST=OFF -DSDL_TESTS=OFF -DSDL_RPATH=OFF \
      -DSDL_X11=ON -DSDL_X11_SHARED=ON \
      -DSDL_WAYLAND=ON -DSDL_WAYLAND_SHARED=ON \
      -DSDL_WAYLAND_LIBDECOR=ON -DSDL_WAYLAND_LIBDECOR_SHARED=ON \
      -DSDL_WAYLAND_QT_TOUCH=OFF \
      -DSDL_KMSDRM=ON -DSDL_KMSDRM_SHARED=ON \
      -DSDL_OPENGL=ON -DSDL_OPENGLES=ON -DSDL_VULKAN=ON \
      -DSDL_PULSEAUDIO=ON -DSDL_PULSEAUDIO_SHARED=ON \
      -DSDL_PIPEWIRE=ON -DSDL_PIPEWIRE_SHARED=ON \
      -DSDL_ALSA=ON -DSDL_ALSA_SHARED=ON \
      -DSDL_JACK=OFF -DSDL_ESD=OFF -DSDL_ARTS=OFF -DSDL_NAS=OFF \
      -DSDL_SNDIO=OFF -DSDL_FUSIONSOUND=OFF -DSDL_LIBSAMPLERATE=OFF \
      -DSDL_DIRECTFB=OFF -DSDL_RPI=OFF -DSDL_VIVANTE=OFF \
      -DSDL_IBUS=OFF -DSDL_DBUS=ON -DSDL_LIBUDEV=ON -DSDL_HIDAPI_LIBUSB=OFF \
      > "${WORK_DIR}/sdl2_configure.log" 2>&1 ||
    { cat "${WORK_DIR}/sdl2_configure.log" >&2; die "SDL2 configure failed"; }
  cmake --build "${WORK_DIR}/sdl2_build" -j"${JOBS}" > "${WORK_DIR}/sdl2_build.log" 2>&1 ||
    { tail -50 "${WORK_DIR}/sdl2_build.log" >&2; die "SDL2 build failed"; }
  cmake --install "${WORK_DIR}/sdl2_build" >/dev/null

  # Every enabled platform backend must be runtime-loaded, never linked.
  local cfg="${PREFIX}/include/SDL2/SDL_config.h"
  python3 - "${cfg}" << 'PYEOF' || die "SDL2 was built without dynamically loaded X11 support"
import re, sys
sys.exit(0 if re.search(r'SDL_VIDEO_DRIVER_X11_DYNAMIC "libX11\.so\.\d+"', open(sys.argv[1]).read()) else 1)
PYEOF
  log "SDL2 dynamic backends:"
  python3 - "${cfg}" << 'PYEOF'
import re, sys
for name, lib in re.findall(r'#define (SDL_\w+_DYNAMIC\w*) "([^"]+)"', open(sys.argv[1]).read()):
    print(f"[static-deps]   {name} -> {lib}")
PYEOF
  install_license SDL2 "${src}/LICENSE.txt"
  stamp_set sdl2 "${id}"
}

if (( ONLY_X86EMU )); then
  build_x86emu
  exit 0
fi

build_zlib
build_libpng
build_x86emu
build_sdl2

log "Static dependencies ready in ${PREFIX}"
