#!/usr/bin/env bash
set -euo pipefail

# Locates (or rootlessly provisions into third_party/bolt-<major>/) the LLVM
# BOLT post-link optimization tools matching the active Clang toolchain:
#   llvm-bolt, merge-fdata, perf2bolt, libbolt_rt_instr.a, and llvm-strip.
#
# Prints shell-evaluatable variable assignments on stdout:
#   BOLT_BIN=...
#   MERGE_FDATA_BIN=...
#   PERF2BOLT_BIN=...
#   BOLT_RT_INSTR_LIB=...
#   LLVM_STRIP_BIN=...

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DOWNLOAD_DIR="${SAN4_BOLT_DOWNLOAD_DIR:-${ROOT_DIR}/third_party/downloads}"

CLANG_ARG=""

usage() {
  cat << 'EOF' >&2
Usage: ensure_bolt.sh [--clang <compiler_path_or_major>]

Locates matching llvm-bolt, merge-fdata, perf2bolt, libbolt_rt_instr.a, and
llvm-strip for the given Clang toolchain. When absent on Debian/Ubuntu hosts,
rootlessly downloads and extracts bolt-<major> and libbolt-<major>-dev into
third_party/bolt-<major>/ (reusing cached .deb archives in third_party/downloads/).
EOF
}

log() { echo "[ensure-bolt] $*" >&2; }
die() { echo "[ensure-bolt] ERROR: $*" >&2; exit 1; }

while (( $# > 0 )); do
  case "$1" in
    --clang)
      [[ $# -ge 2 ]] || die "--clang requires an argument"
      CLANG_ARG="$2"
      shift 2
      ;;
    --clang=*)
      CLANG_ARG="${1#*=}"
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

LLVM_BINDIR=""
CLANG_MAJOR=""

if [[ -z "${CLANG_ARG}" ]]; then
  for sfx in "" -23 -22 -21 -20 -19 -18 -17 -16 -15; do
    if command -v "clang++${sfx}" >/dev/null 2>&1; then
      CLANG_ARG="$(command -v "clang++${sfx}")"
      break
    elif command -v "clang${sfx}" >/dev/null 2>&1; then
      CLANG_ARG="$(command -v "clang${sfx}")"
      break
    fi
  done
  [[ -n "${CLANG_ARG}" ]] || die "no clang compiler found; pass --clang <path_or_major>"
fi

if [[ "${CLANG_ARG}" =~ ^[0-9]+$ ]]; then
  CLANG_MAJOR="${CLANG_ARG}"
  if [[ -d "/usr/lib/llvm-${CLANG_MAJOR}/bin" ]]; then
    LLVM_BINDIR="/usr/lib/llvm-${CLANG_MAJOR}/bin"
  elif command -v "clang-${CLANG_MAJOR}" >/dev/null 2>&1; then
    CLANG_REAL="$(readlink -f "$(command -v "clang-${CLANG_MAJOR}")")"
    LLVM_BINDIR="$(dirname "${CLANG_REAL}")"
  fi
else
  CLANG_PATH=""
  if [[ -x "${CLANG_ARG}" ]]; then
    CLANG_PATH="${CLANG_ARG}"
  elif command -v "${CLANG_ARG}" >/dev/null 2>&1; then
    CLANG_PATH="$(command -v "${CLANG_ARG}")"
  else
    die "compiler '${CLANG_ARG}' not found or not executable"
  fi
  CLANG_REAL="$(readlink -f "${CLANG_PATH}")"
  LLVM_BINDIR="$(dirname "${CLANG_REAL}")"
  CLANG_VER="$("${CLANG_PATH}" -dumpversion 2>/dev/null || true)"
  CLANG_MAJOR="${CLANG_VER%%.*}"
  if [[ ! "${CLANG_MAJOR}" =~ ^[0-9]+$ ]]; then
    die "unable to determine Clang major version from '${CLANG_PATH}' (got '${CLANG_VER}')"
  fi
fi

LOCAL_BOLT_DIR="${ROOT_DIR}/third_party/bolt-${CLANG_MAJOR}"
LOCAL_LLVM_PREFIX="${LOCAL_BOLT_DIR}/usr/lib/llvm-${CLANG_MAJOR}"

# Preserve symlink basename (perf2bolt -> llvm-bolt, llvm-strip -> llvm-objcopy
# are argv[0]-dispatched multicall binaries).
abs_keep_name() {
  local p="$1"
  echo "$(cd "$(dirname "${p}")" && pwd)/$(basename "${p}")"
}

find_bolt_rt_lib() {
  local prefix="$1"
  [[ -n "${prefix}" && -d "${prefix}" ]] || return 1
  python3 - "${prefix}" << 'PYEOF'
import os, sys
root = sys.argv[1]
for dirpath, _, filenames in os.walk(root):
    if "libbolt_rt_instr.a" in filenames:
        print(os.path.join(dirpath, "libbolt_rt_instr.a"))
        sys.exit(0)
sys.exit(1)
PYEOF
}

resolve_bolt_suite() {
  BOLT_BIN=""
  MERGE_FDATA_BIN=""
  PERF2BOLT_BIN=""
  BOLT_RT_INSTR_LIB=""

  local cand_bins=()
  if [[ -n "${LLVM_BINDIR}" ]]; then
    cand_bins+=("${LLVM_BINDIR}/llvm-bolt")
  fi
  if command -v "llvm-bolt-${CLANG_MAJOR}" >/dev/null 2>&1; then
    cand_bins+=("$(command -v "llvm-bolt-${CLANG_MAJOR}")")
  fi
  if command -v "llvm-bolt" >/dev/null 2>&1; then
    local unv
    unv="$(command -v "llvm-bolt")"
    if "${unv}" --version 2>/dev/null | python3 - "${CLANG_MAJOR}" -c '
import sys
maj = sys.argv[1]
txt = sys.stdin.read()
sys.exit(0 if (f"LLVM version {maj}." in txt or f"version {maj}." in txt) else 1)
'; then
      cand_bins+=("${unv}")
    fi
  fi
  cand_bins+=("${LOCAL_LLVM_PREFIX}/bin/llvm-bolt")
  cand_bins+=("${LOCAL_BOLT_DIR}/usr/bin/llvm-bolt-${CLANG_MAJOR}")

  local cand real_bin bindir prefix
  for cand in "${cand_bins[@]}"; do
    if [[ -x "${cand}" ]]; then
      real_bin="$(readlink -f "${cand}")"
      bindir="$(dirname "${real_bin}")"
      prefix="$(dirname "${bindir}")"
      local mf="" pf=""
      for m_cand in "${bindir}/merge-fdata" "$(command -v "merge-fdata-${CLANG_MAJOR}" 2>/dev/null || true)" "${LOCAL_LLVM_PREFIX}/bin/merge-fdata" "$(command -v merge-fdata 2>/dev/null || true)"; do
        if [[ -n "${m_cand}" && -x "${m_cand}" ]]; then
          mf="${m_cand}"
          break
        fi
      done
      for p_cand in "${bindir}/perf2bolt" "$(command -v "perf2bolt-${CLANG_MAJOR}" 2>/dev/null || true)" "${LOCAL_LLVM_PREFIX}/bin/perf2bolt" "$(command -v perf2bolt 2>/dev/null || true)"; do
        if [[ -n "${p_cand}" && -x "${p_cand}" ]]; then
          pf="${p_cand}"
          break
        fi
      done
      if [[ -n "${mf}" && -n "${pf}" ]]; then
        BOLT_BIN="$(abs_keep_name "${cand}")"
        MERGE_FDATA_BIN="$(abs_keep_name "${mf}")"
        PERF2BOLT_BIN="$(abs_keep_name "${pf}")"
        break
      fi
    fi
  done

  local rt_prefixes=()
  if [[ -n "${BOLT_BIN}" ]]; then
    rt_prefixes+=("$(dirname "$(dirname "${BOLT_BIN}")")")
  fi
  if [[ -n "${LLVM_BINDIR}" ]]; then
    rt_prefixes+=("$(dirname "${LLVM_BINDIR}")")
  fi
  rt_prefixes+=("/usr/lib/llvm-${CLANG_MAJOR}" "${LOCAL_LLVM_PREFIX}" "${LOCAL_BOLT_DIR}")

  local rtp found_rt
  for rtp in "${rt_prefixes[@]}"; do
    if found_rt="$(find_bolt_rt_lib "${rtp}" 2>/dev/null)"; then
      BOLT_RT_INSTR_LIB="${found_rt}"
      break
    fi
  done
}

resolve_bolt_suite

if [[ -z "${BOLT_BIN}" || -z "${MERGE_FDATA_BIN}" || -z "${BOLT_RT_INSTR_LIB}" ]]; then
  if command -v apt-get >/dev/null 2>&1 && command -v dpkg-deb >/dev/null 2>&1; then
    mkdir -p "${DOWNLOAD_DIR}" "${LOCAL_BOLT_DIR}"
    shopt -s nullglob
    for pkg in "bolt-${CLANG_MAJOR}" "libbolt-${CLANG_MAJOR}-dev"; do
      cached_debs=("${DOWNLOAD_DIR}/${pkg}_"*.deb)
      if (( ${#cached_debs[@]} == 0 )); then
        log "Downloading ${pkg} into ${DOWNLOAD_DIR}"
        (cd "${DOWNLOAD_DIR}" && apt-get download "${pkg}" >&2) ||
          die "failed to download ${pkg} via apt-get download"
        cached_debs=("${DOWNLOAD_DIR}/${pkg}_"*.deb)
      else
        log "Reusing cached ${cached_debs[0]}"
      fi
      (( ${#cached_debs[@]} > 0 )) || die "no .deb archive found for ${pkg} in ${DOWNLOAD_DIR}"
      dpkg-deb -x "${cached_debs[0]}" "${LOCAL_BOLT_DIR}"
    done
    shopt -u nullglob
    resolve_bolt_suite
  fi
fi

[[ -n "${BOLT_BIN}" && -x "${BOLT_BIN}" ]] ||
  die "llvm-bolt for Clang ${CLANG_MAJOR} not found (install bolt-${CLANG_MAJOR})"
[[ -n "${MERGE_FDATA_BIN}" && -x "${MERGE_FDATA_BIN}" ]] ||
  die "merge-fdata for Clang ${CLANG_MAJOR} not found (install bolt-${CLANG_MAJOR})"
[[ -n "${PERF2BOLT_BIN}" && -x "${PERF2BOLT_BIN}" ]] ||
  die "perf2bolt for Clang ${CLANG_MAJOR} not found (install bolt-${CLANG_MAJOR})"
[[ -n "${BOLT_RT_INSTR_LIB}" && -f "${BOLT_RT_INSTR_LIB}" ]] ||
  die "libbolt_rt_instr.a for Clang ${CLANG_MAJOR} not found (install libbolt-${CLANG_MAJOR}-dev)"

LLVM_STRIP_BIN=""
strip_cands=()
if [[ -n "${LLVM_BINDIR}" ]]; then
  strip_cands+=("${LLVM_BINDIR}/llvm-strip")
fi
strip_cands+=(
  "$(command -v "llvm-strip-${CLANG_MAJOR}" 2>/dev/null || true)"
  "${LOCAL_LLVM_PREFIX}/bin/llvm-strip"
  "$(command -v llvm-strip 2>/dev/null || true)"
  "$(command -v strip 2>/dev/null || true)"
)
for sc in "${strip_cands[@]}"; do
  if [[ -n "${sc}" && -x "${sc}" ]]; then
    LLVM_STRIP_BIN="$(abs_keep_name "${sc}")"
    break
  fi
done
[[ -n "${LLVM_STRIP_BIN}" ]] || die "neither llvm-strip nor strip found in PATH"

printf 'BOLT_BIN=%q\n' "${BOLT_BIN}"
printf 'MERGE_FDATA_BIN=%q\n' "${MERGE_FDATA_BIN}"
printf 'PERF2BOLT_BIN=%q\n' "${PERF2BOLT_BIN}"
printf 'BOLT_RT_INSTR_LIB=%q\n' "${BOLT_RT_INSTR_LIB}"
printf 'LLVM_STRIP_BIN=%q\n' "${LLVM_STRIP_BIN}"
