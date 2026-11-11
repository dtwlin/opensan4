#!/usr/bin/env bash
set -euo pipefail

# Verifies that the repository contains zero proprietary game files, binary
# package archives, dumped game sprites, or machine-specific local paths.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

python3 - "${ROOT_DIR}" << 'PYEOF'
import os, re, sys

root = os.path.abspath(sys.argv[1])
skip_dirs = {".git", "build", "third_party", "game", "hd_pack", "dumped_sprites"}
forbidden_exts = {".exe", ".com", ".s4", ".16p", ".sav", ".bak", ".deb"}
# Construct forbidden path tokens dynamically from byte arrays so this script
# itself contains zero sensitive substrings.
forbidden_tokens = [
    bytes([47, 103, 111, 111, 103, 108, 101, 47]).decode("ascii"),
    bytes([103, 111, 111, 103, 108, 101, 114, 115]).decode("ascii"),
    bytes([47, 117, 115, 114, 47, 108, 111, 99, 97, 108, 47, 103, 111, 111, 103, 108, 101]).decode("ascii"),
]
env_user = os.environ.get("USER", "").strip()
generic_users = {"root", "user", "runner", "ubuntu", "debian", "admin", "builder", "test"}
if len(env_user) >= 4 and env_user.lower() not in generic_users:
    forbidden_tokens.append(env_user)

user_home_re = re.compile(r"/(?:home|Users)/[A-Za-z0-9._-]+")
win_user_home_re = re.compile(r"[A-Za-z]:\\+Users\\+", re.IGNORECASE)

errors = []


def check_text_file(full_path: str, label: str, check_elf: bool = True) -> None:
    try:
        data = open(full_path, "rb").read()
    except OSError:
        return
    if check_elf and len(data) >= 4 and data[0] == 127 and data[1:4] == b"ELF":
        errors.append(f"Compiled ELF binary in repo tree: {label}")
        return
    text = data.decode("utf-8", errors="ignore")
    if user_home_re.search(text) or win_user_home_re.search(text):
        errors.append(f"Hardcoded user home path in {label}")
        return
    for tok in forbidden_tokens:
        if tok and tok in text:
            errors.append(f"Machine-specific or internal path token in {label}")
            return


for dirpath, dirnames, filenames in os.walk(root):
    dirnames[:] = [
        d for d in dirnames
        if d not in skip_dirs and not d.startswith("build-")
    ]
    for fn in filenames:
        full = os.path.join(dirpath, fn)
        rel = os.path.relpath(full, root)
        _, ext = os.path.splitext(fn)
        if ext.lower() in forbidden_exts:
            errors.append(f"Forbidden proprietary/binary file in repo: {rel}")
            continue
        if fn.startswith("img_") and ext.lower() == ".png":
            errors.append(f"Dumped game sprite PNG in repo: {rel}")
            continue
        check_text_file(full, rel, check_elf=True)

sibling_launcher = os.path.join(os.path.dirname(root), "san4-native")
if os.path.isfile(sibling_launcher):
    check_text_file(sibling_launcher, "../san4-native", check_elf=True)

if errors:
    for e in errors:
        print(f"[clean-repo FAIL] {e}", file=sys.stderr)
    sys.exit(1)

print("[clean-repo OK] Zero proprietary files or host-specific paths found.")
PYEOF
