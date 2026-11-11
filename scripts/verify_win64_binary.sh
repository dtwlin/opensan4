#!/usr/bin/env bash
set -euo pipefail

# Verifies that Windows x86_64 PE32+ executables produced by a SAN4_STATIC
# build are self-contained and Windows 11 compliant:
#   1. Valid PE32+ (AMD64, Magic 0x20b) headers with HIGH_ENTROPY_VA (0x0020),
#      DYNAMIC_BASE (0x0040), and NX_COMPAT (0x0100) DllCharacteristics.
#   2. Expected Subsystem: WINDOWS_GUI (2) for san4.exe, WINDOWS_CUI (3) for
#      san4-hdpack-gen.exe.
#   3. Embedded RT_MANIFEST (Type 24) resource on san4.exe declaring
#      PerMonitorV2 DPI awareness and UTF-8 activeCodePage.
#   4. Zero non-inbox DLL imports (normalizing case; rejecting libstdc++-6.dll,
#      libgcc_s_seh-1.dll, libwinpthread-1.dll, SDL2.dll, libpng16.dll, zlib1.dll).
#   5. When --verify-lto is passed: 100% of C/C++ object files (.obj/.o,
#      excluding .res COFF resource objects), internal archives
#      (libsan4_opl.a, libsan4_core.a), and static dependency archives
#      (libz.a, libpng16.a, libx86emu.a, libSDL2.a) contain LLVM bitcode
#      (BC 0xc0de) and valid archive symbol tables.

EXPECT_GUI=()
EXPECT_CUI=()
EXPECT_MANIFEST=()
VERIFY_LTO=0
BUILD_DIR=""
STATIC_PREFIX=""
PE_FILES=()

usage() {
  cat << 'EOF'
Usage: verify_win64_binary.sh [options] <pe_exe> [<pe_exe>...]

Options:
  --expect-gui <PE>       Require IMAGE_SUBSYSTEM_WINDOWS_GUI (2) on <PE>
  --expect-cui <PE>       Require IMAGE_SUBSYSTEM_WINDOWS_CUI (3) on <PE>
  --expect-manifest <PE>  Require embedded RT_MANIFEST with PerMonitorV2 + UTF-8 on <PE>
  --verify-lto            Verify 100% LLVM bitcode across CMake .obj files and .a archives
  --build-dir <DIR>       Build directory for --verify-lto
  --static-prefix <DIR>   Static dependency prefix for --verify-lto
  -h, --help              Show this help message and exit
EOF
}

while (( $# > 0 )); do
  case "$1" in
    --expect-gui)
      [[ $# -ge 2 ]] || { echo "error: --expect-gui requires an argument" >&2; exit 2; }
      EXPECT_GUI+=("$2")
      shift 2
      ;;
    --expect-gui=*)
      EXPECT_GUI+=("${1#*=}")
      shift
      ;;
    --expect-cui)
      [[ $# -ge 2 ]] || { echo "error: --expect-cui requires an argument" >&2; exit 2; }
      EXPECT_CUI+=("$2")
      shift 2
      ;;
    --expect-cui=*)
      EXPECT_CUI+=("${1#*=}")
      shift
      ;;
    --expect-manifest)
      [[ $# -ge 2 ]] || { echo "error: --expect-manifest requires an argument" >&2; exit 2; }
      EXPECT_MANIFEST+=("$2")
      shift 2
      ;;
    --expect-manifest=*)
      EXPECT_MANIFEST+=("${1#*=}")
      shift
      ;;
    --verify-lto)
      VERIFY_LTO=1
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
    --static-prefix)
      [[ $# -ge 2 ]] || { echo "error: --static-prefix requires an argument" >&2; exit 2; }
      STATIC_PREFIX="$2"
      shift 2
      ;;
    --static-prefix=*)
      STATIC_PREFIX="${1#*=}"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      PE_FILES+=("$1")
      shift
      ;;
  esac
done

if (( ${#PE_FILES[@]} == 0 )); then
  usage >&2
  exit 2
fi

python3 - "${VERIFY_LTO}" "${BUILD_DIR}" "${STATIC_PREFIX}" \
  "$(printf '%s\n' "${EXPECT_GUI[@]:-}")" \
  "$(printf '%s\n' "${EXPECT_CUI[@]:-}")" \
  "$(printf '%s\n' "${EXPECT_MANIFEST[@]:-}")" \
  "${PE_FILES[@]}" << 'PYEOF'
import os
import re
import shutil
import struct
import subprocess
import sys

ALLOWED_DLLS = {
    "advapi32.dll",
    "bcrypt.dll",
    "dinput8.dll",
    "gdi32.dll",
    "imm32.dll",
    "kernel32.dll",
    "msvcrt.dll",
    "ntdll.dll",
    "ole32.dll",
    "oleaut32.dll",
    "rpcrt4.dll",
    "setupapi.dll",
    "shell32.dll",
    "ucrtbase.dll",
    "user32.dll",
    "version.dll",
    "winmm.dll",
    "ws2_32.dll",
}

BC_RAW_MAGIC = b"BC\xc0\xde"
BC_WRAP_MAGIC = b"\xde\xc0\x17\x0b"
AR_MAGIC = b"!<arch>\n"


def is_allowed_dll(name: str) -> bool:
    low = name.lower()
    if low in ALLOWED_DLLS:
        return True
    if low.startswith("api-ms-win-") and low.endswith(".dll"):
        return True
    return False


def is_bitcode_bytes(buf: bytes) -> bool:
    return buf[:4] in (BC_RAW_MAGIC, BC_WRAP_MAGIC)


def find_llvm_readobj():
    for sfx in ("", "-23", "-22", "-21", "-20", "-19", "-18", "-17", "-16", "-15"):
        cand = shutil.which(f"llvm-readobj{sfx}")
        if cand:
            return cand
    return None


def parse_ar(path: str):
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


def rva_to_offset(rva: int, sections) -> int:
    for _, va, vsize, raw_ptr, raw_size in sections:
        span = max(vsize, raw_size)
        if va <= rva < va + span:
            off = raw_ptr + (rva - va)
            if off < raw_ptr + raw_size:
                return off
    return -1


def read_cstr(data: bytes, off: int) -> str:
    if off < 0 or off >= len(data):
        return ""
    end = data.find(b"\x00", off)
    if end == -1:
        end = len(data)
    return data[off:end].decode("latin1", errors="replace")


def extract_rt_manifests(data: bytes, rsrc_rva: int, rsrc_size: int, sections):
    """Walks the PE .rsrc directory tree and returns all Type 24 (RT_MANIFEST) payloads."""
    base_off = rva_to_offset(rsrc_rva, sections)
    if base_off < 0 or base_off + 16 > len(data):
        return []

    manifests = []

    def walk_dir(dir_off: int, depth: int, top_type):
        if dir_off < 0 or dir_off + 16 > len(data) or depth > 4:
            return
        num_named, num_id = struct.unpack_from("<HH", data, dir_off + 12)
        total = num_named + num_id
        entry_off = dir_off + 16
        for _ in range(total):
            if entry_off + 8 > len(data):
                break
            name_or_id, offset_to_data = struct.unpack_from("<II", data, entry_off)
            entry_off += 8
            cur_type = top_type
            if depth == 0:
                if (name_or_id & 0x80000000) == 0:
                    cur_type = name_or_id & 0xFFFF
                else:
                    cur_type = -1
            if offset_to_data & 0x80000000:
                sub_off = base_off + (offset_to_data & 0x7FFFFFFF)
                walk_dir(sub_off, depth + 1, cur_type)
            else:
                if cur_type == 24:
                    data_entry_off = base_off + offset_to_data
                    if data_entry_off + 16 <= len(data):
                        data_rva, data_sz, _, _ = struct.unpack_from("<IIII", data, data_entry_off)
                        raw_off = rva_to_offset(data_rva, sections)
                        if raw_off >= 0 and raw_off + data_sz <= len(data):
                            manifests.append(data[raw_off : raw_off + data_sz])

    walk_dir(base_off, 0, None)
    return manifests


verify_lto = sys.argv[1] == "1"
build_dir = sys.argv[2]
static_prefix = sys.argv[3]
expect_gui = {os.path.abspath(p) for p in sys.argv[4].splitlines() if p.strip()}
expect_cui = {os.path.abspath(p) for p in sys.argv[5].splitlines() if p.strip()}
expect_manifest = {os.path.abspath(p) for p in sys.argv[6].splitlines() if p.strip()}
pe_files = sys.argv[7:]

readobj_bin = find_llvm_readobj()
errors = []

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


def scan_binary_leaks(data: bytes):
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

for path in pe_files:
    abs_path = os.path.abspath(path)
    if not os.path.isfile(path):
        errors.append(f"{path}: file does not exist")
        continue
    data = open(path, "rb").read()
    if len(data) < 0x40 or data[:2] != b"MZ":
        errors.append(f"{path}: missing MZ DOS header")
        continue
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if e_lfanew + 24 > len(data) or data[e_lfanew : e_lfanew + 4] != b"PE\x00\x00":
        errors.append(f"{path}: missing PE\\0\\0 signature")
        continue

    machine, num_sections, _, _, _, opt_hdr_size, _ = struct.unpack_from(
        "<HHIIIHH", data, e_lfanew + 4
    )
    if machine != 0x8664:
        errors.append(f"{path}: expected Machine=0x8664 (AMD64), got 0x{machine:04x}")
        continue

    opt_off = e_lfanew + 24
    if opt_off + opt_hdr_size > len(data) or opt_hdr_size < 112:
        errors.append(f"{path}: truncated OptionalHeader (size={opt_hdr_size})")
        continue

    magic = struct.unpack_from("<H", data, opt_off)[0]
    if magic != 0x020B:
        errors.append(f"{path}: expected PE32+ Magic=0x20b, got 0x{magic:04x}")
        continue

    subsystem, dll_chars = struct.unpack_from("<HH", data, opt_off + 68)
    stack_reserve = struct.unpack_from("<Q", data, opt_off + 72)[0]
    num_rva = struct.unpack_from("<I", data, opt_off + 108)[0]

    data_dirs = []
    for i in range(min(num_rva, 16)):
        dd_off = opt_off + 112 + i * 8
        if dd_off + 8 <= opt_off + opt_hdr_size:
            data_dirs.append(struct.unpack_from("<II", data, dd_off))
        else:
            data_dirs.append((0, 0))

    sec_table_off = opt_off + opt_hdr_size
    sections = []
    for i in range(num_sections):
        sh_off = sec_table_off + i * 40
        if sh_off + 40 > len(data):
            break
        raw_name = data[sh_off : sh_off + 8].rstrip(b"\x00").decode("latin1", errors="replace")
        vsize, va, raw_size, raw_ptr = struct.unpack_from("<IIII", data, sh_off + 8)
        sections.append((raw_name, va, vsize, raw_ptr, raw_size))

    # Verify Windows 11 DllCharacteristics: HIGH_ENTROPY_VA (0x0020),
    # DYNAMIC_BASE (0x0040), NX_COMPAT (0x0100).
    required_dll_chars = (
        (0x0020, "HIGH_ENTROPY_VA"),
        (0x0040, "DYNAMIC_BASE"),
        (0x0100, "NX_COMPAT"),
    )
    missing_chars = [name for bit, name in required_dll_chars if not (dll_chars & bit)]
    if missing_chars:
        errors.append(
            f"{path}: missing DllCharacteristics ({', '.join(missing_chars)}), "
            f"got 0x{dll_chars:04x}"
        )

    # Verify PE32+ SizeOfStackReserve >= 8 MB (8388608 bytes) so large host/emulator
    # state objects never trigger STATUS_STACK_OVERFLOW (0xC00000FD) on Windows.
    if stack_reserve < 8 * 1024 * 1024:
        errors.append(
            f"{path}: SizeOfStackReserve ({stack_reserve} bytes) is less than 8 MB (8388608 bytes)"
        )

    # Verify Subsystem when requested.
    if abs_path in expect_gui and subsystem != 2:
        errors.append(
            f"{path}: expected Subsystem=2 (WINDOWS_GUI), got {subsystem}"
        )
    if abs_path in expect_cui and subsystem != 3:
        errors.append(
            f"{path}: expected Subsystem=3 (WINDOWS_CUI), got {subsystem}"
        )

    # Parse Import Directory (DataDirectory[1]).
    imported_dlls = []
    if len(data_dirs) > 1 and data_dirs[1][0] != 0:
        imp_rva, _ = data_dirs[1]
        imp_off = rva_to_offset(imp_rva, sections)
        while imp_off >= 0 and imp_off + 20 <= len(data):
            ilt, ts, fwd, name_rva, iat = struct.unpack_from("<IIIII", data, imp_off)
            if ilt == 0 and ts == 0 and fwd == 0 and name_rva == 0 and iat == 0:
                break
            dll_name = read_cstr(data, rva_to_offset(name_rva, sections))
            if dll_name:
                imported_dlls.append(dll_name)
            imp_off += 20

    # Cross-check with llvm-readobj --coff-imports when available.
    if readobj_bin:
        ro_out = subprocess.run(
            [readobj_bin, "--coff-imports", path],
            check=False,
            capture_output=True,
            text=True,
        ).stdout
        for m in re.finditer(r"^\s*Name:\s*(\S+)\s*$", ro_out, re.M):
            dll = m.group(1)
            if dll.lower() not in {d.lower() for d in imported_dlls}:
                imported_dlls.append(dll)

    bad_dlls = [d for d in imported_dlls if not is_allowed_dll(d)]
    subsys_str = {2: "WINDOWS_GUI", 3: "WINDOWS_CUI"}.get(subsystem, f"SUBSYS_{subsystem}")
    print(f"[win64-verify] {path} ({os.path.getsize(path)} bytes)")
    print(
        f"[win64-verify]   PE32+ AMD64, Subsystem={subsys_str} ({subsystem}), "
        f"DllCharacteristics=0x{dll_chars:04x}, StackReserve={stack_reserve}"
    )
    print(
        f"[win64-verify]   Imports ({len(imported_dlls)}): "
        + (" ".join(imported_dlls) if imported_dlls else "(none)")
    )
    if bad_dlls:
        errors.append(
            f"{path}: non-inbox DLL dependencies: {' '.join(bad_dlls)}"
        )

    leaks = scan_binary_leaks(data)
    if leaks:
        errors.append(
            f"{path}: leaked build/host path strings detected ({len(leaks)} sample(s))"
        )

    # Verify embedded RT_MANIFEST resource when requested.
    if abs_path in expect_manifest:
        if len(data_dirs) <= 2 or data_dirs[2][0] == 0:
            errors.append(f"{path}: missing .rsrc DataDirectory for RT_MANIFEST")
        else:
            rsrc_rva, rsrc_size = data_dirs[2]
            manifests = extract_rt_manifests(data, rsrc_rva, rsrc_size, sections)
            if not manifests:
                errors.append(f"{path}: no RT_MANIFEST (Type 24) found in .rsrc")
            else:
                xml_text = b"\n".join(manifests).decode("utf-8", errors="replace")
                if any(b"\x00" in m for m in manifests):
                    errors.append(f"{path}: RT_MANIFEST contains NUL byte padding (breaks Windows SxS parser)")
                if "PerMonitorV2" not in xml_text:
                    errors.append(f"{path}: RT_MANIFEST missing PerMonitorV2 DPI awareness")
                if "UTF-8" not in xml_text:
                    errors.append(f"{path}: RT_MANIFEST missing UTF-8 activeCodePage")
                if "PerMonitorV2" in xml_text and "UTF-8" in xml_text and not any(b"\x00" in m for m in manifests):
                    print(
                        f"[win64-verify]   RT_MANIFEST: OK ({len(manifests[0])} bytes, "
                        "PerMonitorV2 + UTF-8 verified)"
                    )

if verify_lto:
    if not build_dir:
        errors.append("--verify-lto requires --build-dir")
    else:
        target_dirs = [
            "san4_opl.dir",
            "san4_core.dir",
            "san4-linux.dir",
            "san4-hdpack-gen.dir",
            "san4_tests.dir",
        ]
        obj_files = []
        skipped_res = 0
        cmake_files_dir = os.path.join(build_dir, "CMakeFiles")
        for td in target_dirs:
            root = os.path.join(cmake_files_dir, td)
            if not os.path.isdir(root):
                continue
            for dpath, _, fnames in os.walk(root):
                for fn in sorted(fnames):
                    if fn.endswith(".res"):
                        skipped_res += 1
                        continue
                    if fn.endswith(".obj") or fn.endswith(".o"):
                        obj_files.append(os.path.join(dpath, fn))

        if not obj_files:
            errors.append(f"no .obj/.o files found under {cmake_files_dir}")
        else:
            non_bc = []
            for obj in obj_files:
                head = open(obj, "rb").read(4)
                if not is_bitcode_bytes(head):
                    non_bc.append(f"{os.path.relpath(obj, build_dir)} (magic={head.hex()})")
            if non_bc:
                errors.append("non-bitcode object files in Win64 Clang LTO build: " + ", ".join(non_bc))
            else:
                print(
                    f"[win64-verify] LTO objects: {len(obj_files)} .obj/.o verified as LLVM bitcode "
                    f"(BC 0xc0de; skipped {skipped_res} .res resource object(s))"
                )

        for lib_name in ("libsan4_opl.a", "libsan4_core.a"):
            lib_path = os.path.join(build_dir, lib_name)
            if not os.path.isfile(lib_path):
                errors.append(f"missing internal archive: {lib_path}")
                continue
            members = parse_ar(lib_path)
            has_symtab = any(name == "<symtab>" for name, _ in members)
            objs = [(n, p) for n, p in members if n != "<symtab>"]
            if not has_symtab:
                errors.append(f"{lib_name}: missing archive symbol table")
            bad_members = [n for n, p in objs if not is_bitcode_bytes(p[:4])]
            if bad_members:
                errors.append(f"{lib_name}: non-bitcode members: {', '.join(bad_members)}")
            else:
                print(f"[win64-verify] {lib_name}: {len(objs)} bitcode member(s), symtab OK")

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
            print(
                f"[win64-verify] static dep {dep}: {bc_count}/{len(objs)} bitcode member(s), "
                f"symtab={'OK' if has_symtab else 'MISSING'}"
            )
            if bc_count != len(objs):
                errors.append(
                    f"{dep_path}: expected 100% LLVM bitcode members, got {bc_count}/{len(objs)}"
                )

if errors:
    for err in errors:
        print(f"[win64-verify FAIL] {err}", file=sys.stderr)
    sys.exit(1)
PYEOF
