#!/usr/bin/env python3
"""Generates ${CMAKE_BINARY_DIR}/generated/embedded_game_data.cpp from a verified
43-file Traditional Chinese DOS SAN4 directory for personal standalone builds.

Strictly embeds ONLY the 43 official manifest files (never SAVEDAT.S4 or stray
directory entries) and emits ZERO host paths into the generated C++ source.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import sys
import zlib

# Exact 43 official Traditional Chinese DOS SAN4 files (sorted by DOS 8.3 name).
MANIFEST: tuple[tuple[str, int, str], ...] = (
    ("EDMUSIC.S4", 8706, "8d5df99f4984564fc129c09e5d32f92057cc43662c06a2bb30f888aa6a050ec2"),
    ("END.EXE", 65787, "faff2e045885b96f08bfaf6ae97f2143b5b4040cd58650c1bedb362bfc20f8b1"),
    ("ENDDAT.S4", 1120708, "6e66190af9e434b48a460d8b7ff2124f0b512209e4a1395198a42166af71b323"),
    ("ENDSTAFF.S4", 4002, "759e5d10b5c21bbe48812c22ab7dc0890b530792208b04296c95074a3c42e874"),
    ("FMDRV.COM", 5469, "803becf535bcf025447f60418573628dc4cdf69094ceaea5c16d377b27c7ddf0"),
    ("FONT.S4", 2880, "bf5ab619dae4ad8f58bd77f9031d3b4350c9b190385fcdf7c2b43f81fd281d9d"),
    ("GRAPH.S4", 15391, "be0ca58152a021e688b010f7c14b2f5b3b6a5c737ebdbc49287bf1981a479eb4"),
    ("GRP600C.EXE", 24849, "1f33eef9ddfd8d5437ceed8dd6bda1cf396c3ebdf7b95f590c9558f3acdf591d"),
    ("HEGRP.S4", 20804, "f27b9b26bc380d019a01a572946b1dbdd082bb5b2f5784dc80181b61c2b2d931"),
    ("HEXGRP.S4", 43793, "b9fd020901458d453a150be42b1f6a2c44f568efa296c437b3f7bccdaec5ba90"),
    ("HEXMAP.S4", 64465, "acae6e7f2696ffd2a137644a3f7bd5f76275827b21cd81e32db1269cb13965cb"),
    ("HIBACK.S4", 114563, "f8b0a3aeab8a2f9922fc8ecc771ef585e96a332f64418600d444a53b3b29dbeb"),
    ("HIGRP.S4", 54092, "ae9c46ce3573340a0516598c58e2ab0b28de9b4fed358250aad0cc221a6fc668"),
    ("HKGRP.S4", 34647, "99d479513bc976c99e5da6c7731bfff8ee954fcd4491ed74ff40500508aef2e6"),
    ("HKMAP.S4", 35112, "9b8b7f709c4f0fa2127c51db5cdb9ad8628cc8d290e3baf5024986fed3f3d47e"),
    ("HUNIT.S4", 17741, "17ddce26809816788f62c310748ad756127cb136fb41256de81cb7f80391abd5"),
    ("HWGRP.S4", 15872, "c1e78ca2e6d5edcd608c422ef7be1a719a725f69ff7f35d10325b39369e1e9bd"),
    ("KAODATA.S4", 530413, "fd1c968a058cd293ee62eaee3ea1a2bdf2a381438f36fb228df2e5f28d8c7356"),
    ("LOGO.S4", 9547, "f4681804b94d6670cb796f716f58422f7892d12b843ce7b4ff32fd65c38f838c"),
    ("MAIN.16P", 56980, "53fe6e53ef5c39956d3561d71ce0ce107bf17b14963bc6fab7f44af8cadaf04d"),
    ("MAIN.EXE", 598974, "78c54faa8714dfbd4910a15c07a57da09944ed1baaec261bea8478023f71234e"),
    ("MAINCG.S4", 59905, "786c8cc6b7b7cd1956af79513a12928efe63f32b3b5572d1508aa3e1aee01bf5"),
    ("MAINMAP.S4", 21804, "b60fab29583c38b2092db779c2dfa6c35b11f08fc4afa4a92e19bcb9edd4517e"),
    ("MESSAGE.S4", 56328, "c14869649065cae2a32ab2fea0d5db01240308316dd3bf5b36c68cba8ea566ca"),
    ("MONDATA.S4", 190464, "01df172a3f086e8e6ed8307190c16f1ff3add2190d7d9de5ebd927729856eff8"),
    ("MUSIC.S4", 38824, "e7afa4fc4bda06d698d87e0d17404d0115948114549c9b53c0652680e751c81f"),
    ("NAME.16P", 280028, "cdebf4d4bf6783cf59776ae9570b1c3a349ed2e4fd26a4425562c9f9594def27"),
    ("NBDATA.S4", 17496, "3bd2d1a30ea5f2e9b8ec2627213970ed9988af8df3062df439d25e2952e54369"),
    ("OPED.16P", 21448, "a77e7d308624f3bec2a8f5393b786ba218e6f761ad069ff7e1437a333c304531"),
    ("OPEN.EXE", 53731, "e779dab95a3392de27c839a345223c32a340422fe0a539e900575dc9d40b869c"),
    ("OPENDAT.S4", 530761, "587a54156a6b3443a22d56050fc226bf73aaa0c921329fba1f34aba381b58b3d"),
    ("OPMUSIC.S4", 13635, "80c5f3a08cf80efdd9f1473cd7871a2927e0e870e9a8ddd18d3a030a5764e09b"),
    ("PACKGRP.S4", 342738, "bb5d05b968853cfbe8a9bf1a980070bc327af55919898731c6fe70a00a8ff137"),
    ("PACKGRP2.S4", 70785, "5c886f1ab109ad061977a50c38d8ad34d1883d5c1ef3fc5995acfa2b27102e28"),
    ("PACKGRP3.S4", 119142, "20f7b5e3de94203c634e7a1c6fe4c26be9bc3de0fe07677225d17304efff6daa"),
    ("PREGRP.S4", 107541, "a4993d73b4197d8ab08d0e7b0346d45873e769d8683ca31f57091571ba1490c9"),
    ("PREMSG.S4", 3669, "881acf082a39a939d566e09e389a65a4958089417ced2672258ea6cc36972405"),
    ("REVGRP.S4", 71234, "415b50b64905309eebef0728041bb38f1bfb0e3d90e7ae9d8797dc60ea783a1b"),
    ("SAN486.COM", 1934, "8bb3d79630b02b8c64406a5fd85d3d4d53242a51f19e424f99b61e6a3e323d8d"),
    ("SNDATA.S4", 59930, "423e7cbcec2c2642a51aaeec3772e39d2492ff857d6f57d70386289dfab944c7"),
    ("TAIKI.S4", 36461, "91b83544940437b32a3b0a29fd87303fdbc75bd141ca03cdeec954e2b23ebd73"),
    ("TOSHICG.S4", 31679, "1add0e2347926c57693dc443bb1fad0ea10fb172e79c1bef52ca3cee7a2b113f"),
    ("TOSHIMAP.S4", 9514, "d536cfd56db861325487d35185df3046204df9a9aa1fbbe1d5f52c0cd043b13e"),
)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Pack verified 43 SAN4 files into C++ embedded VFS source."
    )
    parser.add_argument("--game-dir", required=True, help="Input SAN4 directory")
    parser.add_argument("--output", required=True, help="Output .cpp path")
    args = parser.parse_args()

    if not os.path.isdir(args.game_dir):
        print(f"[embed-vfs] ERROR: not a directory: {args.game_dir}", file=sys.stderr)
        return 1

    upper_map: dict[str, str] = {}
    for entry in sorted(os.listdir(args.game_dir)):
        full = os.path.join(args.game_dir, entry)
        if os.path.isfile(full) and not os.path.islink(full):
            upper_map[entry.upper()] = full

    chunks: list[bytes] = []
    descs: list[tuple[str, int, int]] = []
    errors: list[str] = []
    offset = 0

    for exp_name, exp_size, exp_sha256 in MANIFEST:
        path = upper_map.get(exp_name)
        if path is None:
            errors.append(f"Missing required game file: {exp_name}")
            continue
        data = open(path, "rb").read()
        if len(data) != exp_size:
            errors.append(
                f"Size mismatch on {exp_name}: got {len(data)}, expected {exp_size}"
            )
            continue
        if exp_name != "NBDATA.S4":
            actual_sha = hashlib.sha256(data).hexdigest()
            if actual_sha != exp_sha256:
                errors.append(f"SHA-256 mismatch on {exp_name}: got {actual_sha}")
                continue
        descs.append((exp_name, offset, len(data)))
        chunks.append(data)
        offset += len(data)

    if errors:
        for err in errors:
            print(f"[embed-vfs] ERROR: {err}", file=sys.stderr)
        return 1

    assert len(descs) == 43
    raw_blob = b"".join(chunks)
    assert len(raw_blob) == offset

    crc32_val = zlib.crc32(raw_blob) & 0xFFFFFFFF
    compressed = zlib.compress(raw_blob, level=9)
    if zlib.decompress(compressed) != raw_blob:
        print("[embed-vfs] ERROR: zlib round-trip verification failed", file=sys.stderr)
        return 1

    out_dir = os.path.dirname(os.path.abspath(args.output))
    os.makedirs(out_dir, exist_ok=True)
    tmp_path = args.output + ".tmp"

    hex_table = [f"0x{b:02x}" for b in range(256)]
    with open(tmp_path, "w", encoding="ascii", newline="\n") as out:
        out.write(
            "// Auto-generated in build tree for personal standalone use. "
            "DO NOT COMMIT OR REDISTRIBUTE.\n"
            '#include "dos/embedded_vfs.h"\n\n'
            "#include <cstddef>\n"
            "#include <cstdint>\n\n"
            "namespace san4 {\n"
            "namespace {\n\n"
            "alignas(16) constexpr std::uint8_t kCompressedBlob[] = {\n"
        )
        stride = 24
        mv = memoryview(compressed)
        for i in range(0, len(mv), stride):
            row = ",".join(hex_table[b] for b in mv[i : i + stride])
            out.write(f"  {row},\n")
        out.write("};\n\nconstexpr EmbeddedFileDesc kEmbeddedFiles[] = {\n")
        for name, f_off, f_sz in descs:
            out.write(f'  {{"{name}", {f_off}u, {f_sz}u}},\n')
        out.write(
            "};\n\n"
            "constexpr EmbeddedPackedArchive kEmbeddedArchive = {\n"
            "  kCompressedBlob,\n"
            "  sizeof(kCompressedBlob),\n"
            f"  {len(raw_blob)}u,\n"
            f"  0x{crc32_val:08x}u,\n"
            "  kEmbeddedFiles,\n"
            f"  {len(descs)}u,\n"
            "};\n\n"
            "}  // namespace\n\n"
            "const EmbeddedPackedArchive* builtin_embedded_game_archive() noexcept {\n"
            "  return &kEmbeddedArchive;\n"
            "}\n\n"
            "}  // namespace san4\n"
        )

    os.replace(tmp_path, args.output)
    print(
        f"[embed-vfs] Packed {len(descs)} files: {len(raw_blob)} B -> "
        f"{len(compressed)} B zlib (CRC32=0x{crc32_val:08x})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
