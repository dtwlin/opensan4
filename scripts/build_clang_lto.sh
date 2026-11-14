#!/usr/bin/env bash
set -euo pipefail

# Configures and builds san4-linux, san4-hdpack-gen, and san4_tests using Clang
# with Link-Time Optimization (Full LTO by default, or ThinLTO with --thin),
# plus optional two-stage LLVM IR PGO (--pgo) and post-link llvm-bolt (--bolt).
#
# Usage:
#   build_clang_lto.sh [options] [extra cmake configure args...]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${SAN4_BUILD_DIR:-${ROOT_DIR}/build}"
JOBS="${SAN4_JOBS:-$(nproc)}"
LTO_MODE="FULL"
USE_PGO=0
USE_BOLT=0
EMIT_BOLT_RELOCS=0
CLEAN_BUILD=0
RUN_TESTS=0
NO_GAME=0
GAME_DIR="${SAN4_GAME_DIR:-}"
EMBED_GAME_DIR="${SAN4_EMBED_GAME_DIR:-}"
HD_PACK_DIR="${SAN4_HD_PACK_DIR:-}"
cmake_args=()

usage() {
  cat << 'EOF'
Usage: build_clang_lto.sh [options] [extra cmake configure args...]

Configures and builds san4-linux, san4-hdpack-gen, and san4_tests with Clang + LTO,
with optional LLVM IR Profile-Guided Optimization (--pgo) and llvm-bolt (--bolt).

Options:
  --build-dir <DIR>      Build directory (default: SAN4_BUILD_DIR or ./build)
  --thin                 Use Clang ThinLTO (SAN4_LTO_MODE=THIN)
  --full                 Use Clang Full LTO (SAN4_LTO_MODE=FULL, default)
  --pgo                  Enable 2-stage LLVM IR Profile-Guided Optimization
  --bolt                 Enable post-link llvm-bolt optimization on san4-linux
  --pgo-bolt             Enable both LLVM IR PGO and post-link llvm-bolt
  --bolt-relocs          Emit -Wl,--emit-relocs without running llvm-bolt
  --game-dir <DIR>       Path to original SAN4 DOS game directory (external BYOG, read-only)
  --embed-game-dir <DIR> Embed verified 43 SAN4 files into standalone san4-linux (personal use only)
  --hd-pack <DIR>        Path to generated 4x HD asset pack for PGO/BOLT training
  --no-game              Build and train using synthetic suites without game files
  --clean            Remove the build directory before configuring
  --test             Run ctest --output-on-failure after building
  -j, --jobs <N>     Parallel build jobs (default: SAN4_JOBS or nproc)
  -h, --help         Show this help message and exit

Examples:
  ./scripts/build_clang_lto.sh --clean --test -DSAN4_GAME_DIR=/path/to/SAN4
  ./scripts/build_clang_lto.sh --pgo-bolt --clean --test --game-dir /path/to/SAN4 --hd-pack /path/to/hd_pack
  ./scripts/build_clang_lto.sh --pgo-bolt --no-game --build-dir build-ci --test -DSAN4_CI_NO_GAME=ON
EOF
}

log() { echo "[clang-lto] $*"; }
die() { echo "[clang-lto] ERROR: $*" >&2; exit 1; }

while (( $# > 0 )); do
  case "$1" in
    --build-dir)
      [[ $# -ge 2 ]] || { echo "error: --build-dir requires an argument" >&2; exit 2; }
      BUILD_DIR="$2"
      shift 2
      ;;
    --build-dir=*)
      BUILD_DIR="${1#*=}"
      shift
      ;;
    --thin)
      LTO_MODE="THIN"
      shift
      ;;
    --full)
      LTO_MODE="FULL"
      shift
      ;;
    --pgo)
      USE_PGO=1
      shift
      ;;
    --bolt)
      USE_BOLT=1
      shift
      ;;
    --pgo-bolt)
      USE_PGO=1
      USE_BOLT=1
      shift
      ;;
    --bolt-relocs)
      EMIT_BOLT_RELOCS=1
      shift
      ;;
    --game-dir)
      [[ $# -ge 2 ]] || { echo "error: --game-dir requires an argument" >&2; exit 2; }
      GAME_DIR="$2"
      shift 2
      ;;
    --game-dir=*)
      GAME_DIR="${1#*=}"
      shift
      ;;
    --embed-game-dir)
      [[ $# -ge 2 ]] || { echo "error: --embed-game-dir requires an argument" >&2; exit 2; }
      EMBED_GAME_DIR="$2"
      shift 2
      ;;
    --embed-game-dir=*)
      EMBED_GAME_DIR="${1#*=}"
      shift
      ;;
    --hd-pack)
      [[ $# -ge 2 ]] || { echo "error: --hd-pack requires an argument" >&2; exit 2; }
      HD_PACK_DIR="$2"
      shift 2
      ;;
    --hd-pack=*)
      HD_PACK_DIR="${1#*=}"
      shift
      ;;
    --no-game)
      NO_GAME=1
      shift
      ;;
    --clean)
      CLEAN_BUILD=1
      shift
      ;;
    --test)
      RUN_TESTS=1
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
      [[ -n "${GAME_DIR}" ]] || GAME_DIR="${arg#*=}"
      ;;
    -DSAN4_EMBED_GAME_DIR=*|-DSAN4_EMBED_GAME_DIR:PATH=*)
      has_cmake_embed_game_dir=1
      [[ -n "${EMBED_GAME_DIR}" ]] || EMBED_GAME_DIR="${arg#*=}"
      ;;
    -DSAN4_CI_NO_GAME=*)
      has_cmake_ci_no_game=1
      val="${arg#*=}"
      case "${val^^}" in
        ON|TRUE|1|YES) NO_GAME=1 ;;
      esac
      ;;
  esac
done

if (( NO_GAME )) && (( ! has_cmake_ci_no_game )); then
  cmake_args+=("-DSAN4_CI_NO_GAME=ON")
fi
if (( ! NO_GAME )) && [[ -n "${EMBED_GAME_DIR}" ]] && (( ! has_cmake_embed_game_dir )) && (( ! has_cmake_ci_no_game )); then
  cmake_args+=("-DSAN4_EMBED_GAME_DIR=${EMBED_GAME_DIR}")
fi
if (( ! NO_GAME )) && [[ -n "${GAME_DIR}" ]] && (( ! has_cmake_game_dir )) && (( ! has_cmake_ci_no_game )); then
  cmake_args+=("-DSAN4_GAME_DIR=${GAME_DIR}")
fi

if (( CLEAN_BUILD )); then
  rm -rf "${BUILD_DIR}"
fi

BOLT_RELOCS_FLAG="OFF"
if (( USE_BOLT || EMIT_BOLT_RELOCS )); then
  BOLT_RELOCS_FLAG="ON"
fi

cache_var() {
  local key="$1" cache_file="${BUILD_DIR}/CMakeCache.txt"
  python3 - "${cache_file}" "${key}" << 'PYEOF'
import re, sys
txt = open(sys.argv[1]).read()
m = re.search(rf"^{re.escape(sys.argv[2])}:[^=]*=(.*)$", txt, re.M)
if m:
    print(m.group(1).strip())
PYEOF
}

train_args=(--build-dir "${BUILD_DIR}")
if (( NO_GAME )); then
  train_args+=(--no-game)
else
  if [[ -n "${GAME_DIR}" ]]; then
    train_args+=(--game-dir "${GAME_DIR}")
  elif [[ -n "${EMBED_GAME_DIR}" ]]; then
    train_args+=(--game-dir "${EMBED_GAME_DIR}")
  fi
  [[ -z "${HD_PACK_DIR}" ]] || train_args+=(--hd-pack "${HD_PACK_DIR}")
fi

PGO_RAW_DIR="${BUILD_DIR}/pgo_raw"
PROFDATA_FILE="${BUILD_DIR}/san4.profdata"
BOLT_WORK="${BUILD_DIR}/bolt_work"

if (( USE_PGO )); then
  log "Stage 1/2: Configuring & building IR PGO instrumented binaries (-fprofile-generate)"
  rm -rf "${PGO_RAW_DIR}" "${PROFDATA_FILE}"
  cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSAN4_USE_CLANG=ON \
    -DSAN4_ENABLE_LTO=ON \
    -DSAN4_LTO_MODE="${LTO_MODE}" \
    -DSAN4_PGO_MODE=GENERATE \
    -DSAN4_PGO_PROFILE_DIR="${PGO_RAW_DIR}" \
    -DSAN4_BOLT_RELOCS=OFF \
    "${cmake_args[@]}"
  cmake --build "${BUILD_DIR}" -j "${JOBS}"

  "${SCRIPT_DIR}/run_pgo_training.sh" --mode pgo --profile-dir "${PGO_RAW_DIR}" "${train_args[@]}"

  PROFDATA_BIN="$(cache_var SAN4_LLVM_PROFDATA)"
  [[ -n "${PROFDATA_BIN}" && -x "${PROFDATA_BIN}" ]] ||
    die "SAN4_LLVM_PROFDATA ('${PROFDATA_BIN}') missing or not executable"

  shopt -s nullglob
  profraw_files=("${PGO_RAW_DIR}"/*.profraw)
  shopt -u nullglob
  (( ${#profraw_files[@]} > 0 )) || die "no .profraw files generated in ${PGO_RAW_DIR}"

  log "Merging ${#profraw_files[@]} raw profile(s) -> ${PROFDATA_FILE}"
  "${PROFDATA_BIN}" merge -output="${PROFDATA_FILE}" "${profraw_files[@]}"

  log "Stage 2/2: Reconfiguring & building PGO-optimized binaries (-fprofile-use)"
  cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSAN4_USE_CLANG=ON \
    -DSAN4_ENABLE_LTO=ON \
    -DSAN4_LTO_MODE="${LTO_MODE}" \
    -DSAN4_PGO_MODE=USE \
    -DSAN4_PGO_PROFILE_FILE="${PROFDATA_FILE}" \
    -DSAN4_BOLT_RELOCS="${BOLT_RELOCS_FLAG}" \
    "${cmake_args[@]}"
  cmake --build "${BUILD_DIR}" --clean-first -j "${JOBS}"
else
  cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSAN4_USE_CLANG=ON \
    -DSAN4_ENABLE_LTO=ON \
    -DSAN4_LTO_MODE="${LTO_MODE}" \
    -DSAN4_PGO_MODE=OFF \
    -DSAN4_BOLT_RELOCS="${BOLT_RELOCS_FLAG}" \
    "${cmake_args[@]}"
  cmake --build "${BUILD_DIR}" -j "${JOBS}"
fi

if (( USE_BOLT )); then
  CLANG_CXX="$(cache_var CMAKE_CXX_COMPILER)"
  eval "$("${SCRIPT_DIR}/ensure_bolt.sh" --clang "${CLANG_CXX}")"

  rm -rf "${BOLT_WORK}"
  mkdir -p "${BOLT_WORK}"
  cp -p "${BUILD_DIR}/san4-linux" "${BOLT_WORK}/san4-linux.prebolt"

  log "BOLT Stage 1/2: Instrumenting san4-linux -> ${BOLT_WORK}/san4-linux.inst"
  "${BOLT_BIN}" "${BOLT_WORK}/san4-linux.prebolt" \
    -o "${BOLT_WORK}/san4-linux.inst" \
    --instrument \
    --runtime-instrumentation-lib="${BOLT_RT_INSTR_LIB}" \
    --instrumentation-file="${BOLT_WORK}/san4_trace.fdata" \
    --instrumentation-file-append-pid

  "${SCRIPT_DIR}/run_pgo_training.sh" \
    --mode bolt \
    --san4-bin "${BOLT_WORK}/san4-linux.inst" \
    "${train_args[@]}"

  shopt -s nullglob
  fdata_files=("${BOLT_WORK}"/san4_trace*.fdata)
  shopt -u nullglob
  (( ${#fdata_files[@]} > 0 )) || die "no BOLT .fdata traces generated in ${BOLT_WORK}"

  log "Merging ${#fdata_files[@]} BOLT trace(s) -> ${BOLT_WORK}/san4.fdata"
  "${MERGE_FDATA_BIN}" -o "${BOLT_WORK}/san4.fdata" "${fdata_files[@]}"
  rm -f "${BOLT_WORK}/san4-linux.inst" "${BOLT_WORK}"/san4_trace*.fdata

  log "BOLT Stage 2/2: Optimizing san4-linux (ext-tsp + cdsort + split-functions)"
  (
    cd "${BOLT_WORK}" &&
    "${BOLT_BIN}" ./san4-linux.prebolt \
      -o ./san4-linux.bolt \
      -data=./san4.fdata \
      -reorder-blocks=ext-tsp \
      -reorder-functions=cdsort \
      -split-functions \
      -split-all-cold \
      -split-eh \
      -icf=1 \
      -use-old-text \
      -no-huge-pages \
      -dyno-stats
  ) 2>&1 | tee "${BOLT_WORK}/bolt.log"

  cp -p "${BOLT_WORK}/san4-linux.bolt" "${BUILD_DIR}/san4-linux.bolt.tmp"
  mv -f "${BUILD_DIR}/san4-linux.bolt.tmp" "${BUILD_DIR}/san4-linux"
fi

find_elf_strip_tool() {
  if [[ -n "${LLVM_STRIP_BIN:-}" && -x "${LLVM_STRIP_BIN}" ]]; then
    echo "${LLVM_STRIP_BIN}"
    return 0
  fi
  for sfx in "" -23 -22 -21 -20 -19 -18 -17 -16 -15; do
    if command -v "llvm-strip${sfx}" >/dev/null 2>&1; then
      command -v "llvm-strip${sfx}"
      return 0
    fi
  done
  if command -v strip >/dev/null 2>&1; then
    command -v strip
    return 0
  fi
  return 1
}

STRIP_ELF_BIN="$(find_elf_strip_tool)" || die "neither llvm-strip nor strip found in PATH"
"${STRIP_ELF_BIN}" --strip-unneeded \
  --remove-section=.note.bolt_info \
  --remove-section=.comment \
  "${BUILD_DIR}/san4-linux" "${BUILD_DIR}/san4-hdpack-gen"

verify_args=(--build-dir "${BUILD_DIR}")
(( USE_PGO )) && verify_args+=(--verify-pgo "${PROFDATA_FILE}")
(( USE_BOLT )) && verify_args+=(--verify-bolt "${BOLT_WORK}/bolt.log")

"${SCRIPT_DIR}/verify_clang_lto.sh" "${verify_args[@]}" \
  "${BUILD_DIR}/san4-linux" \
  "${BUILD_DIR}/san4-hdpack-gen" \
  "${BUILD_DIR}/san4_tests"

if (( RUN_TESTS )); then
  ctest --test-dir "${BUILD_DIR}" --output-on-failure
fi

mode_desc="${LTO_MODE}"
(( USE_PGO )) && mode_desc+="+PGO"
(( USE_BOLT )) && mode_desc+="+BOLT"
log "Built (${mode_desc}): ${BUILD_DIR}/san4-linux ${BUILD_DIR}/san4-hdpack-gen"
