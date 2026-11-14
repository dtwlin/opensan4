#!/usr/bin/env bash
set -euo pipefail

# Synthetic CMake configure-time contract tests for SAN4_USE_CLANG,
# SAN4_ENABLE_LTO, SAN4_LTO_MODE, SAN4_PGO_MODE, SAN4_BOLT_RELOCS,
# SAN4_PGO_X86EMU_LIB, and sanitizer interaction.
# Runnable everywhere with -DSAN4_CI_NO_GAME=ON (no game files required).

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
CMAKE_BIN="$(command -v cmake)"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT

fail() {
  echo "[cmake-lto-test FAIL] $*" >&2
  exit 1
}

assert_contains() {
  local haystack needle="$2" label="$3"
  haystack="$(printf '%s' "$1" | tr '\r\n' '  ' | tr -s ' ')"
  if [[ "${haystack}" != *"${needle}"* ]]; then
    echo "--- output ---" >&2
    echo "$1" >&2
    fail "${label}: expected output to contain '${needle}'"
  fi
}

# 1. Invalid SAN4_LTO_MODE must fail at configure time with a clear message.
if out="$("${CMAKE_BIN}" -S "${ROOT_DIR}" -B "${WORK_DIR}/bad_mode" \
    -DSAN4_CI_NO_GAME=ON -DSAN4_LTO_MODE=INVALID_MODE 2>&1)"; then
  fail "SAN4_LTO_MODE=INVALID_MODE unexpectedly succeeded"
fi
assert_contains "${out}" "Invalid SAN4_LTO_MODE" "invalid LTO mode rejection"

# 2. SAN4_USE_CLANG=ON when no clang binary exists in PATH must fail cleanly.
mkdir -p "${WORK_DIR}/no_clang_bin"
ln -s "${CMAKE_BIN}" "${WORK_DIR}/no_clang_bin/cmake"
if out="$(PATH="${WORK_DIR}/no_clang_bin" "${CMAKE_BIN}" -S "${ROOT_DIR}" -B "${WORK_DIR}/no_clang" \
    -DSAN4_CI_NO_GAME=ON -DSAN4_USE_CLANG=ON 2>&1)"; then
  fail "SAN4_USE_CLANG=ON with empty PATH unexpectedly succeeded"
fi
assert_contains "${out}" "SAN4_USE_CLANG=ON, but no matching clang/clang++ compiler pair" \
  "missing clang failure message"

# 3. SAN4_ENABLE_LTO=ON combined with SAN4_ENABLE_SANITIZERS=ON must warn and
#    skip LTO cleanly rather than breaking sanitizer configuration.
out="$("${CMAKE_BIN}" -S "${ROOT_DIR}" -B "${WORK_DIR}/san_lto_skip" \
  -DSAN4_CI_NO_GAME=ON -DSAN4_ENABLE_SANITIZERS=ON -DSAN4_ENABLE_LTO=ON 2>&1)" ||
  fail "SAN4_ENABLE_SANITIZERS=ON + SAN4_ENABLE_LTO=ON should configure cleanly: ${out}"
assert_contains "${out}" "skipping LTO" "sanitizer LTO skip warning"
assert_contains "${out}" "LTO=OFF (skipped for sanitizers)" "sanitizer LTO status summary"

# 4. If a Clang pair is installed on this host, verify default LTO behavior:
#    - Debug build defaults LTO to OFF
#    - Release build defaults LTO to ON (FULL) and passes the LTO probe
has_clang=0
for sfx in "" -23 -22 -21 -20 -19 -18 -17 -16 -15; do
  if command -v "clang${sfx}" >/dev/null 2>&1 && command -v "clang++${sfx}" >/dev/null 2>&1; then
    has_clang=1
    break
  fi
done

if (( has_clang )); then
  out="$("${CMAKE_BIN}" -S "${ROOT_DIR}" -B "${WORK_DIR}/clang_debug" \
    -DCMAKE_BUILD_TYPE=Debug -DSAN4_CI_NO_GAME=ON -DSAN4_USE_CLANG=ON 2>&1)" ||
    fail "Clang Debug configure failed: ${out}"
  assert_contains "${out}" "compiler=Clang" "Clang auto-discovery in Debug"
  assert_contains "${out}" "LTO=OFF" "Clang Debug defaults LTO=OFF"

  out="$("${CMAKE_BIN}" -S "${ROOT_DIR}" -B "${WORK_DIR}/clang_rel" \
    -DCMAKE_BUILD_TYPE=Release -DSAN4_CI_NO_GAME=ON -DSAN4_USE_CLANG=ON 2>&1)" ||
    fail "Clang Release default LTO configure failed: ${out}"
  assert_contains "${out}" "compiler=Clang" "Clang auto-discovery in Release"
  assert_contains "${out}" "LTO=ON (FULL)" "Clang Release defaults LTO=ON (FULL)"

  out="$("${CMAKE_BIN}" -S "${ROOT_DIR}" -B "${WORK_DIR}/clang_thin" \
    -DCMAKE_BUILD_TYPE=Release -DSAN4_CI_NO_GAME=ON -DSAN4_USE_CLANG=ON -DSAN4_LTO_MODE=thin 2>&1)" ||
    fail "Clang ThinLTO case-insensitive configure failed: ${out}"
  assert_contains "${out}" "LTO=ON (THIN)" "Clang ThinLTO case-insensitive mode"

  # 5. SAN4_PGO_MODE=generate (case-insensitive) configures cleanly and logs PGO=GENERATE.
  out="$("${CMAKE_BIN}" -S "${ROOT_DIR}" -B "${WORK_DIR}/clang_pgo_gen" \
    -DCMAKE_BUILD_TYPE=Release -DSAN4_CI_NO_GAME=ON -DSAN4_USE_CLANG=ON \
    -DSAN4_PGO_MODE=generate -DSAN4_PGO_PROFILE_DIR="${WORK_DIR}/pgo_raw" 2>&1)" ||
    fail "Clang SAN4_PGO_MODE=generate configure failed: ${out}"
  assert_contains "${out}" "PGO=GENERATE" "Clang PGO=GENERATE summary"
  assert_contains "${out}" "BOLT_RELOCS=OFF" "default BOLT_RELOCS=OFF summary"

  # 6. SAN4_PGO_MODE=USE with missing .profdata must fail fast.
  if out="$("${CMAKE_BIN}" -S "${ROOT_DIR}" -B "${WORK_DIR}/clang_pgo_missing" \
      -DCMAKE_BUILD_TYPE=Release -DSAN4_CI_NO_GAME=ON -DSAN4_USE_CLANG=ON \
      -DSAN4_PGO_MODE=USE -DSAN4_PGO_PROFILE_FILE="${WORK_DIR}/does_not_exist.profdata" 2>&1)"; then
    fail "SAN4_PGO_MODE=USE with missing profdata unexpectedly succeeded"
  fi
  assert_contains "${out}" "SAN4_PGO_MODE=USE requires an existing merged profile" \
    "missing profdata rejection"

  # 7. SAN4_BOLT_RELOCS=ON configures cleanly and logs BOLT_RELOCS=ON.
  out="$("${CMAKE_BIN}" -S "${ROOT_DIR}" -B "${WORK_DIR}/clang_bolt_relocs" \
    -DCMAKE_BUILD_TYPE=Release -DSAN4_CI_NO_GAME=ON -DSAN4_USE_CLANG=ON \
    -DSAN4_BOLT_RELOCS=ON 2>&1)" ||
    fail "Clang SAN4_BOLT_RELOCS=ON configure failed: ${out}"
  assert_contains "${out}" "BOLT_RELOCS=ON" "Clang BOLT_RELOCS=ON summary"

  # 8. SAN4_PGO_MODE != OFF combined with sanitizers must fail fast.
  if out="$("${CMAKE_BIN}" -S "${ROOT_DIR}" -B "${WORK_DIR}/pgo_asan_bad" \
      -DSAN4_CI_NO_GAME=ON -DSAN4_USE_CLANG=ON \
      -DSAN4_ENABLE_SANITIZERS=ON -DSAN4_PGO_MODE=GENERATE 2>&1)"; then
    fail "SAN4_PGO_MODE=GENERATE + SAN4_ENABLE_SANITIZERS=ON unexpectedly succeeded"
  fi
  assert_contains "${out}" "cannot be combined with SAN4_ENABLE_SANITIZERS or SAN4_ENABLE_TSAN" \
    "PGO + ASan mutual exclusion"

  if out="$("${CMAKE_BIN}" -S "${ROOT_DIR}" -B "${WORK_DIR}/pgo_tsan_bad" \
      -DSAN4_CI_NO_GAME=ON -DSAN4_USE_CLANG=ON \
      -DSAN4_ENABLE_TSAN=ON -DSAN4_PGO_MODE=GENERATE 2>&1)"; then
    fail "SAN4_PGO_MODE=GENERATE + SAN4_ENABLE_TSAN=ON unexpectedly succeeded"
  fi
  assert_contains "${out}" "cannot be combined with SAN4_ENABLE_SANITIZERS or SAN4_ENABLE_TSAN" \
    "PGO + TSan mutual exclusion"

  # 8b. SAN4_ENABLE_COVERAGE=ON configures cleanly on Clang (defaulting LTO=OFF)
  #     and rejects sanitizers and PGO.
  out="$("${CMAKE_BIN}" -S "${ROOT_DIR}" -B "${WORK_DIR}/clang_cov_ok" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSAN4_CI_NO_GAME=ON -DSAN4_USE_CLANG=ON \
    -DSAN4_ENABLE_COVERAGE=ON 2>&1)" ||
    fail "Clang SAN4_ENABLE_COVERAGE=ON configure failed: ${out}"
  assert_contains "${out}" "LTO=OFF" "Clang coverage defaults LTO=OFF"

  if out="$("${CMAKE_BIN}" -S "${ROOT_DIR}" -B "${WORK_DIR}/clang_cov_asan_bad" \
      -DSAN4_CI_NO_GAME=ON -DSAN4_USE_CLANG=ON \
      -DSAN4_ENABLE_COVERAGE=ON -DSAN4_ENABLE_SANITIZERS=ON 2>&1)"; then
    fail "SAN4_ENABLE_COVERAGE=ON + SAN4_ENABLE_SANITIZERS=ON unexpectedly succeeded"
  fi
  assert_contains "${out}" "SAN4_ENABLE_COVERAGE cannot be combined" \
    "coverage + ASan mutual exclusion"

  if out="$("${CMAKE_BIN}" -S "${ROOT_DIR}" -B "${WORK_DIR}/clang_cov_pgo_bad" \
      -DSAN4_CI_NO_GAME=ON -DSAN4_USE_CLANG=ON \
      -DSAN4_ENABLE_COVERAGE=ON -DSAN4_PGO_MODE=GENERATE 2>&1)"; then
    fail "SAN4_ENABLE_COVERAGE=ON + SAN4_PGO_MODE=GENERATE unexpectedly succeeded"
  fi
  assert_contains "${out}" "SAN4_ENABLE_COVERAGE cannot be combined with SAN4_PGO_MODE" \
    "coverage + PGO mutual exclusion"
fi

# 9. Invalid SAN4_PGO_MODE must fail at configure time on any compiler.
if out="$("${CMAKE_BIN}" -S "${ROOT_DIR}" -B "${WORK_DIR}/bad_pgo_mode" \
    -DSAN4_CI_NO_GAME=ON -DSAN4_PGO_MODE=BOGUS_MODE 2>&1)"; then
  fail "SAN4_PGO_MODE=BOGUS_MODE unexpectedly succeeded"
fi
assert_contains "${out}" "Invalid SAN4_PGO_MODE" "invalid PGO mode rejection"

# 10. Non-Clang compiler + SAN4_PGO_MODE != OFF must fail cleanly.
if command -v gcc >/dev/null 2>&1 && command -v g++ >/dev/null 2>&1; then
  if out="$("${CMAKE_BIN}" -S "${ROOT_DIR}" -B "${WORK_DIR}/gcc_pgo_bad" \
      -DSAN4_CI_NO_GAME=ON -DSAN4_USE_CLANG=OFF \
      -DCMAKE_C_COMPILER="$(command -v gcc)" \
      -DCMAKE_CXX_COMPILER="$(command -v g++)" \
      -DSAN4_PGO_MODE=GENERATE 2>&1)"; then
    fail "GCC + SAN4_PGO_MODE=GENERATE unexpectedly succeeded"
  fi
  assert_contains "${out}" "requires Clang" "non-Clang PGO rejection"

  if out="$("${CMAKE_BIN}" -S "${ROOT_DIR}" -B "${WORK_DIR}/gcc_cov_bad" \
      -DSAN4_CI_NO_GAME=ON -DSAN4_USE_CLANG=OFF \
      -DCMAKE_C_COMPILER="$(command -v gcc)" \
      -DCMAKE_CXX_COMPILER="$(command -v g++)" \
      -DSAN4_ENABLE_COVERAGE=ON 2>&1)"; then
    fail "GCC + SAN4_ENABLE_COVERAGE=ON unexpectedly succeeded"
  fi
  assert_contains "${out}" "SAN4_ENABLE_COVERAGE requires Clang" "non-Clang coverage rejection"
fi

# 11. SAN4_STATIC=ON + missing SAN4_PGO_X86EMU_LIB override must fail fast.
if out="$("${CMAKE_BIN}" -S "${ROOT_DIR}" -B "${WORK_DIR}/bad_x86emu_override" \
    -DSAN4_CI_NO_GAME=ON -DSAN4_STATIC=ON \
    -DSAN4_PGO_X86EMU_LIB="${WORK_DIR}/missing_x86emu.a" 2>&1)"; then
  fail "missing SAN4_PGO_X86EMU_LIB unexpectedly succeeded"
fi
assert_contains "${out}" "SAN4_PGO_X86EMU_LIB" "missing SAN4_PGO_X86EMU_LIB rejection"

# 12. SAN4_EMBED_GAME_DIR contract checks: missing directory, incomplete game
#     directory, and mutual exclusion with SAN4_CI_NO_GAME=ON must all fail fast.
if out="$("${CMAKE_BIN}" -S "${ROOT_DIR}" -B "${WORK_DIR}/embed_missing_dir" \
    -DSAN4_EMBED_GAME_DIR="${WORK_DIR}/no_such_dir" 2>&1)"; then
  fail "SAN4_EMBED_GAME_DIR pointing to missing directory unexpectedly succeeded"
fi
assert_contains "${out}" "SAN4_EMBED_GAME_DIR does not exist or is not a directory" \
  "missing SAN4_EMBED_GAME_DIR rejection"

mkdir -p "${WORK_DIR}/empty_embed_dir"
if out="$("${CMAKE_BIN}" -S "${ROOT_DIR}" -B "${WORK_DIR}/embed_empty_dir" \
    -DSAN4_EMBED_GAME_DIR="${WORK_DIR}/empty_embed_dir" 2>&1)"; then
  fail "SAN4_EMBED_GAME_DIR pointing to empty directory unexpectedly succeeded"
fi
assert_contains "${out}" "Embedded game file verification failed" \
  "empty SAN4_EMBED_GAME_DIR manifest rejection"
assert_contains "${out}" "Missing: EDMUSIC.S4" \
  "empty SAN4_EMBED_GAME_DIR missing file detail"

if out="$("${CMAKE_BIN}" -S "${ROOT_DIR}" -B "${WORK_DIR}/embed_ci_conflict" \
    -DSAN4_CI_NO_GAME=ON -DSAN4_EMBED_GAME_DIR="${WORK_DIR}/empty_embed_dir" 2>&1)"; then
  fail "SAN4_EMBED_GAME_DIR + SAN4_CI_NO_GAME=ON unexpectedly succeeded"
fi
assert_contains "${out}" "SAN4_EMBED_GAME_DIR cannot be combined with SAN4_CI_NO_GAME=ON" \
  "SAN4_EMBED_GAME_DIR + SAN4_CI_NO_GAME mutual exclusion"

echo "[cmake-lto-test OK] All CMake Clang/LTO/PGO/BOLT/Embed contract checks passed."
