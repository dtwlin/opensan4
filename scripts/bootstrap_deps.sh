#!/usr/bin/env bash
set -euo pipefail

# Rootless dependency bootstrapper for environments where libsdl2-dev or
# libx86emu-dev are not installed system-wide, and one-time extractor for
# GPL-2.0-or-later DOSBox DBOPL sources if not yet present in src/opl/.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
TP_DIR="${ROOT_DIR}/third_party"
MULTIARCH="$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || echo x86_64-linux-gnu)"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT

mkdir -p "${TP_DIR}" "${ROOT_DIR}/src/opl"

cd "${WORK_DIR}"

if [[ ! -f "/usr/include/SDL2/SDL.h" && ! -f "${TP_DIR}/sdl2_dev/usr/include/SDL2/SDL.h" ]]; then
  echo "[bootstrap] Downloading rootless libsdl2-dev headers..."
  apt-get download libsdl2-dev >/dev/null
  mkdir -p "${TP_DIR}/sdl2_dev"
  for deb in libsdl2-dev_*.deb; do
    [[ -e "${deb}" ]] && dpkg-deb -x "${deb}" "${TP_DIR}/sdl2_dev"
  done
  mkdir -p "${TP_DIR}/sdl2_dev/lib"
  if [[ -e "/usr/lib/${MULTIARCH}/libSDL2-2.0.so.0" ]]; then
    ln -sf "/usr/lib/${MULTIARCH}/libSDL2-2.0.so.0" "${TP_DIR}/sdl2_dev/lib/libSDL2.so"
  else
    echo "[bootstrap] System libSDL2-2.0.so.0 absent; downloading rootless libsdl2-2.0-0..."
    apt-get download libsdl2-2.0-0 >/dev/null
    for deb in libsdl2-2.0-0_*.deb; do
      [[ -e "${deb}" ]] && dpkg-deb -x "${deb}" "${TP_DIR}/sdl2_dev"
    done
    if [[ -e "${TP_DIR}/sdl2_dev/usr/lib/${MULTIARCH}/libSDL2-2.0.so.0" ]]; then
      ln -sf "${TP_DIR}/sdl2_dev/usr/lib/${MULTIARCH}/libSDL2-2.0.so.0" "${TP_DIR}/sdl2_dev/lib/libSDL2-2.0.so.0"
      ln -sf "${TP_DIR}/sdl2_dev/usr/lib/${MULTIARCH}/libSDL2-2.0.so.0" "${TP_DIR}/sdl2_dev/lib/libSDL2.so"
    fi
  fi
fi

if [[ ! -f "/usr/include/x86emu.h" && ! -f "${TP_DIR}/x86emu/usr/include/x86emu.h" ]]; then
  echo "[bootstrap] Downloading rootless libx86emu-dev & libx86emu3..."
  apt-get download libx86emu-dev libx86emu3 >/dev/null
  mkdir -p "${TP_DIR}/x86emu"
  for deb in libx86emu*.deb; do
    [[ -e "${deb}" ]] && dpkg-deb -x "${deb}" "${TP_DIR}/x86emu"
  done
  mkdir -p "${TP_DIR}/x86emu/lib"
  for so in "${TP_DIR}/x86emu"/usr/lib/*/libx86emu.so*; do
    [[ -e "${so}" ]] && ln -sf "${so}" "${TP_DIR}/x86emu/lib/$(basename "${so}")"
  done
fi

if [[ ! -f "${ROOT_DIR}/src/opl/dbopl.cpp" || ! -f "${ROOT_DIR}/src/opl/dbopl.h" ]]; then
  echo "[bootstrap] Extracting GPL-2.0-or-later DBOPL from dosbox source package..."
  apt-get source dosbox >/dev/null
  shopt -s nullglob
  dosbox_hw_dirs=( "${WORK_DIR}"/dosbox-*/src/hardware )
  shopt -u nullglob
  if (( ${#dosbox_hw_dirs[@]} == 0 )); then
    echo "[bootstrap] ERROR: extracted dosbox hardware directory not found" >&2
    exit 1
  fi
  DOSBOX_SRC="${dosbox_hw_dirs[0]}"
  python3 - "${DOSBOX_SRC}" "${ROOT_DIR}/src/opl" << 'PYEOF'
import os, re, sys

src_dir, dst_dir = sys.argv[1], sys.argv[2]

h_txt = open(os.path.join(src_dir, "dbopl.h"), "r", encoding="utf-8", errors="replace").read()
h_txt = h_txt.replace('#include "adlib.h"', '#pragma once')
h_txt = h_txt.replace('#include "dosbox.h"', '#include "dbopl_shim.h"')
idx = h_txt.find("struct Handler : public Adlib::Handler")
if idx != -1:
    h_txt = h_txt[:idx] + "void InitTables();\n\n}  // namespace DBOPL\n"
open(os.path.join(dst_dir, "dbopl.h"), "w", encoding="utf-8").write(h_txt)

c_txt = open(os.path.join(src_dir, "dbopl.cpp"), "r", encoding="utf-8", errors="replace").read()
c_txt = c_txt.replace('#include "dosbox.h"', '#include "dbopl_shim.h"')
for hdr in ["inout.h", "mixer.h", "pic.h", "setup.h", "hardware.h", "mem.h"]:
    c_txt = c_txt.replace(f'#include "{hdr}"', '')
c_txt = c_txt.replace("static void InitTables( void )", "void InitTables( void )")
idx = c_txt.find("Bit32u Handler::WriteAddr")
if idx != -1:
    c_txt = c_txt[:idx] + "}  // namespace DBOPL\n"

# Replace null-pointer dereference UB in InitTables() with stack dummy_chip math
old_pat = re.compile(
    r"DBOPL::Chip\*\s*chip\s*=\s*0;.*?OpOffsetTable\[i\]\s*=\s*ChanOffsetTable\[\s*chNum\s*\]\s*\+\s*blah;\s*\}",
    re.DOTALL,
)
clean_tables = """DBOPL::Chip dummy_chip;
\tconst unsigned char* chip_base = reinterpret_cast<const unsigned char*>(&dummy_chip);
\tfor ( Bitu i = 0; i < 32; i++ ) {
\t\tBitu index = i & 0xf;
\t\tif ( index >= 9 ) {
\t\t\tChanOffsetTable[i] = 0;
\t\t\tcontinue;
\t\t}
\t\tif ( index < 6 ) {
\t\t\tindex = (index % 3) * 2 + ( index / 3 );
\t\t}
\t\tif ( i >= 16 )
\t\t\tindex += 9;
\t\tconst unsigned char* chan_ptr = reinterpret_cast<const unsigned char*>(&dummy_chip.chan[ index ]);
\t\tChanOffsetTable[i] = static_cast<Bit16u>(chan_ptr - chip_base);
\t}
\tfor ( Bitu i = 0; i < 64; i++ ) {
\t\tif ( i % 8 >= 6 || ( (i / 8) % 4 == 3 ) ) {
\t\t\tOpOffsetTable[i] = 0;
\t\t\tcontinue;
\t\t}
\t\tBitu chNum = (i / 8) * 3 + (i % 8) % 3;
\t\tif ( chNum >= 12 )
\t\t\tchNum += 16 - 12;
\t\tBitu opNum = ( i % 8 ) / 3;
\t\tBitu chanIdx = chNum & 0xf;
\t\tif ( chanIdx < 6 ) {
\t\t\tchanIdx = (chanIdx % 3) * 2 + ( chanIdx / 3 );
\t\t}
\t\tif ( chNum >= 16 )
\t\t\tchanIdx += 9;
\t\tconst unsigned char* op_ptr = reinterpret_cast<const unsigned char*>(&dummy_chip.chan[chanIdx].op[opNum]);
\t\tOpOffsetTable[i] = static_cast<Bit16u>(op_ptr - chip_base);
\t}"""
c_txt, n_rep = old_pat.subn(clean_tables, c_txt, count=1)
if n_rep != 1:
    raise RuntimeError("Failed to patch DBOPL::InitTables null-pointer dereference")

c_txt = c_txt.replace(
    "Chip::Chip() {\n\treg08 = 0;",
    "Chip::Chip() {\n\tlfoCounter = 0;\n\tlfoAdd = 0;\n\tnoiseCounter = 0;\n\tnoiseAdd = 0;\n\tnoiseValue = 1;\n\tmemset( freqMul, 0, sizeof( freqMul ) );\n\tmemset( linearRates, 0, sizeof( linearRates ) );\n\tmemset( attackRates, 0, sizeof( attackRates ) );\n\tvibratoIndex = 0;\n\ttremoloIndex = 0;\n\tvibratoSign = 0;\n\tvibratoShift = 0;\n\ttremoloValue = 0;\n\tvibratoStrength = 0;\n\ttremoloStrength = 0;\n\twaveFormMask = 0;\n\treg08 = 0;",
)
c_txt = c_txt.replace(
    "Operator::Operator() {\n\tchanData = 0;",
    "Operator::Operator() {\n#if (DBOPL_WAVE != WAVE_HANDLER)\n\twaveBase = WaveTable;\n\twaveMask = 1023;\n\twaveStart = 0;\n#endif\n\tvibrato = 0;\n\tattackAdd = 0;\n\tdecayAdd = 0;\n\trateIndex = 0;\n\ttremoloMask = 0;\n\tvibStrength = 0;\n\tchanData = 0;",
)
c_txt = c_txt.replace(
    "Bit32s mul = ((original - diff) << 12) / original;\n\t\t\t\tguessAdd = ((guessAdd * mul) >> 12);",
    "Bit64s mul = (static_cast<Bit64s>(original - diff) << 12) / original;\n\t\t\t\tguessAdd = static_cast<Bit32s>((static_cast<Bit64s>(guessAdd) * mul) >> 12);",
)
c_txt = c_txt.replace(
    "Bit32s mul = ((original - diff) << 12) / original;\n\t\t\t\tguessAdd = (guessAdd * mul) >> 12;",
    "Bit64s mul = (static_cast<Bit64s>(original - diff) << 12) / original;\n\t\t\t\tguessAdd = static_cast<Bit32s>((static_cast<Bit64s>(guessAdd) * mul) >> 12);",
)

open(os.path.join(dst_dir, "dbopl.cpp"), "w", encoding="utf-8").write(c_txt)
PYEOF
fi

echo "[bootstrap] Dependencies ready."
