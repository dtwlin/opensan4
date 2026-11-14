# Resolves the self-contained static dependency set produced by
# scripts/build_static_deps.sh for SAN4_STATIC=ON builds.
#
# Defines SAN4_SDL2_*, SAN4_X86EMU_* and SAN4_PNG_LIBRARIES in the same shape
# as the dynamic dependency discovery, plus SAN4_STATIC_LINK_FLAGS. Only glibc
# (libc, libm, ld-linux) remains a runtime dependency; SDL loads every platform
# backend (X11, Wayland, KMSDRM, GL/EGL, PulseAudio, PipeWire, ALSA) with
# dlopen() on demand.

set(SAN4_PGO_X86EMU_LIB "" CACHE FILEPATH
  "Optional override path to libx86emu.a (e.g. stage-local PGO-instrumented archive)")

set(_san4_static_inc "${SAN4_STATIC_PREFIX}/include")
set(_san4_static_lib "${SAN4_STATIC_PREFIX}/lib")

set(_san4_x86emu_archive "${_san4_static_lib}/libx86emu.a")
if(SAN4_PGO_X86EMU_LIB)
  if(NOT EXISTS "${SAN4_PGO_X86EMU_LIB}")
    message(FATAL_ERROR
      "SAN4_PGO_X86EMU_LIB='${SAN4_PGO_X86EMU_LIB}' was specified, but the file does not exist.")
  endif()
  set(_san4_x86emu_archive "${SAN4_PGO_X86EMU_LIB}")
endif()

set(_san4_static_missing "")
foreach(_hdr IN ITEMS SDL2/SDL.h x86emu.h png.h zlib.h)
  if(NOT EXISTS "${_san4_static_inc}/${_hdr}")
    list(APPEND _san4_static_missing "include/${_hdr}")
  endif()
endforeach()
foreach(_lib IN ITEMS SDL2 png16 z)
  if(NOT EXISTS "${_san4_static_lib}/lib${_lib}.a")
    list(APPEND _san4_static_missing "lib/lib${_lib}.a")
  endif()
endforeach()
if(NOT SAN4_PGO_X86EMU_LIB AND NOT EXISTS "${_san4_static_lib}/libx86emu.a")
  list(APPEND _san4_static_missing "lib/libx86emu.a")
endif()
if(_san4_static_missing)
  list(JOIN _san4_static_missing ", " _san4_static_missing_str)
  message(FATAL_ERROR
    "SAN4_STATIC=ON but static dependencies are missing from "
    "SAN4_STATIC_PREFIX='${SAN4_STATIC_PREFIX}': ${_san4_static_missing_str}.\n"
    "Run ./scripts/build_static_deps.sh (or point SAN4_STATIC_PREFIX at a "
    "prefix it populated).")
endif()

add_library(san4_static_zlib STATIC IMPORTED)
set_target_properties(san4_static_zlib PROPERTIES
  IMPORTED_LOCATION "${_san4_static_lib}/libz.a"
  INTERFACE_INCLUDE_DIRECTORIES "${_san4_static_inc}")

add_library(san4_static_png STATIC IMPORTED)
if(WIN32)
  set_target_properties(san4_static_png PROPERTIES
    IMPORTED_LOCATION "${_san4_static_lib}/libpng16.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_san4_static_inc}"
    INTERFACE_LINK_LIBRARIES "san4_static_zlib")
else()
  set_target_properties(san4_static_png PROPERTIES
    IMPORTED_LOCATION "${_san4_static_lib}/libpng16.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_san4_static_inc}"
    INTERFACE_LINK_LIBRARIES "san4_static_zlib;m")
endif()

add_library(san4_static_sdl2 STATIC IMPORTED)
if(WIN32)
  set_target_properties(san4_static_sdl2 PROPERTIES
    IMPORTED_LOCATION "${_san4_static_lib}/libSDL2.a"
    INTERFACE_LINK_LIBRARIES
      "kernel32;user32;gdi32;winmm;imm32;ole32;oleaut32;version;uuid;advapi32;setupapi;shell32;dinput8")
else()
  set_target_properties(san4_static_sdl2 PROPERTIES
    IMPORTED_LOCATION "${_san4_static_lib}/libSDL2.a"
    INTERFACE_LINK_LIBRARIES "${CMAKE_DL_LIBS};m;rt;Threads::Threads")
endif()

add_library(san4_static_x86emu STATIC IMPORTED)
set_target_properties(san4_static_x86emu PROPERTIES
  IMPORTED_LOCATION "${_san4_x86emu_archive}")

set(SAN4_SDL2_INCLUDE_DIRS "${_san4_static_inc}")
set(SAN4_SDL2_LIBRARIES san4_static_sdl2)
set(SAN4_X86EMU_INCLUDE_DIRS "${_san4_static_inc}")
set(SAN4_X86EMU_LIBRARIES san4_static_x86emu)
set(SAN4_PNG_LIBRARIES san4_static_png)
set(SAN4_ZLIB_LIBRARIES san4_static_zlib)

if(WIN32)
  set(SAN4_STATIC_LINK_FLAGS -pthread -static -static-libstdc++ -static-libgcc)
else()
  # --as-needed drops the glibc 2.34+ libdl/librt/libpthread stub sonames that
  # SDL's link interface would otherwise record as NEEDED.
  set(SAN4_STATIC_LINK_FLAGS -static-libstdc++ -static-libgcc -Wl,--as-needed)
endif()
