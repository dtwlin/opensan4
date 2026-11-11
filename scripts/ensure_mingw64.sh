#!/usr/bin/env bash
set -euo pipefail

# Locates (or rootlessly provisions into third_party/mingw64/) the MinGW-w64
# x86_64-w64-mingw32 sysroot headers, CRT objects, static C++/POSIX-thread
# runtimes (libstdc++.a, libgcc.a, libgcc_eh.a, libwinpthread.a), and optional
# MinGW GCC + binutils cross-compilers (--mode gcc).
#
# Prints shell-evaluatable variable assignments on stdout:
#   MINGW_SYSROOT_USR=...
#   MINGW_TRIPLE=x86_64-w64-mingw32
#   MINGW_GCC_LIBDIR=...
#   MINGW_CXX_INCDIR=...
#   MINGW_CC_BIN=...       (when --mode gcc)
#   MINGW_CXX_BIN=...      (when --mode gcc)
#   MINGW_AR_BIN=...       (when --mode gcc)
#   MINGW_RANLIB_BIN=...   (when --mode gcc)
#   MINGW_WINDRES_BIN=...  (when --mode gcc)
#   MINGW_STRIP_BIN=...    (when --mode gcc)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DOWNLOAD_DIR="${SAN4_MINGW_DOWNLOAD_DIR:-${ROOT_DIR}/third_party/downloads}"
LOCAL_MINGW_ROOT="${ROOT_DIR}/third_party/mingw64"

MODE="clang"
SYSROOT_ARG="${SAN4_MINGW_SYSROOT:-}"

usage() {
  cat << 'EOF' >&2
Usage: ensure_mingw64.sh [--mode clang|gcc] [--sysroot <DIR>]

Locates or rootlessly unpacks the x86_64-w64-mingw32 sysroot (Win32 headers,
import libraries, libwinpthread.a, libstdc++.a, libgcc.a, crtbegin.o, plus
x86_64-w64-mingw32-gcc/g++/binutils when --mode gcc is requested).
EOF
}

log() { echo "[ensure-mingw64] $*" >&2; }
die() { echo "[ensure-mingw64] ERROR: $*" >&2; exit 1; }

while (( $# > 0 )); do
  case "$1" in
    --mode)
      [[ $# -ge 2 ]] || die "--mode requires clang or gcc"
      MODE="$2"
      shift 2
      ;;
    --mode=*)
      MODE="${1#*=}"
      shift
      ;;
    --sysroot)
      [[ $# -ge 2 ]] || die "--sysroot requires a directory argument"
      SYSROOT_ARG="$2"
      shift 2
      ;;
    --sysroot=*)
      SYSROOT_ARG="${1#*=}"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown argument '$1'"
      ;;
  esac
done

if [[ "${MODE}" != "clang" && "${MODE}" != "gcc" ]]; then
  die "invalid --mode '${MODE}' (expected clang or gcc)"
fi

MINGW_TRIPLE="x86_64-w64-mingw32"
MINGW_SYSROOT_USR=""
MINGW_GCC_LIBDIR=""
MINGW_CXX_INCDIR=""
MINGW_CC_BIN=""
MINGW_CXX_BIN=""
MINGW_AR_BIN=""
MINGW_RANLIB_BIN=""
MINGW_WINDRES_BIN=""
MINGW_STRIP_BIN=""

find_gcc_libdir() {
  local usr_dir="$1"
  shopt -s nullglob
  local cands=(
    "${usr_dir}/lib/gcc/${MINGW_TRIPLE}/"*-posix
    "${usr_dir}/lib/gcc/${MINGW_TRIPLE}/"*
  )
  shopt -u nullglob
  local d
  for d in "${cands[@]}"; do
    if [[ -f "${d}/libstdc++.a" && -f "${d}/libgcc.a" && -f "${d}/crtbegin.o" ]]; then
      echo "${d}"
      return 0
    fi
  done
  if [[ -f "${usr_dir}/lib/libstdc++.a" && -f "${usr_dir}/lib/libgcc.a" ]]; then
    echo "${usr_dir}/lib"
    return 0
  fi
  return 1
}

check_sysroot() {
  local cand="$1"
  [[ -n "${cand}" && -d "${cand}" ]] || return 1
  local usr_dir="${cand}"
  if [[ -d "${cand}/usr/${MINGW_TRIPLE}" ]]; then
    usr_dir="${cand}/usr"
  fi
  [[ -f "${usr_dir}/${MINGW_TRIPLE}/include/windows.h" &&
     -f "${usr_dir}/${MINGW_TRIPLE}/include/winver.h" &&
     -f "${usr_dir}/${MINGW_TRIPLE}/lib/libwinpthread.a" &&
     -f "${usr_dir}/${MINGW_TRIPLE}/lib/libkernel32.a" &&
     -f "${usr_dir}/${MINGW_TRIPLE}/lib/libdinput8.a" ]] || return 1

  local gcc_libdir=""
  gcc_libdir="$(find_gcc_libdir "${usr_dir}")" || return 1
  local gcc_ver_name
  gcc_ver_name="$(basename "${gcc_libdir}")"
  local cxx_inc="${gcc_libdir}/include/c++"
  if [[ ! -d "${cxx_inc}" ]]; then
    cxx_inc="${usr_dir}/include/c++/${gcc_ver_name}"
  fi
  if [[ ! -d "${cxx_inc}" ]]; then
    shopt -s nullglob
    local inc_cands=("${usr_dir}/include/c++/"*)
    shopt -u nullglob
    (( ${#inc_cands[@]} > 0 )) || return 1
    cxx_inc="${inc_cands[0]}"
  fi
  [[ -f "${cxx_inc}/filesystem" ]] || return 1

  if [[ "${MODE}" == "gcc" ]]; then
    local cc_bin="" cxx_bin="" ar_bin="" ranlib_bin="" rc_bin="" strip_bin=""
    for b in "${usr_dir}/bin/${MINGW_TRIPLE}-gcc-posix" "${usr_dir}/bin/${MINGW_TRIPLE}-gcc" "$(command -v "${MINGW_TRIPLE}-gcc-posix" 2>/dev/null || true)" "$(command -v "${MINGW_TRIPLE}-gcc" 2>/dev/null || true)"; do
      if [[ -n "${b}" && -x "${b}" ]]; then cc_bin="${b}"; break; fi
    done
    for b in "${usr_dir}/bin/${MINGW_TRIPLE}-g++-posix" "${usr_dir}/bin/${MINGW_TRIPLE}-g++" "$(command -v "${MINGW_TRIPLE}-g++-posix" 2>/dev/null || true)" "$(command -v "${MINGW_TRIPLE}-g++" 2>/dev/null || true)"; do
      if [[ -n "${b}" && -x "${b}" ]]; then cxx_bin="${b}"; break; fi
    done
    for b in "${usr_dir}/bin/${MINGW_TRIPLE}-ar" "$(command -v "${MINGW_TRIPLE}-ar" 2>/dev/null || true)"; do
      if [[ -n "${b}" && -x "${b}" ]]; then ar_bin="${b}"; break; fi
    done
    for b in "${usr_dir}/bin/${MINGW_TRIPLE}-ranlib" "$(command -v "${MINGW_TRIPLE}-ranlib" 2>/dev/null || true)"; do
      if [[ -n "${b}" && -x "${b}" ]]; then ranlib_bin="${b}"; break; fi
    done
    for b in "${usr_dir}/bin/${MINGW_TRIPLE}-windres" "$(command -v "${MINGW_TRIPLE}-windres" 2>/dev/null || true)"; do
      if [[ -n "${b}" && -x "${b}" ]]; then rc_bin="${b}"; break; fi
    done
    for b in "${usr_dir}/bin/${MINGW_TRIPLE}-strip" "$(command -v "${MINGW_TRIPLE}-strip" 2>/dev/null || true)"; do
      if [[ -n "${b}" && -x "${b}" ]]; then strip_bin="${b}"; break; fi
    done
    [[ -n "${cc_bin}" && -n "${cxx_bin}" && -n "${ar_bin}" && -n "${ranlib_bin}" && -n "${rc_bin}" ]] || return 1
    MINGW_CC_BIN="${cc_bin}"
    MINGW_CXX_BIN="${cxx_bin}"
    MINGW_AR_BIN="${ar_bin}"
    MINGW_RANLIB_BIN="${ranlib_bin}"
    MINGW_WINDRES_BIN="${rc_bin}"
    MINGW_STRIP_BIN="${strip_bin}"
  fi

  MINGW_SYSROOT_USR="${usr_dir}"
  MINGW_GCC_LIBDIR="${gcc_libdir}"
  MINGW_CXX_INCDIR="${cxx_inc}"
  return 0
}

try_resolve() {
  local cands=()
  if [[ -n "${SYSROOT_ARG}" ]]; then
    cands+=("${SYSROOT_ARG}")
  fi
  cands+=("/usr" "/mingw64" "${LOCAL_MINGW_ROOT}/usr")
  local c
  for c in "${cands[@]}"; do
    if check_sysroot "${c}"; then
      return 0
    fi
  done
  return 1
}

if ! try_resolve; then
  if command -v apt-get >/dev/null 2>&1 && command -v dpkg-deb >/dev/null 2>&1; then
    mkdir -p "${DOWNLOAD_DIR}" "${LOCAL_MINGW_ROOT}"
    req_pkgs=(
      mingw-w64-common
      mingw-w64-x86-64-dev
      gcc-mingw-w64-x86-64-posix
      g++-mingw-w64-x86-64-posix
    )
    opt_pkgs=()
    if [[ "${MODE}" == "gcc" ]]; then
      req_pkgs+=(binutils-mingw-w64-x86-64)
      opt_pkgs+=(gcc-mingw-w64-base binutils-mingw-w64-base)
    fi
    shopt -s nullglob
    for pkg in "${req_pkgs[@]}"; do
      cached_debs=("${DOWNLOAD_DIR}/${pkg}_"*.deb)
      if (( ${#cached_debs[@]} == 0 )); then
        log "Downloading ${pkg} into ${DOWNLOAD_DIR}"
        (cd "${DOWNLOAD_DIR}" && apt-get download "${pkg}" >&2) ||
          die "failed to download ${pkg} via apt-get download"
        cached_debs=("${DOWNLOAD_DIR}/${pkg}_"*.deb)
      else
        log "Reusing cached $(basename "${cached_debs[0]}")"
      fi
      (( ${#cached_debs[@]} > 0 )) || die "no .deb archive found for ${pkg} in ${DOWNLOAD_DIR}"
      dpkg-deb -x "${cached_debs[0]}" "${LOCAL_MINGW_ROOT}"
    done
    for pkg in "${opt_pkgs[@]}"; do
      cached_debs=("${DOWNLOAD_DIR}/${pkg}_"*.deb)
      if (( ${#cached_debs[@]} == 0 )); then
        (cd "${DOWNLOAD_DIR}" && apt-get download "${pkg}" >&2) || true
        cached_debs=("${DOWNLOAD_DIR}/${pkg}_"*.deb)
      fi
      if (( ${#cached_debs[@]} > 0 )); then
        dpkg-deb -x "${cached_debs[0]}" "${LOCAL_MINGW_ROOT}"
      fi
    done
    shopt -u nullglob
    if [[ -d "${LOCAL_MINGW_ROOT}/usr/bin" ]]; then
      if [[ -x "${LOCAL_MINGW_ROOT}/usr/bin/${MINGW_TRIPLE}-gcc-posix" && ! -e "${LOCAL_MINGW_ROOT}/usr/bin/${MINGW_TRIPLE}-gcc" ]]; then
        ln -sf "${MINGW_TRIPLE}-gcc-posix" "${LOCAL_MINGW_ROOT}/usr/bin/${MINGW_TRIPLE}-gcc"
      fi
      if [[ -x "${LOCAL_MINGW_ROOT}/usr/bin/${MINGW_TRIPLE}-g++-posix" && ! -e "${LOCAL_MINGW_ROOT}/usr/bin/${MINGW_TRIPLE}-g++" ]]; then
        ln -sf "${MINGW_TRIPLE}-g++-posix" "${LOCAL_MINGW_ROOT}/usr/bin/${MINGW_TRIPLE}-g++"
      fi
    fi
  fi
  try_resolve || die "MinGW-w64 x86_64-w64-mingw32 sysroot not found or incomplete (install mingw-w64)"
fi

printf 'MINGW_SYSROOT_USR=%q\n' "${MINGW_SYSROOT_USR}"
printf 'MINGW_TRIPLE=%q\n' "${MINGW_TRIPLE}"
printf 'MINGW_GCC_LIBDIR=%q\n' "${MINGW_GCC_LIBDIR}"
printf 'MINGW_CXX_INCDIR=%q\n' "${MINGW_CXX_INCDIR}"
if [[ "${MODE}" == "gcc" ]]; then
  printf 'MINGW_CC_BIN=%q\n' "${MINGW_CC_BIN}"
  printf 'MINGW_CXX_BIN=%q\n' "${MINGW_CXX_BIN}"
  printf 'MINGW_AR_BIN=%q\n' "${MINGW_AR_BIN}"
  printf 'MINGW_RANLIB_BIN=%q\n' "${MINGW_RANLIB_BIN}"
  printf 'MINGW_WINDRES_BIN=%q\n' "${MINGW_WINDRES_BIN}"
  printf 'MINGW_STRIP_BIN=%q\n' "${MINGW_STRIP_BIN}"
fi
