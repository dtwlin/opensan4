#!/usr/bin/env bash
set -euo pipefail

# Builds OpenSan4 with Clang source-based code coverage (-DSAN4_ENABLE_COVERAGE=ON),
# runs the full hermetic CTest suite (-DSAN4_CI_NO_GAME=ON), and prints exact
# per-file and aggregate llvm-cov coverage tables.
#
# Usage:
#   ./scripts/run_coverage.sh [--build-dir <DIR>] [--jobs <N>] [--min-line-pct <PCT>] [--json-out <FILE>]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${ROOT_DIR}/build-cov"
JOBS="$(nproc 2>/dev/null || echo 8)"
MIN_LINE_PCT="0"
JSON_OUT=""

while (( $# > 0 )); do
  case "$1" in
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --build-dir=*)
      BUILD_DIR="${1#*=}"
      shift
      ;;
    --jobs|-j)
      JOBS="$2"
      shift 2
      ;;
    --jobs=*)
      JOBS="${1#*=}"
      shift
      ;;
    --min-line-pct)
      MIN_LINE_PCT="$2"
      shift 2
      ;;
    --min-line-pct=*)
      MIN_LINE_PCT="${1#*=}"
      shift
      ;;
    --json-out)
      JSON_OUT="$2"
      shift 2
      ;;
    --json-out=*)
      JSON_OUT="${1#*=}"
      shift
      ;;
    -h|--help)
      echo "Usage: $0 [--build-dir <DIR>] [--jobs <N>] [--min-line-pct <PCT>] [--json-out <FILE>]"
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      exit 2
      ;;
  esac
done

CLANG_SFX=""
FOUND_LLVM=0
for sfx in "" -23 -22 -21 -20 -19 -18 -17 -16 -15; do
  if command -v "clang${sfx}" >/dev/null 2>&1 && \
     command -v "clang++${sfx}" >/dev/null 2>&1 && \
     command -v "llvm-profdata${sfx}" >/dev/null 2>&1 && \
     command -v "llvm-cov${sfx}" >/dev/null 2>&1; then
    CLANG_SFX="${sfx}"
    FOUND_LLVM=1
    break
  fi
done

if (( ! FOUND_LLVM )); then
  echo "[coverage] Missing matching clang/clang++/llvm-profdata/llvm-cov toolchain." >&2
  exit 1
fi

CC_BIN="$(command -v "clang${CLANG_SFX}")"
CXX_BIN="$(command -v "clang++${CLANG_SFX}")"
PROFDATA_BIN="$(command -v "llvm-profdata${CLANG_SFX}")"
COV_BIN="$(command -v "llvm-cov${CLANG_SFX}")"

mkdir -p "${BUILD_DIR}"
BUILD_DIR="$(cd "${BUILD_DIR}" && pwd)"
if [[ -n "${JSON_OUT}" ]]; then
  JSON_DIR="$(dirname "${JSON_OUT}")"
  mkdir -p "${JSON_DIR}"
  JSON_OUT="$(cd "${JSON_DIR}" && pwd)/$(basename "${JSON_OUT}")"
fi

rm -rf "${BUILD_DIR}/prof"
mkdir -p "${BUILD_DIR}/prof"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_C_COMPILER="${CC_BIN}" \
  -DCMAKE_CXX_COMPILER="${CXX_BIN}" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSAN4_CI_NO_GAME=ON \
  -DSAN4_ENABLE_COVERAGE=ON \
  -DSAN4_WARNINGS_AS_ERRORS=ON

cmake --build "${BUILD_DIR}" -j "${JOBS}"

# Run unit conformance and CLI entrypoint suites into separate profile dirs so
# each executable's `main()` symbol merges cleanly without hash collisions.
mkdir -p "${BUILD_DIR}/prof/unit" "${BUILD_DIR}/prof/cli"
LLVM_PROFILE_FILE="${BUILD_DIR}/prof/unit/%p-%m.profraw" \
  ctest --test-dir "${BUILD_DIR}" -E "^san4_cli_entrypoints$" --output-on-failure
SAN4_COV_CLI_DIR="${BUILD_DIR}/prof/cli" \
  ctest --test-dir "${BUILD_DIR}" -R "^san4_cli_entrypoints$" --output-on-failure

"${PROFDATA_BIN}" merge -sparse "${BUILD_DIR}/prof/unit/"*.profraw \
  -o "${BUILD_DIR}/prof/unit.profdata"
"${PROFDATA_BIN}" merge -sparse "${BUILD_DIR}/prof/cli/runtime/"*.profraw \
  -o "${BUILD_DIR}/prof/cli_runtime.profdata"
"${PROFDATA_BIN}" merge -sparse "${BUILD_DIR}/prof/cli/hdgen/"*.profraw \
  -o "${BUILD_DIR}/prof/cli_hdgen.profdata"

python3 - "${COV_BIN}" "${ROOT_DIR}" "${BUILD_DIR}" "${MIN_LINE_PCT}" "${JSON_OUT}" << 'PYEOF'
import json
import subprocess
import sys

cov_bin, root_dir, build_dir, min_pct_str, json_out = sys.argv[1:6]
min_pct = float(min_pct_str)


def export_summary(binary, profdata):
    out = subprocess.run(
        [cov_bin, "export", "--summary-only", binary, f"-instr-profile={profdata}"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return json.loads(out)["data"][0]["files"]


def norm_rel(fn: str) -> str:
    idx = fn.find("/src/")
    if idx != -1:
        return fn[idx + 1 :]
    if fn.startswith("./src/"):
        return fn[2:]
    if fn.startswith("src/"):
        return fn
    return ""


files = {}
for f in export_summary(f"{build_dir}/san4_tests", f"{build_dir}/prof/unit.profdata"):
    rel = norm_rel(f["filename"])
    if rel:
        files[rel] = f["summary"]

for bin_name, prof_name, target_rel in [
    ("san4-linux", "cli_runtime.profdata", "src/main.cpp"),
    ("san4-hdpack-gen", "cli_hdgen.profdata", "src/hd_gen/hdpack_gen_main.cpp"),
]:
    for f in export_summary(f"{build_dir}/{bin_name}", f"{build_dir}/prof/{prof_name}"):
        rel = norm_rel(f["filename"])
        if rel == target_rel:
            files[rel] = f["summary"]

if json_out:
    with open(json_out, "w", encoding="utf-8") as jf:
        json.dump(files, jf, indent=2, sort_keys=True)


def render_table(title: str, exclude_vendor: bool):
    print(f"=== {title} ===")
    hdr = f"{'File':<34} {'Regions':>18} {'Functions':>18} {'Lines':>20} {'Branches':>18}"
    print(hdr)
    print("-" * len(hdr))
    tot = {k: [0, 0] for k in ("regions", "functions", "lines", "branches")}
    for rel in sorted(files):
        if exclude_vendor and "src/opl/dbopl." in rel:
            continue
        s = files[rel]
        for k in tot:
            tot[k][0] += s[k]["covered"]
            tot[k][1] += s[k]["count"]

        def fmt(k):
            c, n = s[k]["covered"], s[k]["count"]
            return f"{c}/{n} ({100.0 * c / n:5.1f}%)" if n else "-"

        print(
            f"{rel:<34} {fmt('regions'):>18} {fmt('functions'):>18} "
            f"{fmt('lines'):>20} {fmt('branches'):>18}"
        )
    print("-" * len(hdr))

    def fmt_t(k):
        c, n = tot[k]
        return f"{c}/{n} ({100.0 * c / n:5.2f}%)" if n else "-"

    print(
        f"{'TOTAL':<34} {fmt_t('regions'):>18} {fmt_t('functions'):>18} "
        f"{fmt_t('lines'):>20} {fmt_t('branches'):>18}"
    )
    line_pct = (100.0 * tot["lines"][0] / tot["lines"][1]) if tot["lines"][1] else 100.0
    return line_pct


first_party_pct = render_table(
    "FIRST-PARTY src/ COVERAGE (EXCLUDING VENDOR src/opl/dbopl.*)", True
)
print()
render_table("ALL src/ COVERAGE (INCLUDING VENDOR src/opl/dbopl.*)", False)

if first_party_pct < min_pct:
    print(
        f"[coverage FAIL] First-party line coverage {first_party_pct:.2f}% < required {min_pct:.2f}%",
        file=sys.stderr,
    )
    sys.exit(1)
PYEOF
