#!/usr/bin/env bash
set -euo pipefail

# One-command portable build: builds the pinned static dependency set, then
# configures and builds build-static/ with SAN4_STATIC=ON (plus optional Clang
# Full/Thin LTO, two-stage LLVM IR PGO including libx86emu.a, and post-link
# llvm-bolt) and verifies that the resulting executables depend on nothing
# beyond glibc core libraries.
#
# Usage:
#   build_static.sh [options] [extra cmake configure args...]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${SAN4_STATIC_BUILD_DIR:-${ROOT_DIR}/build-static}"
JOBS="${SAN4_STATIC_JOBS:-$(nproc)}"
PREFIX_OVERRIDE="${SAN4_STATIC_PREFIX:-}"

use_clang=0
use_lto=0
lto_mode="FULL"
use_pgo=0
use_bolt=0
emit_bolt_relocs=0
clean_build=0
run_tests=0
no_game=0
game_dir="${SAN4_GAME_DIR:-}"
hd_pack_dir="${SAN4_HD_PACK_DIR:-}"
deps_args=()
cmake_args=()

usage() {
  cat << 'EOF'
Usage: build_static.sh [options] [extra cmake configure args...]

Builds self-contained portable san4-linux and san4-hdpack-gen binaries.

Options:
  --clang-lto        Build static deps & binaries with Clang + Full LTO
  --thin-lto         Build static deps & binaries with Clang + ThinLTO
  --lto              Enable LTO (GCC -flto=auto unless --clang-lto is also set)
  --pgo              Enable 2-stage LLVM IR PGO (implies --clang-lto; instruments libx86emu.a)
  --bolt             Enable post-link llvm-bolt optimization (implies --clang-lto)
  --pgo-bolt         Enable both LLVM IR PGO and post-link llvm-bolt (implies --clang-lto)
  --bolt-relocs      Emit -Wl,--emit-relocs without running llvm-bolt (implies --clang-lto)
  --game-dir <DIR>   Path to original SAN4 DOS game directory (read-only)
  --hd-pack <DIR>    Path to generated 4x HD asset pack for PGO/BOLT training
  --no-game          Build and train using synthetic suites without game files
  --build-dir <DIR>  Build directory (default: SAN4_STATIC_BUILD_DIR or ./build-static)
  --prefix <DIR>     Static dependency prefix (overrides SAN4_STATIC_PREFIX)
  --clean            Remove the build directory before configuring
  --test             Run ctest --output-on-failure after building
  -j, --jobs <N>     Parallel build jobs (default: SAN4_STATIC_JOBS or nproc)
  -h, --help         Show this help message and exit

Examples:
  ./scripts/build_static.sh --clang-lto --clean --test -DSAN4_GAME_DIR=/path/to/SAN4
  ./scripts/build_static.sh --clang-lto --pgo-bolt --clean --test --game-dir /path/to/SAN4 --hd-pack /path/to/hd_pack
  ./scripts/build_static.sh --test -DSAN4_CI_NO_GAME=ON
EOF
}

log() { echo "[static-build] $*"; }
die() { echo "[static-build] ERROR: $*" >&2; exit 1; }

while (( $# > 0 )); do
  case "$1" in
    --clang-lto)
      use_clang=1
      use_lto=1
      shift
      ;;
    --thin-lto)
      use_clang=1
      use_lto=1
      lto_mode="THIN"
      shift
      ;;
    --lto)
      use_lto=1
      shift
      ;;
    --pgo)
      use_pgo=1
      use_clang=1
      use_lto=1
      shift
      ;;
    --bolt)
      use_bolt=1
      use_clang=1
      use_lto=1
      shift
      ;;
    --pgo-bolt)
      use_pgo=1
      use_bolt=1
      use_clang=1
      use_lto=1
      shift
      ;;
    --bolt-relocs)
      emit_bolt_relocs=1
      use_clang=1
      use_lto=1
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
    --hd-pack)
      [[ $# -ge 2 ]] || { echo "error: --hd-pack requires an argument" >&2; exit 2; }
      hd_pack_dir="$2"
      shift 2
      ;;
    --hd-pack=*)
      hd_pack_dir="${1#*=}"
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
    --clean)
      clean_build=1
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
has_cmake_ci_no_game=0
for arg in "${cmake_args[@]}"; do
  case "${arg}" in
    -DSAN4_GAME_DIR=*|-DSAN4_GAME_DIR:PATH=*)
      has_cmake_game_dir=1
      [[ -n "${game_dir}" ]] || game_dir="${arg#*=}"
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
elif (( ! no_game )) && [[ -n "${game_dir}" ]] && (( ! has_cmake_game_dir )) && (( ! has_cmake_ci_no_game )); then
  cmake_args+=("-DSAN4_GAME_DIR=${game_dir}")
fi

if [[ -n "${PREFIX_OVERRIDE}" ]]; then
  PREFIX="${PREFIX_OVERRIDE}"
elif (( use_clang )); then
  if [[ "${lto_mode}" == "THIN" ]]; then
    PREFIX="${ROOT_DIR}/third_party/static-clang-thinlto"
  else
    PREFIX="${ROOT_DIR}/third_party/static-clang-lto"
  fi
elif (( use_lto )); then
  PREFIX="${ROOT_DIR}/third_party/static-gcc-lto"
else
  PREFIX="${ROOT_DIR}/third_party/static"
fi

if (( use_clang )); then
  if [[ "${lto_mode}" == "THIN" ]]; then
    deps_args+=(--thin-lto)
  else
    deps_args+=(--clang-lto)
  fi
  cmake_toolchain_args=(
    -DSAN4_USE_CLANG=ON
    -DSAN4_ENABLE_LTO=ON
    -DSAN4_LTO_MODE="${lto_mode}"
  )
elif (( use_lto )); then
  deps_args+=(--lto)
  cmake_toolchain_args=(
    -DSAN4_ENABLE_LTO=ON
  )
else
  cmake_toolchain_args=()
fi

SAN4_STATIC_PREFIX="${PREFIX}" SAN4_STATIC_JOBS="${JOBS}" \
  "${SCRIPT_DIR}/build_static_deps.sh" --prefix "${PREFIX}" --jobs "${JOBS}" "${deps_args[@]}"

if (( clean_build )); then
  rm -rf "${BUILD_DIR}"
fi

bolt_relocs_flag="OFF"
if (( use_bolt || emit_bolt_relocs )); then
  bolt_relocs_flag="ON"
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
if (( no_game )); then
  train_args+=(--no-game)
else
  [[ -z "${game_dir}" ]] || train_args+=(--game-dir "${game_dir}")
  [[ -z "${hd_pack_dir}" ]] || train_args+=(--hd-pack "${hd_pack_dir}")
fi

PGO_RAW_DIR="${BUILD_DIR}/pgo_raw"
PROFDATA_FILE="${BUILD_DIR}/san4.profdata"
PGO_X86EMU_DIR="${BUILD_DIR}/pgo_x86emu"
X86EMU_GEN_LIB="${PGO_X86EMU_DIR}/libx86emu_gen.a"
X86EMU_USE_LIB="${PGO_X86EMU_DIR}/libx86emu_use.a"
BOLT_WORK="${BUILD_DIR}/bolt_work"

if (( use_pgo )); then
  log "Stage 1/2: Building PGO-instrumented libx86emu.a & binaries (-fprofile-generate)"
  rm -rf "${PGO_RAW_DIR}" "${PROFDATA_FILE}" "${PGO_X86EMU_DIR}"
  mkdir -p "${PGO_RAW_DIR}" "${PGO_X86EMU_DIR}"

  SAN4_STATIC_PREFIX="${PREFIX}" SAN4_STATIC_JOBS="${JOBS}" \
    "${SCRIPT_DIR}/build_static_deps.sh" --prefix "${PREFIX}" --jobs "${JOBS}" "${deps_args[@]}" \
    --only-x86emu \
    --extra-cflags "-fprofile-generate=${PGO_RAW_DIR}" \
    --output-lib "${X86EMU_GEN_LIB}"

  cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSAN4_STATIC=ON \
    -DSAN4_STATIC_PREFIX="${PREFIX}" \
    "${cmake_toolchain_args[@]}" \
    -DSAN4_PGO_MODE=GENERATE \
    -DSAN4_PGO_PROFILE_DIR="${PGO_RAW_DIR}" \
    -DSAN4_PGO_X86EMU_LIB="${X86EMU_GEN_LIB}" \
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

  log "Stage 2/2: Building PGO-optimized libx86emu.a & binaries (-fprofile-use)"
  SAN4_STATIC_PREFIX="${PREFIX}" SAN4_STATIC_JOBS="${JOBS}" \
    "${SCRIPT_DIR}/build_static_deps.sh" --prefix "${PREFIX}" --jobs "${JOBS}" "${deps_args[@]}" \
    --only-x86emu \
    --extra-cflags "-fprofile-use=${PROFDATA_FILE} -Wno-backend-plugin -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date" \
    --output-lib "${X86EMU_USE_LIB}"

  cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSAN4_STATIC=ON \
    -DSAN4_STATIC_PREFIX="${PREFIX}" \
    "${cmake_toolchain_args[@]}" \
    -DSAN4_PGO_MODE=USE \
    -DSAN4_PGO_PROFILE_FILE="${PROFDATA_FILE}" \
    -DSAN4_PGO_X86EMU_LIB="${X86EMU_USE_LIB}" \
    -DSAN4_BOLT_RELOCS="${bolt_relocs_flag}" \
    "${cmake_args[@]}"
  cmake --build "${BUILD_DIR}" --clean-first -j "${JOBS}"
else
  cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSAN4_STATIC=ON \
    -DSAN4_STATIC_PREFIX="${PREFIX}" \
    "${cmake_toolchain_args[@]}" \
    -DSAN4_PGO_MODE=OFF \
    -DSAN4_PGO_X86EMU_LIB="" \
    -DSAN4_BOLT_RELOCS="${bolt_relocs_flag}" \
    "${cmake_args[@]}"
  cmake --build "${BUILD_DIR}" -j "${JOBS}"
fi

if (( use_bolt )); then
  CLANG_CXX="$(cache_var CMAKE_CXX_COMPILER)"
  eval "$("${SCRIPT_DIR}/ensure_bolt.sh" --clang "${CLANG_CXX}")"

  rm -rf "${BOLT_WORK}"
  mkdir -p "${BOLT_WORK}"
  cp -p "${BUILD_DIR}/san4-linux" "${BOLT_WORK}/san4-linux.prebolt"

  log "BOLT Stage 1/2: Instrumenting static san4-linux -> ${BOLT_WORK}/san4-linux.inst"
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

  log "BOLT Stage 2/2: Optimizing static san4-linux (ext-tsp + cdsort + split-functions)"
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

"${SCRIPT_DIR}/verify_static_binary.sh" \
  "${BUILD_DIR}/san4-linux" "${BUILD_DIR}/san4-hdpack-gen"

if (( use_clang )); then
  lto_verify_args=(
    --build-dir "${BUILD_DIR}"
    --static-prefix "${PREFIX}"
    --require-bitcode-x86emu
  )
  (( use_pgo )) && lto_verify_args+=(--x86emu-lib "${X86EMU_USE_LIB}" --verify-pgo "${PROFDATA_FILE}")
  (( use_bolt )) && lto_verify_args+=(--verify-bolt "${BOLT_WORK}/bolt.log")

  "${SCRIPT_DIR}/verify_clang_lto.sh" \
    "${lto_verify_args[@]}" \
    "${BUILD_DIR}/san4-linux" "${BUILD_DIR}/san4-hdpack-gen" "${BUILD_DIR}/san4_tests"
fi

# Collect the license texts of every statically linked component so they can
# be shipped next to the binaries.
LICENSES_OUT="${BUILD_DIR}/THIRD_PARTY_LICENSES.txt"
{
  cat "${ROOT_DIR}/THIRD_PARTY_NOTICES.md"
  shopt -s nullglob
  for lic in "${PREFIX}"/share/licenses/*/*; do
    [[ -f "${lic}" ]] || continue
    printf '\n\n===== %s =====\n\n' "$(basename "$(dirname "${lic}")")"
    cat "${lic}"
  done
  shopt -u nullglob
} > "${LICENSES_OUT}"

if (( run_tests )); then
  ctest --test-dir "${BUILD_DIR}" --output-on-failure
fi

log "Portable binaries: ${BUILD_DIR}/san4-linux ${BUILD_DIR}/san4-hdpack-gen"
log "Ship together with: ${ROOT_DIR}/LICENSE ${LICENSES_OUT}"
