# Clean-room Bring-Your-Own-Game (BYOG) CMake verification gate.
# Verifies that the builder possesses the 43 original SAN4 files
# (matching exact byte lengths and SHA-256 digests, case-insensitively).
# By default (SAN4_EMBED_GAME_DIR=""), zero bytes of game code or assets are
# embedded into compiled outputs. When SAN4_EMBED_GAME_DIR is explicitly set by
# the builder for a personal standalone executable, all 43 files in
# SAN4_EMBED_GAME_DIR are verified before packing into the build directory.

set(SAN4_EMBED_ACTIVE OFF)
set(SAN4_EMBED_DEPENDS_FILES "")

set(_SAN4_MANIFEST
  "EDMUSIC.S4|8706|8d5df99f4984564fc129c09e5d32f92057cc43662c06a2bb30f888aa6a050ec2"
  "END.EXE|65787|faff2e045885b96f08bfaf6ae97f2143b5b4040cd58650c1bedb362bfc20f8b1"
  "ENDDAT.S4|1120708|6e66190af9e434b48a460d8b7ff2124f0b512209e4a1395198a42166af71b323"
  "ENDSTAFF.S4|4002|759e5d10b5c21bbe48812c22ab7dc0890b530792208b04296c95074a3c42e874"
  "FMDRV.COM|5469|803becf535bcf025447f60418573628dc4cdf69094ceaea5c16d377b27c7ddf0"
  "FONT.S4|2880|bf5ab619dae4ad8f58bd77f9031d3b4350c9b190385fcdf7c2b43f81fd281d9d"
  "GRAPH.S4|15391|be0ca58152a021e688b010f7c14b2f5b3b6a5c737ebdbc49287bf1981a479eb4"
  "GRP600C.EXE|24849|1f33eef9ddfd8d5437ceed8dd6bda1cf396c3ebdf7b95f590c9558f3acdf591d"
  "HEGRP.S4|20804|f27b9b26bc380d019a01a572946b1dbdd082bb5b2f5784dc80181b61c2b2d931"
  "HEXGRP.S4|43793|b9fd020901458d453a150be42b1f6a2c44f568efa296c437b3f7bccdaec5ba90"
  "HEXMAP.S4|64465|acae6e7f2696ffd2a137644a3f7bd5f76275827b21cd81e32db1269cb13965cb"
  "HIBACK.S4|114563|f8b0a3aeab8a2f9922fc8ecc771ef585e96a332f64418600d444a53b3b29dbeb"
  "HIGRP.S4|54092|ae9c46ce3573340a0516598c58e2ab0b28de9b4fed358250aad0cc221a6fc668"
  "HKGRP.S4|34647|99d479513bc976c99e5da6c7731bfff8ee954fcd4491ed74ff40500508aef2e6"
  "HKMAP.S4|35112|9b8b7f709c4f0fa2127c51db5cdb9ad8628cc8d290e3baf5024986fed3f3d47e"
  "HUNIT.S4|17741|17ddce26809816788f62c310748ad756127cb136fb41256de81cb7f80391abd5"
  "HWGRP.S4|15872|c1e78ca2e6d5edcd608c422ef7be1a719a725f69ff7f35d10325b39369e1e9bd"
  "KAODATA.S4|530413|fd1c968a058cd293ee62eaee3ea1a2bdf2a381438f36fb228df2e5f28d8c7356"
  "LOGO.S4|9547|f4681804b94d6670cb796f716f58422f7892d12b843ce7b4ff32fd65c38f838c"
  "MAIN.16P|56980|53fe6e53ef5c39956d3561d71ce0ce107bf17b14963bc6fab7f44af8cadaf04d"
  "MAIN.EXE|598974|78c54faa8714dfbd4910a15c07a57da09944ed1baaec261bea8478023f71234e"
  "MAINCG.S4|59905|786c8cc6b7b7cd1956af79513a12928efe63f32b3b5572d1508aa3e1aee01bf5"
  "MAINMAP.S4|21804|b60fab29583c38b2092db779c2dfa6c35b11f08fc4afa4a92e19bcb9edd4517e"
  "MESSAGE.S4|56328|c14869649065cae2a32ab2fea0d5db01240308316dd3bf5b36c68cba8ea566ca"
  "MONDATA.S4|190464|01df172a3f086e8e6ed8307190c16f1ff3add2190d7d9de5ebd927729856eff8"
  "MUSIC.S4|38824|e7afa4fc4bda06d698d87e0d17404d0115948114549c9b53c0652680e751c81f"
  "NAME.16P|280028|cdebf4d4bf6783cf59776ae9570b1c3a349ed2e4fd26a4425562c9f9594def27"
  "NBDATA.S4|17496|3bd2d1a30ea5f2e9b8ec2627213970ed9988af8df3062df439d25e2952e54369"
  "OPED.16P|21448|a77e7d308624f3bec2a8f5393b786ba218e6f761ad069ff7e1437a333c304531"
  "OPEN.EXE|53731|e779dab95a3392de27c839a345223c32a340422fe0a539e900575dc9d40b869c"
  "OPENDAT.S4|530761|587a54156a6b3443a22d56050fc226bf73aaa0c921329fba1f34aba381b58b3d"
  "OPMUSIC.S4|13635|80c5f3a08cf80efdd9f1473cd7871a2927e0e870e9a8ddd18d3a030a5764e09b"
  "PACKGRP.S4|342738|bb5d05b968853cfbe8a9bf1a980070bc327af55919898731c6fe70a00a8ff137"
  "PACKGRP2.S4|70785|5c886f1ab109ad061977a50c38d8ad34d1883d5c1ef3fc5995acfa2b27102e28"
  "PACKGRP3.S4|119142|20f7b5e3de94203c634e7a1c6fe4c26be9bc3de0fe07677225d17304efff6daa"
  "PREGRP.S4|107541|a4993d73b4197d8ab08d0e7b0346d45873e769d8683ca31f57091571ba1490c9"
  "PREMSG.S4|3669|881acf082a39a939d566e09e389a65a4958089417ced2672258ea6cc36972405"
  "REVGRP.S4|71234|415b50b64905309eebef0728041bb38f1bfb0e3d90e7ae9d8797dc60ea783a1b"
  "SAN486.COM|1934|8bb3d79630b02b8c64406a5fd85d3d4d53242a51f19e424f99b61e6a3e323d8d"
  "SNDATA.S4|59930|423e7cbcec2c2642a51aaeec3772e39d2492ff857d6f57d70386289dfab944c7"
  "TAIKI.S4|36461|91b83544940437b32a3b0a29fd87303fdbc75bd141ca03cdeec954e2b23ebd73"
  "TOSHICG.S4|31679|1add0e2347926c57693dc443bb1fad0ea10fb172e79c1bef52ca3cee7a2b113f"
  "TOSHIMAP.S4|9514|d536cfd56db861325487d35185df3046204df9a9aa1fbbe1d5f52c0cd043b13e"
)

macro(_san4_verify_dir_manifest _DIR_ABS _LABEL _OUT_MATCHED_LIST)
  file(GLOB _DIR_ENTRIES RELATIVE "${_DIR_ABS}" "${_DIR_ABS}/*")
  set(_MISSING "")
  set(${_OUT_MATCHED_LIST} "")
  foreach(_PARTS IN LISTS _SAN4_MANIFEST)
    string(REGEX REPLACE "^([^|]+)\\|([^|]+)\\|([^|]+)$" "\\1" _EXP_NAME "${_PARTS}")
    string(REGEX REPLACE "^([^|]+)\\|([^|]+)\\|([^|]+)$" "\\2" _EXP_SIZE "${_PARTS}")
    string(REGEX REPLACE "^([^|]+)\\|([^|]+)\\|([^|]+)$" "\\3" _EXP_HASH "${_PARTS}")

    set(_MATCHED_PATH "")
    foreach(_ENT IN LISTS _DIR_ENTRIES)
      string(TOUPPER "${_ENT}" _ENT_UP)
      if("${_ENT_UP}" STREQUAL "${_EXP_NAME}")
        set(_MATCHED_PATH "${_DIR_ABS}/${_ENT}")
        break()
      endif()
    endforeach()

    if(NOT _MATCHED_PATH OR NOT EXISTS "${_MATCHED_PATH}")
      list(APPEND _MISSING "  - Missing: ${_EXP_NAME}")
    else()
      file(SIZE "${_MATCHED_PATH}" _ACT_SIZE)
      file(SHA256 "${_MATCHED_PATH}" _ACT_HASH)
      # NBDATA.S4 (17,496 B) holds user-registered custom officers (New Busho Data)
      # and is overwritten in-place by MAIN.EXE at fixed byte size.
      if(NOT "${_ACT_SIZE}" STREQUAL "${_EXP_SIZE}")
        list(APPEND _MISSING "  - Size mismatch on ${_EXP_NAME} (got ${_ACT_SIZE} B, expected ${_EXP_SIZE} B)")
      elseif(NOT "${_EXP_NAME}" STREQUAL "NBDATA.S4" AND NOT "${_ACT_HASH}" STREQUAL "${_EXP_HASH}")
        list(APPEND _MISSING "  - SHA-256 mismatch on ${_EXP_NAME} (got ${_ACT_HASH})")
      else()
        list(APPEND ${_OUT_MATCHED_LIST} "${_MATCHED_PATH}")
      endif()
    endif()
  endforeach()

  if(_MISSING)
    string(JOIN "\n" _ERR_LINES ${_MISSING})
    message(FATAL_ERROR
      "\n[BYOG] ${_LABEL} verification failed in ${_DIR_ABS}:\n${_ERR_LINES}\n"
      "Supply an unmodified copy of the Traditional Chinese DOS release of RTK4 (SAN4).\n")
  endif()
endmacro()

if(NOT "${SAN4_EMBED_GAME_DIR}" STREQUAL "")
  if(SAN4_CI_NO_GAME)
    message(FATAL_ERROR
      "SAN4_EMBED_GAME_DIR cannot be combined with SAN4_CI_NO_GAME=ON.")
  endif()
  get_filename_component(SAN4_EMBED_GAME_DIR_ABS "${SAN4_EMBED_GAME_DIR}" ABSOLUTE)
  if(NOT IS_DIRECTORY "${SAN4_EMBED_GAME_DIR_ABS}")
    message(FATAL_ERROR
      "[BYOG] SAN4_EMBED_GAME_DIR does not exist or is not a directory: ${SAN4_EMBED_GAME_DIR_ABS}")
  endif()
  _san4_verify_dir_manifest("${SAN4_EMBED_GAME_DIR_ABS}" "Embedded game file" SAN4_EMBED_DEPENDS_FILES)
  set(SAN4_EMBED_ACTIVE ON)
  message(STATUS
    "[BYOG] STANDALONE EMBEDDED MODE (personal use only; do NOT redistribute compiled san4 binary): "
    "verified all 43 game files in ${SAN4_EMBED_GAME_DIR_ABS}.")
  if(NOT SAN4_GAME_DIR)
    return()
  endif()
endif()

if(SAN4_CI_NO_GAME)
  message(STATUS "[BYOG] SAN4_CI_NO_GAME=ON: Building clean-room engine & synthetic test suite without game files.")
  return()
endif()

if(NOT SAN4_GAME_DIR AND DEFINED ENV{SAN4_GAME_DIR} AND NOT "$ENV{SAN4_GAME_DIR}" STREQUAL "")
  set(SAN4_GAME_DIR "$ENV{SAN4_GAME_DIR}")
endif()

if(NOT SAN4_GAME_DIR)
  message(FATAL_ERROR
    "\n[BYOG] SAN4_GAME_DIR is required to build locally.\n"
    "Supply your own legally owned copy of the Traditional Chinese DOS release:\n"
    "  cmake -S . -B build -DSAN4_GAME_DIR=/path/to/SAN4\n"
    "(For headless CI builds using synthetic tests only, pass -DSAN4_CI_NO_GAME=ON.)\n")
endif()

get_filename_component(SAN4_GAME_DIR_ABS "${SAN4_GAME_DIR}" ABSOLUTE)
if(NOT IS_DIRECTORY "${SAN4_GAME_DIR_ABS}")
  message(FATAL_ERROR "[BYOG] SAN4_GAME_DIR does not exist or is not a directory: ${SAN4_GAME_DIR_ABS}")
endif()

_san4_verify_dir_manifest("${SAN4_GAME_DIR_ABS}" "Game file" _SAN4_EXTERNAL_MATCHED_FILES)

file(WRITE "${CMAKE_BINARY_DIR}/san4_game_dir.txt" "${SAN4_GAME_DIR_ABS}\n")
message(STATUS "[BYOG] Verified all 43 original game files in ${SAN4_GAME_DIR_ABS} (external only; zero bytes embedded).")
