#!/usr/bin/env bash
set -euo pipefail

# Verifies that ELF executables produced by a SAN4_STATIC build are portable:
# only glibc core libraries may appear in DT_NEEDED, no RPATH/RUNPATH may be
# recorded, and the minimum glibc symbol version required is reported.
#
# Usage: verify_static_binary.sh <elf> [<elf>...]

if (( $# == 0 )); then
  echo "usage: $0 <elf> [<elf>...]" >&2
  exit 2
fi
for tool in readelf objdump; do
  command -v "${tool}" >/dev/null || { echo "[static-verify] ${tool} not found" >&2; exit 2; }
done

python3 - "$@" << 'PYEOF'
import os, re, subprocess, sys

# glibc core sonames that every Linux distribution provides. libpthread/libdl
# are tolerated because pre-2.34 glibc toolchains cannot avoid them.
allowed = {
    "libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2",
    "ld-linux-x86-64.so.2", "ld-linux-aarch64.so.1", "ld-linux.so.2",
}

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


def run(*cmd):
    # Fully static executables have no dynamic symbol table; objdump -T then
    # exits non-zero, which simply means "no glibc symbol versions".
    return subprocess.run(cmd, check=False, capture_output=True, text=True).stdout

ok = True
for path in sys.argv[1:]:
    dyn = run("readelf", "-d", "-W", path)
    needed = re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", dyn)
    rpaths = re.findall(r"\((?:RPATH|RUNPATH)\)\s+Library \w+: \[([^\]]*)\]", dyn)
    versions = set(re.findall(r"GLIBC_(\d+(?:\.\d+)+)", run("objdump", "-T", path)))
    glibc_min = max(versions, key=lambda v: tuple(int(x) for x in v.split("."))) if versions else "none"
    bad = [n for n in needed if n not in allowed]
    leaks = scan_binary_leaks(path)
    print(f"[static-verify] {path}")
    print(f"[static-verify]   NEEDED: {' '.join(needed) if needed else '(none)'}")
    print(f"[static-verify]   minimum glibc: {glibc_min}")
    if bad:
        ok = False
        print(f"[static-verify]   FAIL non-glibc dependencies: {' '.join(bad)}", file=sys.stderr)
    if rpaths:
        ok = False
        print(f"[static-verify]   FAIL embedded RPATH/RUNPATH: {' '.join(rpaths)}", file=sys.stderr)
    if leaks:
        ok = False
        print(f"[static-verify]   FAIL leaked build/host path strings ({len(leaks)} sample(s))", file=sys.stderr)

sys.exit(0 if ok else 1)
PYEOF
