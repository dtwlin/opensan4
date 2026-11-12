#!/usr/bin/env bash
set -euo pipefail

# End-to-end subprocess CLI contract tests for `san4-linux` and
# `san4-hdpack-gen`. Uses only synthetic temporary directories (runnable under
# -DSAN4_CI_NO_GAME=ON with zero proprietary game files).

if [[ $# -lt 2 ]]; then
  echo "usage: $0 <san4-linux-bin> <san4-hdpack-gen-bin>" >&2
  exit 2
fi

RUNTIME_BIN="$1"
HDPACK_BIN="$2"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT

export SAN4_NO_ERROR_DIALOG=1
unset SAN4_GAME_DIR || true

fail() {
  echo "[cli-test FAIL] $*" >&2
  exit 1
}

expect_exit() {
  local expected_rc="$1"
  local needle="$2"
  local label="$3"
  shift 3
  local out=""
  local rc=0
  set +e
  out="$(timeout 15s "$@" 2>&1)"
  rc=$?
  set -e
  if [[ "${rc}" -ne "${expected_rc}" ]]; then
    echo "--- output ---" >&2
    echo "${out}" >&2
    fail "${label}: expected exit ${expected_rc}, got ${rc}"
  fi
  if [[ -n "${needle}" && "${out}" != *"${needle}"* ]]; then
    echo "--- output ---" >&2
    echo "${out}" >&2
    fail "${label}: expected output to contain '${needle}'"
  fi
}

mkdir -p "${WORK_DIR}/empty_game" "${WORK_DIR}/out_pack"

# When invoked under `scripts/run_coverage.sh`, separate runtime and generator
# raw profile directories so their C-linkage `main()` functions never collide.
if [[ -n "${SAN4_COV_CLI_DIR:-}" ]]; then
  mkdir -p "${SAN4_COV_CLI_DIR}/runtime" "${SAN4_COV_CLI_DIR}/hdgen"
  export LLVM_PROFILE_FILE="${SAN4_COV_CLI_DIR}/runtime/%p-%m.profraw"
fi

# 1. san4-linux CLI flags & BYOG verification gate
expect_exit 0 "--headless-script" "san4-linux --help" "${RUNTIME_BIN}" --help
expect_exit 0 "--deterministic" "san4-linux -h" "${RUNTIME_BIN}" -h
expect_exit 2 "Unknown option" "san4-linux unknown flag" "${RUNTIME_BIN}" --not-a-real-flag
expect_exit 2 "Missing argument for --scale" "san4-linux missing value" "${RUNTIME_BIN}" --scale
expect_exit 2 "Unknown --scale mode" "san4-linux invalid scale" "${RUNTIME_BIN}" --scale=bogus
expect_exit 2 "--deterministic requires --headless-script" "san4-linux deterministic guard" \
  "${RUNTIME_BIN}" --deterministic
expect_exit 1 "Bring-Your-Own-Game required" "san4-linux BYOG empty dir rejection" \
  "${RUNTIME_BIN}" --game-dir "${WORK_DIR}/empty_game" --headless-script "quit"

if [[ -n "${SAN4_COV_CLI_DIR:-}" ]]; then
  export LLVM_PROFILE_FILE="${SAN4_COV_CLI_DIR}/hdgen/%p-%m.profraw"
fi

# 2. san4-hdpack-gen CLI flags & BYOG verification gate
expect_exit 0 "--out-dir <DIR>" "san4-hdpack-gen --help" "${HDPACK_BIN}" --help
expect_exit 0 "--jobs <1..128>" "san4-hdpack-gen -h" "${HDPACK_BIN}" -h
expect_exit 2 "Unknown option" "san4-hdpack-gen unknown flag" "${HDPACK_BIN}" --bogus
expect_exit 2 "--out-dir <DIR> is required" "san4-hdpack-gen missing --out-dir" "${HDPACK_BIN}"
expect_exit 2 "Invalid --jobs" "san4-hdpack-gen invalid --jobs" \
  "${HDPACK_BIN}" --jobs=0 --out-dir "${WORK_DIR}/out_pack"
expect_exit 1 "Game directory verification failed" "san4-hdpack-gen BYOG empty dir rejection" \
  "${HDPACK_BIN}" --game-dir "${WORK_DIR}/empty_game" --out-dir "${WORK_DIR}/out_pack"

echo "[cli-test OK] All san4-linux & san4-hdpack-gen CLI entrypoint checks passed."
