#!/usr/bin/env bash
set -euo pipefail

# Deterministic training workload driver for LLVM IR PGO (--mode pgo) and
# llvm-bolt binary instrumentation (--mode bolt).
#
# Always isolates DOS writes into a temporary --save-dir (cleaned on exit) so
# --game-dir and --hd-pack remain strictly read-only.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

MODE="pgo"
BUILD_DIR=""
SAN4_BIN=""
HDPACK_GEN_BIN=""
TESTS_BIN=""
GAME_DIR="${SAN4_GAME_DIR:-}"
HD_PACK_DIR="${SAN4_HD_PACK_DIR:-}"
NO_GAME=0
PROFILE_DIR=""

usage() {
  cat << 'EOF'
Usage: run_pgo_training.sh [options]

Runs deterministic training workloads for Clang IR PGO or llvm-bolt.

Options:
  --mode <pgo|bolt>       Training mode (default: pgo)
  --build-dir <DIR>       Build directory for default binary paths
  --san4-bin <FILE>       Path to san4-linux (or san4-linux.inst in bolt mode)
  --hdpack-gen-bin <FILE> Path to san4-hdpack-gen
  --tests-bin <FILE>      Path to san4_tests
  --game-dir <DIR>        Path to original SAN4 DOS game directory (read-only)
  --hd-pack <DIR>         Path to generated 4x HD asset pack (read-only)
  --no-game               Force synthetic no-game fallback workload
  --profile-dir <DIR>     Output directory for .profraw files in --mode pgo
  -h, --help              Show this help message and exit
EOF
}

log() { echo "[pgo-train:${MODE}] $*"; }
die() { echo "[pgo-train:${MODE}] ERROR: $*" >&2; exit 1; }

while (( $# > 0 )); do
  case "$1" in
    --mode)
      [[ $# -ge 2 ]] || die "--mode requires an argument"
      MODE="$2"
      shift 2
      ;;
    --mode=*)
      MODE="${1#*=}"
      shift
      ;;
    --build-dir)
      [[ $# -ge 2 ]] || die "--build-dir requires an argument"
      BUILD_DIR="$2"
      shift 2
      ;;
    --build-dir=*)
      BUILD_DIR="${1#*=}"
      shift
      ;;
    --san4-bin)
      [[ $# -ge 2 ]] || die "--san4-bin requires an argument"
      SAN4_BIN="$2"
      shift 2
      ;;
    --san4-bin=*)
      SAN4_BIN="${1#*=}"
      shift
      ;;
    --hdpack-gen-bin)
      [[ $# -ge 2 ]] || die "--hdpack-gen-bin requires an argument"
      HDPACK_GEN_BIN="$2"
      shift 2
      ;;
    --hdpack-gen-bin=*)
      HDPACK_GEN_BIN="${1#*=}"
      shift
      ;;
    --tests-bin)
      [[ $# -ge 2 ]] || die "--tests-bin requires an argument"
      TESTS_BIN="$2"
      shift 2
      ;;
    --tests-bin=*)
      TESTS_BIN="${1#*=}"
      shift
      ;;
    --game-dir)
      [[ $# -ge 2 ]] || die "--game-dir requires an argument"
      GAME_DIR="$2"
      shift 2
      ;;
    --game-dir=*)
      GAME_DIR="${1#*=}"
      shift
      ;;
    --hd-pack)
      [[ $# -ge 2 ]] || die "--hd-pack requires an argument"
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
    --profile-dir)
      [[ $# -ge 2 ]] || die "--profile-dir requires an argument"
      PROFILE_DIR="$2"
      shift 2
      ;;
    --profile-dir=*)
      PROFILE_DIR="${1#*=}"
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

case "${MODE}" in
  pgo|bolt) ;;
  *) die "invalid --mode '${MODE}' (expected pgo or bolt)" ;;
esac

if [[ -n "${BUILD_DIR}" ]]; then
  [[ -n "${SAN4_BIN}" ]] || SAN4_BIN="${BUILD_DIR}/san4-linux"
  [[ -n "${HDPACK_GEN_BIN}" ]] || HDPACK_GEN_BIN="${BUILD_DIR}/san4-hdpack-gen"
  [[ -n "${TESTS_BIN}" ]] || TESTS_BIN="${BUILD_DIR}/san4_tests"
  [[ -n "${PROFILE_DIR}" ]] || PROFILE_DIR="${BUILD_DIR}/pgo_raw"
  if [[ -z "${GAME_DIR}" && -f "${BUILD_DIR}/san4_game_dir.txt" ]]; then
    GAME_DIR="$(head -n 1 "${BUILD_DIR}/san4_game_dir.txt" | tr -d '\r\n')"
  fi
fi

[[ -n "${SAN4_BIN}" && -x "${SAN4_BIN}" ]] || die "san4-linux binary not executable: '${SAN4_BIN}'"

if [[ "${MODE}" == "pgo" ]]; then
  [[ -n "${PROFILE_DIR}" ]] || die "--profile-dir (or --build-dir) is required in --mode pgo"
  [[ -n "${HDPACK_GEN_BIN}" && -x "${HDPACK_GEN_BIN}" ]] ||
    die "san4-hdpack-gen binary not executable: '${HDPACK_GEN_BIN}'"
  [[ -n "${TESTS_BIN}" && -x "${TESTS_BIN}" ]] ||
    die "san4_tests binary not executable: '${TESTS_BIN}'"
  mkdir -p "${PROFILE_DIR}"
  export LLVM_PROFILE_FILE="${PROFILE_DIR}/san4_%p_%m.profraw"
fi

# Ensure bundled dynamic third_party libraries remain discoverable even when an
# instrumented binary (e.g. bolt_work/san4-linux.inst) runs outside <build_dir>.
extra_ld_paths=()
for tp_lib in "${ROOT_DIR}/third_party/sdl2_dev/lib" "${ROOT_DIR}/third_party/x86emu/lib"; do
  if [[ -d "${tp_lib}" ]]; then
    extra_ld_paths+=("${tp_lib}")
  fi
done
if (( ${#extra_ld_paths[@]} > 0 )); then
  joined_ld="$(IFS=:; echo "${extra_ld_paths[*]}")"
  export LD_LIBRARY_PATH="${joined_ld}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
fi

TMP_SAVE_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_SAVE_DIR}"' EXIT

has_game_main() {
  local dir="$1"
  [[ -n "${dir}" && -d "${dir}" ]] || return 1
  python3 - "${dir}" << 'PYEOF'
import os, sys
for fn in os.listdir(sys.argv[1]):
    if fn.upper() == "MAIN.EXE" and os.path.isfile(os.path.join(sys.argv[1], fn)):
        sys.exit(0)
sys.exit(1)
PYEOF
}

if (( ! NO_GAME )) && [[ -z "${HD_PACK_DIR}" && -f "${ROOT_DIR}/../hd_pack/hd_manifest.tsv" ]]; then
  HD_PACK_DIR="$(cd "${ROOT_DIR}/../hd_pack" && pwd)"
fi

if (( ! NO_GAME )) && has_game_main "${GAME_DIR}"; then
  # Deterministic ~1,800-frame script: title -> main menu -> load save dialog ->
  # strategic map panning -> city panel & officer inspection -> quit.
  HD_SCRIPT="frames:170;key:enter;frames:100"
  HD_SCRIPT+=";move:396:136;frames:20;click:396:136;frames:100"
  HD_SCRIPT+=";move:360:234;frames:20;click:360:234;frames:420"
  HD_SCRIPT+=";key:right;frames:30;key:down;frames:30;key:left;frames:30;key:up;frames:30"
  HD_SCRIPT+=";move:312:138;frames:20;click:312:138;frames:320"
  HD_SCRIPT+=";move:766:262;frames:20;click:766:262;frames:240"
  HD_SCRIPT+=";wheel:-3;frames:30;rclick:400:300;frames:120;stats;quit"

  if [[ -n "${HD_PACK_DIR}" && -f "${HD_PACK_DIR}/hd_manifest.tsv" ]]; then
    log "Run 1: Deterministic HD 4x replay (HdPackManager + simd_blend + libx86emu + DBOPL)"
    timeout 90s "${SAN4_BIN}" \
      --deterministic --skip-intro \
      --game-dir "${GAME_DIR}" \
      --hd-pack "${HD_PACK_DIR}" \
      --save-dir "${TMP_SAVE_DIR}" \
      --scale xbr4x \
      --headless-script "${HD_SCRIPT}"
  else
    log "Skipping HD 4x replay (no hd_manifest.tsv found)"
  fi

  CLASSIC_SCRIPT="frames:170;key:enter;frames:90"
  CLASSIC_SCRIPT+=";move:396:136;frames:15;click:396:136;frames:90"
  CLASSIC_SCRIPT+=";move:360:234;frames:15;click:360:234;frames:180"
  CLASSIC_SCRIPT+=";key:right;frames:20;move:312:138;frames:15;click:312:138;frames:90;stats;quit"

  for scale_mode in xbr4x xbr2x; do
    log "Run 2 (${scale_mode}): Deterministic classic replay (PixelUpscaler ${scale_mode} + VesaPlanarVideo + DBOPL)"
    timeout 60s "${SAN4_BIN}" \
      --deterministic --skip-intro --no-hd-pack \
      --game-dir "${GAME_DIR}" \
      --save-dir "${TMP_SAVE_DIR}" \
      --scale "${scale_mode}" \
      --headless-script "${CLASSIC_SCRIPT}"
  done

  if [[ "${MODE}" == "pgo" ]]; then
    log "Run 3: Synthetic conformance & codec test suite (san4_tests + san4-hdpack-gen --help)"
    timeout 120s "${TESTS_BIN}" >/dev/null
    timeout 15s "${HDPACK_GEN_BIN}" --help >/dev/null
    timeout 15s "${SAN4_BIN}" --help >/dev/null
  fi
else
  log "Running synthetic no-game fallback training workload"
  if [[ "${MODE}" == "pgo" ]]; then
    timeout 120s "${TESTS_BIN}" >/dev/null
    timeout 15s "${HDPACK_GEN_BIN}" --help >/dev/null
    timeout 15s "${SAN4_BIN}" --help >/dev/null
  else
    timeout 30s "${SAN4_BIN}" --help >/dev/null
  fi
fi

log "Training completed."
