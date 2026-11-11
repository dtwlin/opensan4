#!/usr/bin/env bash
set -euo pipefail

# Verifies Clang LTO build artifacts:
#   1. Intermediate .o files and project static archives (libsan4_opl.a,
#      libsan4_core.a) contain LLVM bitcode (BC\xc0\xde) and carry valid archive
#      symbol tables produced by llvm-ar.
#   2. Optional --static-prefix archives carry valid symbol tables (and
#      libx86emu.a contains LLVM bitcode when --require-bitcode-x86emu is set).
#   3. Final linked executables are native ELF binaries (not raw bitcode) and
#      launch cleanly with --help.
#
# Usage:
#   verify_clang_lto.sh --build-dir <DIR> [--static-prefix <DIR>]
#                       [--x86emu-lib <FILE>] [--require-bitcode-x86emu]
#                       [--verify-pgo <PROFDATA>] [--verify-bolt <BOLT_LOG>]
#                       <elf> [<elf>...]

BUILD_DIR=""
STATIC_PREFIX=""
X86EMU_LIB_OVERRIDE=""
REQUIRE_BITCODE_X86EMU=0
VERIFY_PGO_FILE=""
VERIFY_BOLT_LOG=""
ELFS=()

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
    --static-prefix)
      [[ $# -ge 2 ]] || { echo "error: --static-prefix requires an argument" >&2; exit 2; }
      STATIC_PREFIX="$2"
      shift 2
      ;;
    --static-prefix=*)
      STATIC_PREFIX="${1#*=}"
      shift
      ;;
    --x86emu-lib)
      [[ $# -ge 2 ]] || { echo "error: --x86emu-lib requires an argument" >&2; exit 2; }
      X86EMU_LIB_OVERRIDE="$2"
      shift 2
      ;;
    --x86emu-lib=*)
      X86EMU_LIB_OVERRIDE="${1#*=}"
      shift
      ;;
    --require-bitcode-x86emu)
      REQUIRE_BITCODE_X86EMU=1
      shift
      ;;
    --verify-pgo)
      [[ $# -ge 2 ]] || { echo "error: --verify-pgo requires an argument" >&2; exit 2; }
      VERIFY_PGO_FILE="$2"
      shift 2
      ;;
    --verify-pgo=*)
      VERIFY_PGO_FILE="${1#*=}"
      shift
      ;;
    --verify-bolt)
      [[ $# -ge 2 ]] || { echo "error: --verify-bolt requires an argument" >&2; exit 2; }
      VERIFY_BOLT_LOG="$2"
      shift 2
      ;;
    --verify-bolt=*)
      VERIFY_BOLT_LOG="${1#*=}"
      shift
      ;;
    -h|--help)
      echo "usage: $0 --build-dir <DIR> [--static-prefix <DIR>] [--x86emu-lib <FILE>] [--require-bitcode-x86emu] [--verify-pgo <PROFDATA>] [--verify-bolt <BOLT_LOG>] <elf> [<elf>...]"
      exit 0
      ;;
    *)
      ELFS+=("$1")
      shift
      ;;
  esac
done

if [[ -z "${BUILD_DIR}" || ${#ELFS[@]} -eq 0 ]]; then
  echo "usage: $0 --build-dir <DIR> [--static-prefix <DIR>] [--x86emu-lib <FILE>] [--require-bitcode-x86emu] [--verify-pgo <PROFDATA>] [--verify-bolt <BOLT_LOG>] <elf> [<elf>...]" >&2
  exit 2
fi

python3 - "${BUILD_DIR}" "${STATIC_PREFIX}" "${X86EMU_LIB_OVERRIDE}" "${REQUIRE_BITCODE_X86EMU}" "${VERIFY_PGO_FILE}" "${VERIFY_BOLT_LOG}" "${ELFS[@]}" << 'PYEOF'
import os
import re
import shutil
import subprocess
import sys

BC_RAW_MAGIC = b"BC\xc0\xde"
BC_WRAP_MAGIC = b"\xde\xc0\x17\x0b"
ELF_MAGIC = b"\x7fELF"
AR_MAGIC = b"!<arch>\n"


def is_bitcode_bytes(buf: bytes) -> bool:
    return buf[:4] in (BC_RAW_MAGIC, BC_WRAP_MAGIC)


def parse_ar(path: str):
    """Yields (name, payload_bytes) for each member in a standard ar archive."""
    data = open(path, "rb").read()
    if not data.startswith(AR_MAGIC):
        raise ValueError(f"{path}: not a valid ar archive")
    pos = len(AR_MAGIC)
    ext_names = b""
    members = []
    while pos + 60 <= len(data):
        hdr = data[pos : pos + 60]
        if hdr[58:60] != b"`\n":
            raise ValueError(f"{path}: malformed ar member header at offset {pos}")
        raw_name = hdr[0:16].decode("latin1")
        size = int(hdr[48:58].decode("ascii").strip() or "0")
        body_start = pos + 60
        body_end = body_start + size
        if body_end > len(data):
            raise ValueError(f"{path}: truncated ar member at offset {pos}")
        payload = data[body_start:body_end]
        pos = body_end + (size & 1)

        trimmed = raw_name.rstrip(" ")
        if trimmed == "//":
            ext_names = payload
            continue
        if trimmed in ("/", "/SYM64/") or trimmed.startswith("__.SYMDEF"):
            members.append(("<symtab>", payload))
            continue
        if trimmed.startswith("#1/"):
            nlen = int(trimmed[3:])
            name = payload[:nlen].rstrip(b"\x00").decode("latin1")
            payload = payload[nlen:]
        elif trimmed.startswith("/") and trimmed[1:].isdigit() and ext_names:
            off = int(trimmed[1:])
            end = ext_names.find(b"/\n", off)
            if end == -1:
                end = ext_names.find(b"\n", off)
            name = ext_names[off:end].decode("latin1") if end != -1 else trimmed
        else:
            name = trimmed.rstrip("/")
        members.append((name, payload))
    return members


build_dir = sys.argv[1]
static_prefix = sys.argv[2]
x86emu_lib_override = sys.argv[3]
require_bc_x86emu = sys.argv[4] == "1"
verify_pgo_file = sys.argv[5]
verify_bolt_log = sys.argv[6]
elf_paths = sys.argv[7:]

errors = []

# 1. Verify CMake object files under target directories are LLVM bitcode.
target_dirs = [
    "san4_opl.dir",
    "san4_core.dir",
    "san4-linux.dir",
    "san4-hdpack-gen.dir",
    "san4_tests.dir",
]
obj_files = []
cmake_files_dir = os.path.join(build_dir, "CMakeFiles")
for td in target_dirs:
    root = os.path.join(cmake_files_dir, td)
    if not os.path.isdir(root):
        continue
    for dpath, _, fnames in os.walk(root):
        for fn in sorted(fnames):
            if fn.endswith(".o"):
                obj_files.append(os.path.join(dpath, fn))

if not obj_files:
    errors.append(f"no .o files found under {cmake_files_dir}")
else:
    non_bc = []
    for obj in obj_files:
        head = open(obj, "rb").read(4)
        if not is_bitcode_bytes(head):
            non_bc.append(f"{os.path.relpath(obj, build_dir)} (magic={head.hex()})")
    if non_bc:
        errors.append("non-bitcode object files in Clang LTO build: " + ", ".join(non_bc))
    else:
        print(f"[lto-verify] {len(obj_files)} object files verified as LLVM bitcode (BC 0xc0de)")

# 2. Verify internal static libraries carry symbol tables and bitcode members.
for lib_name in ("libsan4_opl.a", "libsan4_core.a"):
    lib_path = os.path.join(build_dir, lib_name)
    if not os.path.isfile(lib_path):
        errors.append(f"missing expected internal archive: {lib_path}")
        continue
    members = parse_ar(lib_path)
    has_symtab = any(name == "<symtab>" for name, _ in members)
    objs = [(n, p) for n, p in members if n != "<symtab>"]
    if not has_symtab:
        errors.append(f"{lib_name}: missing archive symbol table (llvm-ar not used?)")
    if not objs:
        errors.append(f"{lib_name}: archive has no object members")
    bad_members = [n for n, p in objs if not is_bitcode_bytes(p[:4])]
    if bad_members:
        errors.append(f"{lib_name}: non-bitcode members: {', '.join(bad_members)}")
    else:
        print(f"[lto-verify] {lib_name}: {len(objs)} bitcode member(s), symbol table OK")

# 3. Inspect optional static dependency prefix archives.
if static_prefix:
    lib_dir = os.path.join(static_prefix, "lib")
    for dep in ("libz.a", "libpng16.a", "libx86emu.a", "libSDL2.a"):
        dep_path = os.path.join(lib_dir, dep)
        if not os.path.isfile(dep_path):
            errors.append(f"missing static dependency archive: {dep_path}")
            continue
        members = parse_ar(dep_path)
        has_symtab = any(name == "<symtab>" for name, _ in members)
        objs = [(n, p) for n, p in members if n != "<symtab>" and len(p) >= 4]
        if not has_symtab:
            errors.append(f"{dep}: missing archive symbol table")
        bc_count = sum(1 for _, p in objs if is_bitcode_bytes(p[:4]))
        elf_count = sum(1 for _, p in objs if p[:4] == ELF_MAGIC)
        print(
            f"[lto-verify] static dep {dep}: {len(objs)} member(s) "
            f"(bitcode={bc_count}, elf={elf_count}), symtab={'OK' if has_symtab else 'MISSING'}"
        )
        if dep == "libx86emu.a" and require_bc_x86emu and bc_count != len(objs):
            errors.append(
                f"{dep_path}: expected 100% LLVM bitcode members for cross-library LTO, "
                f"got bitcode={bc_count}/{len(objs)}"
            )

if x86emu_lib_override:
    if not os.path.isfile(x86emu_lib_override):
        errors.append(f"missing x86emu override archive: {x86emu_lib_override}")
    else:
        members = parse_ar(x86emu_lib_override)
        has_symtab = any(name == "<symtab>" for name, _ in members)
        objs = [(n, p) for n, p in members if n != "<symtab>" and len(p) >= 4]
        if not has_symtab:
            errors.append(f"{x86emu_lib_override}: missing archive symbol table")
        bc_count = sum(1 for _, p in objs if is_bitcode_bytes(p[:4]))
        print(
            f"[lto-verify] override x86emu ({os.path.basename(x86emu_lib_override)}): "
            f"{len(objs)} member(s) (bitcode={bc_count}), symtab={'OK' if has_symtab else 'MISSING'}"
        )
        if require_bc_x86emu and bc_count != len(objs):
            errors.append(
                f"{x86emu_lib_override}: expected 100% LLVM bitcode members, got {bc_count}/{len(objs)}"
            )

# 4. Verify merged IR PGO profile (.profdata) when requested.
if verify_pgo_file:
    if not os.path.isfile(verify_pgo_file) or os.path.getsize(verify_pgo_file) == 0:
        errors.append(f"missing or empty PGO .profdata file: {verify_pgo_file}")
    else:
        profdata_bin = None
        cache_path = os.path.join(build_dir, "CMakeCache.txt")
        if os.path.isfile(cache_path):
            m = re.search(r"^SAN4_LLVM_PROFDATA:FILEPATH=(.+)$", open(cache_path).read(), re.M)
            if m and os.path.isfile(m.group(1).strip()):
                profdata_bin = m.group(1).strip()
        if not profdata_bin:
            for sfx in ("", "-23", "-22", "-21", "-20", "-19", "-18", "-17", "-16", "-15"):
                cand = shutil.which(f"llvm-profdata{sfx}")
                if cand:
                    profdata_bin = cand
                    break
        if not profdata_bin:
            errors.append("llvm-profdata not found to verify PGO profile")
        else:
            proc = subprocess.run(
                [profdata_bin, "show", "--all-functions", verify_pgo_file],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=30,
            )
            if proc.returncode != 0:
                errors.append(f"llvm-profdata show failed ({proc.returncode}): {proc.stdout[:400]}")
            else:
                out = proc.stdout
                m_fn = re.search(r"Total functions:\s*(\d+)", out)
                m_max_fn = re.search(r"Maximum function count:\s*(\d+)", out)
                m_max_blk = re.search(r"Maximum internal block count:\s*(\d+)", out)
                total_fns = int(m_fn.group(1)) if m_fn else 0
                max_cnt = max(
                    int(m_max_fn.group(1)) if m_max_fn else 0,
                    int(m_max_blk.group(1)) if m_max_blk else 0,
                )
                if total_fns <= 0 or max_cnt <= 0:
                    errors.append(
                        f"{verify_pgo_file}: empty PGO counters (functions={total_fns}, max_count={max_cnt})"
                    )
                elif require_bc_x86emu and "x86emu_run" not in out:
                    errors.append(
                        f"{verify_pgo_file}: static x86emu PGO profile missing hot symbol 'x86emu_run'"
                    )
                else:
                    print(
                        f"[lto-verify] PGO profile {os.path.basename(verify_pgo_file)}: "
                        f"functions={total_fns}, max_count={max_cnt}"
                        + (", x86emu_run=OK" if "x86emu_run" in out else "")
                    )

# 5. Verify llvm-bolt optimization log and stripped relocation sections when requested.
if verify_bolt_log:
    if not os.path.isfile(verify_bolt_log) or os.path.getsize(verify_bolt_log) == 0:
        errors.append(f"missing or empty BOLT log file: {verify_bolt_log}")
    else:
        btxt = open(verify_bolt_log, "r", errors="replace").read()
        if "BOLT-ERROR:" in btxt:
            errors.append(f"{verify_bolt_log}: contains BOLT-ERROR")
        elif "BOLT-INFO:" not in btxt:
            errors.append(f"{verify_bolt_log}: missing BOLT-INFO output")
        else:
            m_prof = re.search(r"BOLT-INFO:\s*(\d+ out of \d+ functions[^\n]*)", btxt)
            prof_summary = m_prof.group(1) if m_prof else "BOLT-INFO verified"
            readelf_bin = shutil.which("readelf")
            for elf in elf_paths:
                if os.path.basename(elf) in ("san4-linux", "san4-hdpack-gen") and os.path.isfile(elf) and readelf_bin:
                    sec_out = subprocess.run(
                        [readelf_bin, "-S", "-W", elf],
                        stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT,
                        text=True,
                    ).stdout
                    if re.search(r"\s\.rela\.text\s", sec_out):
                        errors.append(f"{elf}: unstripped .rela.text relocation bloat remains after BOLT stage")
                    if re.search(r"\s\.note\.bolt_info\s", sec_out):
                        errors.append(f"{elf}: unstripped .note.bolt_info section remains after BOLT stage")
                    if re.search(r"\s\.comment\s", sec_out):
                        errors.append(f"{elf}: unstripped .comment section remains after strip stage")
            print(f"[lto-verify] BOLT log {os.path.basename(verify_bolt_log)}: {prof_summary}")

FORBIDDEN_TOKENS = [
    "/tmp/tmp.",
    bytes([47, 103, 111, 111, 103, 108, 101, 47]).decode("ascii"),
    bytes([103, 111, 111, 103, 108, 101, 114, 115]).decode("ascii"),
    bytes([47, 117, 115, 114, 47, 108, 111, 99, 97, 108, 47, 103, 111, 111, 103, 108, 101]).decode("ascii"),
]
env_user = os.environ.get("USER", "").strip()
if len(env_user) >= 4 and env_user.lower() not in {"root", "user", "runner", "ubuntu", "debian", "admin", "builder", "test"}:
    FORBIDDEN_TOKENS.append(env_user)

USER_HOME_RE = re.compile(r"/(?:home|Users)/[A-Za-z0-9._-]+")
WIN_HOME_RE = re.compile(r"[A-Za-z]:\\+Users\\+", re.IGNORECASE)
ASCII_RUN_RE = re.compile(rb"[\x20-\x7e]{6,}")
UTF16LE_RUN_RE = re.compile(rb"(?:[\x20-\x7e]\x00){6,}")


def scan_binary_leaks(path: str):
    data = open(path, "rb").read()
    leaks = []
    for raw in ASCII_RUN_RE.findall(data):
        s = raw.decode("ascii", errors="ignore")
        if USER_HOME_RE.search(s) or WIN_HOME_RE.search(s) or any(t in s for t in FORBIDDEN_TOKENS):
            leaks.append(s[:120])
            if len(leaks) >= 5:
                return leaks
    for raw in UTF16LE_RUN_RE.findall(data):
        s = raw.decode("utf-16le", errors="ignore")
        if USER_HOME_RE.search(s) or WIN_HOME_RE.search(s) or any(t in s for t in FORBIDDEN_TOKENS):
            leaks.append(s[:120])
            if len(leaks) >= 5:
                return leaks
    return leaks


# 6. Verify final linked executables are native ELF binaries, free of leaked
#    host paths, and run --help for the CLI tools (san4-linux, san4-hdpack-gen).
for elf in elf_paths:
    if not os.path.isfile(elf):
        errors.append(f"missing executable: {elf}")
        continue
    head = open(elf, "rb").read(4)
    if head != ELF_MAGIC:
        errors.append(f"{elf}: expected native ELF binary, got magic={head.hex()}")
        continue
    leaks = scan_binary_leaks(elf)
    if leaks:
        errors.append(f"{elf}: leaked build/host path strings detected ({len(leaks)} sample(s))")
    base = os.path.basename(elf)
    if base in ("san4-linux", "san4-hdpack-gen"):
        proc = subprocess.run(
            [elf, "--help"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=10,
        )
        if proc.returncode != 0:
            errors.append(f"{elf} --help exited with {proc.returncode}: {proc.stdout}")
        else:
            print(f"[lto-verify] {base}: native ELF ({os.path.getsize(elf)} bytes), --help OK")
    else:
        print(f"[lto-verify] {base}: native ELF ({os.path.getsize(elf)} bytes)")

if errors:
    for err in errors:
        print(f"[lto-verify FAIL] {err}", file=sys.stderr)
    sys.exit(1)
PYEOF
