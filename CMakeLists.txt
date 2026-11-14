cmake_minimum_required(VERSION 3.21)

option(SAN4_USE_CLANG "Select Clang (clang/clang++) when C/CXX compilers are not explicitly specified" OFF)
if(SAN4_USE_CLANG AND NOT CMAKE_TOOLCHAIN_FILE AND NOT CMAKE_C_COMPILER AND NOT CMAKE_CXX_COMPILER)
  set(_san4_found_clang_pair OFF)
  foreach(_san4_sfx IN ITEMS "" -23 -22 -21 -20 -19 -18 -17 -16 -15)
    unset(_san4_cand_cc CACHE)
    unset(_san4_cand_cxx CACHE)
    find_program(_san4_cand_cc NAMES "clang${_san4_sfx}")
    find_program(_san4_cand_cxx NAMES "clang++${_san4_sfx}")
    if(_san4_cand_cc AND _san4_cand_cxx)
      set(CMAKE_C_COMPILER "${_san4_cand_cc}" CACHE FILEPATH "C compiler" FORCE)
      set(CMAKE_CXX_COMPILER "${_san4_cand_cxx}" CACHE FILEPATH "CXX compiler" FORCE)
      set(_san4_found_clang_pair ON)
      break()
    endif()
  endforeach()
  unset(_san4_cand_cc CACHE)
  unset(_san4_cand_cxx CACHE)
  if(NOT _san4_found_clang_pair)
    message(FATAL_ERROR
      "SAN4_USE_CLANG=ON, but no matching clang/clang++ compiler pair "
      "(clang or clang-23..clang-15) was found in PATH.")
  endif()
elseif(CMAKE_CXX_COMPILER AND NOT CMAKE_C_COMPILER AND "${CMAKE_CXX_COMPILER}" MATCHES "clang\\+\\+")
  string(REGEX REPLACE "clang\\+\\+([^/\\]*)$" "clang\\1" _san4_derived_cc "${CMAKE_CXX_COMPILER}")
  if(NOT "${_san4_derived_cc}" STREQUAL "${CMAKE_CXX_COMPILER}")
    unset(_san4_cand_cc CACHE)
    find_program(_san4_cand_cc NAMES "${_san4_derived_cc}")
    if(_san4_cand_cc)
      set(CMAKE_C_COMPILER "${_san4_cand_cc}" CACHE FILEPATH "C compiler" FORCE)
    endif()
    unset(_san4_cand_cc CACHE)
  endif()
endif()

project(san4_linux VERSION 1.0.0 LANGUAGES C CXX)
if(WIN32)
  enable_language(RC)
endif()

if(SAN4_USE_CLANG AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR
    "SAN4_USE_CLANG=ON, but the configured C++ compiler is ${CMAKE_CXX_COMPILER_ID} "
    "('${CMAKE_CXX_COMPILER}'). Clear the build directory (or pass --fresh) when switching compilers.")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SAN4_CI_NO_GAME "Allow building engine & synthetic tests without local game files" OFF)
option(SAN4_ENABLE_AVX2 "Enable AVX2 instructions on x86_64" OFF)
option(SAN4_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(SAN4_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(SAN4_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(SAN4_ENABLE_COVERAGE "Enable Clang source-based code coverage (-fprofile-instr-generate -fcoverage-mapping)" OFF)
option(SAN4_STATIC "Link SDL2, libx86emu, libpng, zlib and the C++ runtime statically (portable binary)" OFF)
set(SAN4_STATIC_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/third_party/static" CACHE PATH
  "Install prefix populated by scripts/build_static_deps.sh for SAN4_STATIC builds")
set(SAN4_GAME_DIR "" CACHE PATH "Path to user-supplied original SAN4 DOS game directory")
set(SAN4_EMBED_GAME_DIR "" CACHE PATH
  "Optional path to verified 43-file SAN4 directory to embed into standalone runtime binary (personal use only)")

if(SAN4_ENABLE_SANITIZERS AND SAN4_ENABLE_TSAN)
  message(FATAL_ERROR "SAN4_ENABLE_SANITIZERS and SAN4_ENABLE_TSAN are mutually exclusive.")
endif()
if(SAN4_STATIC AND (SAN4_ENABLE_SANITIZERS OR SAN4_ENABLE_TSAN))
  message(FATAL_ERROR "SAN4_STATIC cannot be combined with sanitizer builds (the sanitizer runtimes require a dynamic C++ runtime).")
endif()
if(SAN4_ENABLE_COVERAGE)
  if(SAN4_ENABLE_SANITIZERS OR SAN4_ENABLE_TSAN OR SAN4_STATIC)
    message(FATAL_ERROR
      "SAN4_ENABLE_COVERAGE cannot be combined with SAN4_ENABLE_SANITIZERS, "
      "SAN4_ENABLE_TSAN, or SAN4_STATIC.")
  endif()
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
      "SAN4_ENABLE_COVERAGE requires Clang (-DSAN4_USE_CLANG=ON), but the "
      "configured compiler is ${CMAKE_CXX_COMPILER_ID}.")
  endif()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/clang_lto.cmake")
if(SAN4_ENABLE_COVERAGE AND NOT _san4_pgo_mode_upper STREQUAL "OFF")
  message(FATAL_ERROR "SAN4_ENABLE_COVERAGE cannot be combined with SAN4_PGO_MODE=${SAN4_PGO_MODE}.")
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_game_dir.cmake")

find_package(Threads REQUIRED)

set(SAN4_BUILD_RPATHS "")
set(SAN4_EXTRA_LINK_FLAGS "")
if(WIN32 AND (MINGW OR CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU"))
  # Set a 16 MB PE stack reserve (matching/exceeding Linux's 8 MB default
  # stack) so tests or callers constructing X86Machine (~1.55 MB) on the stack
  # never hit Windows's 1 MB default SizeOfStackReserve limit.
  list(APPEND SAN4_EXTRA_LINK_FLAGS
    -Wl,--high-entropy-va
    -Wl,--dynamicbase
    -Wl,--nxcompat
    -Wl,--no-insert-timestamp
    -Wl,--stack,16777216
  )
endif()

if(SAN4_STATIC)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/static_deps.cmake")
  list(APPEND SAN4_EXTRA_LINK_FLAGS ${SAN4_STATIC_LINK_FLAGS})
else()
  find_package(PNG REQUIRED)
  find_package(ZLIB REQUIRED)
  set(SAN4_PNG_LIBRARIES PNG::PNG)
  set(SAN4_ZLIB_LIBRARIES ZLIB::ZLIB)

  # Prefer system SDL2 first; fall back to rootless third_party/sdl2_dev
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SYS_SDL2 QUIET sdl2)
  endif()

  if(SYS_SDL2_FOUND)
    set(SAN4_SDL2_INCLUDE_DIRS ${SYS_SDL2_INCLUDE_DIRS})
    set(SAN4_SDL2_LIBRARIES ${SYS_SDL2_LIBRARIES})
    foreach(_dir IN LISTS SYS_SDL2_LIBRARY_DIRS)
      list(APPEND SAN4_EXTRA_LINK_FLAGS "-L${_dir}")
    endforeach()
  else()
    set(_TP_SDL_INC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sdl2_dev/usr/include")
    set(_TP_SDL_INC_ARCH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sdl2_dev/usr/include/${CMAKE_LIBRARY_ARCHITECTURE}")
    set(_TP_SDL_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sdl2_dev/lib")
    set(_TP_SDL_LIB "${_TP_SDL_LIB_DIR}/libSDL2.so")
    if(EXISTS "${_TP_SDL_INC}/SDL2/SDL.h" AND EXISTS "${_TP_SDL_LIB}")
      set(SAN4_SDL2_INCLUDE_DIRS "${_TP_SDL_INC}" "${_TP_SDL_INC_ARCH}")
      set(SAN4_SDL2_LIBRARIES SDL2)
      list(APPEND SAN4_EXTRA_LINK_FLAGS "-L${_TP_SDL_LIB_DIR}")
      file(RELATIVE_PATH _REL_SDL_DIR "${CMAKE_BINARY_DIR}" "${_TP_SDL_LIB_DIR}")
      list(APPEND SAN4_BUILD_RPATHS "$ORIGIN/${_REL_SDL_DIR}")
    else()
      message(FATAL_ERROR "SDL2 headers/libs not found. Install libsdl2-dev or run ./scripts/bootstrap_deps.sh")
    endif()
  endif()

  # Prefer system libx86emu first; fall back to rootless third_party/x86emu
  find_path(SYS_X86EMU_INCLUDE_DIR x86emu.h)
  find_library(SYS_X86EMU_LIBRARY x86emu)

  if(SYS_X86EMU_INCLUDE_DIR AND SYS_X86EMU_LIBRARY)
    set(SAN4_X86EMU_INCLUDE_DIRS "${SYS_X86EMU_INCLUDE_DIR}")
    set(SAN4_X86EMU_LIBRARIES "${SYS_X86EMU_LIBRARY}")
  else()
    set(_TP_X86_INC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/x86emu/usr/include")
    set(_TP_X86_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/x86emu/lib")
    file(GLOB _TP_X86_SO "${_TP_X86_LIB_DIR}/libx86emu.so*")
    if(EXISTS "${_TP_X86_INC}/x86emu.h" AND _TP_X86_SO)
      set(SAN4_X86EMU_INCLUDE_DIRS "${_TP_X86_INC}")
      if(EXISTS "${_TP_X86_LIB_DIR}/libx86emu.so")
        set(SAN4_X86EMU_LIBRARIES x86emu)
        list(APPEND SAN4_EXTRA_LINK_FLAGS "-L${_TP_X86_LIB_DIR}")
      else()
        list(GET _TP_X86_SO 0 _TP_X86_FIRST_SO)
        set(SAN4_X86EMU_LIBRARIES "${_TP_X86_FIRST_SO}")
      endif()
      file(RELATIVE_PATH _REL_X86_DIR "${CMAKE_BINARY_DIR}" "${_TP_X86_LIB_DIR}")
      list(APPEND SAN4_BUILD_RPATHS "$ORIGIN/${_REL_X86_DIR}")
    else()
      message(FATAL_ERROR "libx86emu not found. Install libx86emu-dev or run ./scripts/bootstrap_deps.sh")
    endif()
  endif()
endif()

set(SAN4_PREFIX_MAP_FLAGS "")
if(NOT SAN4_ENABLE_COVERAGE)
  file(REAL_PATH "${CMAKE_CURRENT_SOURCE_DIR}" _SAN4_REAL_SOURCE_DIR)
  file(REAL_PATH "${CMAKE_BINARY_DIR}" _SAN4_REAL_BINARY_DIR)
  set(SAN4_PREFIX_MAP_FLAGS
    "-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=."
    "-ffile-prefix-map=${_SAN4_REAL_SOURCE_DIR}=."
    "-ffile-prefix-map=${CMAKE_BINARY_DIR}=."
    "-ffile-prefix-map=${_SAN4_REAL_BINARY_DIR}=."
  )
  if(SAN4_STATIC AND SAN4_STATIC_PREFIX)
    list(APPEND SAN4_PREFIX_MAP_FLAGS "-ffile-prefix-map=${SAN4_STATIC_PREFIX}=.")
    if(EXISTS "${SAN4_STATIC_PREFIX}")
      file(REAL_PATH "${SAN4_STATIC_PREFIX}" _SAN4_REAL_STATIC_PREFIX)
      list(APPEND SAN4_PREFIX_MAP_FLAGS "-ffile-prefix-map=${_SAN4_REAL_STATIC_PREFIX}=.")
    endif()
  endif()
  if(CMAKE_SYSROOT)
    list(APPEND SAN4_PREFIX_MAP_FLAGS "-ffile-prefix-map=${CMAKE_SYSROOT}=.")
    if(EXISTS "${CMAKE_SYSROOT}")
      file(REAL_PATH "${CMAKE_SYSROOT}" _SAN4_REAL_SYSROOT)
      list(APPEND SAN4_PREFIX_MAP_FLAGS "-ffile-prefix-map=${_SAN4_REAL_SYSROOT}=.")
    endif()
  endif()
  list(REMOVE_DUPLICATES SAN4_PREFIX_MAP_FLAGS)
endif()

set(SAN4_WARNING_FLAGS
  -Wall
  -Wextra
  -Wpedantic
  ${SAN4_PREFIX_MAP_FLAGS}
)
if(SAN4_WARNINGS_AS_ERRORS)
  list(APPEND SAN4_WARNING_FLAGS -Werror)
endif()

set(SAN4_ARCH_FLAGS "")
if(SAN4_ENABLE_AVX2)
  message(STATUS "SAN4_ENABLE_AVX2=ON: AVX2 kernels use runtime function multiversioning in simd_blend.cpp (global -mavx2 omitted for baseline x86_64/ARM64 portability)")
endif()

set(SAN4_SANITIZER_COMPILE_FLAGS "")
set(SAN4_SANITIZER_LINK_FLAGS "")
if(SAN4_ENABLE_SANITIZERS)
  set(SAN4_SANITIZER_COMPILE_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
  set(SAN4_SANITIZER_LINK_FLAGS -fsanitize=address,undefined)
elseif(SAN4_ENABLE_TSAN)
  set(SAN4_SANITIZER_COMPILE_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
  set(SAN4_SANITIZER_LINK_FLAGS -fsanitize=thread)
endif()

set(SAN4_COVERAGE_COMPILE_FLAGS "")
set(SAN4_COVERAGE_LINK_FLAGS "")
if(SAN4_ENABLE_COVERAGE)
  set(SAN4_COVERAGE_COMPILE_FLAGS -fprofile-instr-generate -fcoverage-mapping)
  set(SAN4_COVERAGE_LINK_FLAGS -fprofile-instr-generate)
endif()

# Standalone GPL-2.0-or-later DBOPL synthesizer target
add_library(san4_opl STATIC
  src/opl/dbopl.cpp
)
target_include_directories(san4_opl PUBLIC src/opl)
target_compile_options(san4_opl PRIVATE
  -w
  ${SAN4_PREFIX_MAP_FLAGS}
  ${SAN4_LTO_COMPILE_FLAGS}
  ${SAN4_PGO_COMPILE_FLAGS}
  ${SAN4_SANITIZER_COMPILE_FLAGS}
  ${SAN4_COVERAGE_COMPILE_FLAGS}
)
target_link_options(san4_opl PUBLIC
  ${SAN4_PGO_LINK_FLAGS}
  ${SAN4_SANITIZER_LINK_FLAGS}
  ${SAN4_COVERAGE_LINK_FLAGS}
)

# Clean-room C++20 runtime library
set(SAN4_CORE_SOURCES
  src/game_manifest.cpp
  src/runtime_cli.cpp
  src/video.cpp
  src/upscaler.cpp
  src/hd_gen/san4_codecs.cpp
  src/hd_gen/hd_art_upscaler.cpp
  src/hd_gen/hd_pack_generator.cpp
  src/hd_pack.cpp
  src/audio.cpp
  src/dos/embedded_vfs.cpp
  src/dos/vfs.cpp
  src/dos/ems.cpp
  src/dos/mouse.cpp
  src/dos/dos_kernel.cpp
  src/machine.cpp
  src/render_policy.cpp
  src/frame_mailbox.cpp
  src/emu_session.cpp
  src/sdl_host.cpp
  src/util/simd_blend.cpp
  src/util/thread_pool.cpp
  src/util/win32_platform.cpp
)

add_library(san4_core STATIC ${SAN4_CORE_SOURCES})
target_compile_definitions(san4_core PUBLIC
  $<$<BOOL:${WIN32}>:SDL_MAIN_HANDLED WIN32_LEAN_AND_MEAN NOMINMAX>
)
target_include_directories(san4_core PUBLIC
  src
)
target_include_directories(san4_core SYSTEM PUBLIC
  ${SAN4_SDL2_INCLUDE_DIRS}
  ${SAN4_X86EMU_INCLUDE_DIRS}
)
target_link_options(san4_core PUBLIC
  ${SAN4_EXTRA_LINK_FLAGS}
  ${SAN4_LTO_LINK_FLAGS}
  ${SAN4_PGO_LINK_FLAGS}
  ${SAN4_SANITIZER_LINK_FLAGS}
  ${SAN4_COVERAGE_LINK_FLAGS}
)
target_link_libraries(san4_core PUBLIC
  san4_opl
  ${SAN4_SDL2_LIBRARIES}
  ${SAN4_X86EMU_LIBRARIES}
  ${SAN4_PNG_LIBRARIES}
  ${SAN4_ZLIB_LIBRARIES}
  Threads::Threads
)
target_compile_options(san4_core PRIVATE
  ${SAN4_WARNING_FLAGS}
  ${SAN4_ARCH_FLAGS}
  ${SAN4_LTO_COMPILE_FLAGS}
  ${SAN4_PGO_COMPILE_FLAGS}
  ${SAN4_SANITIZER_COMPILE_FLAGS}
  ${SAN4_COVERAGE_COMPILE_FLAGS}
)

if(SAN4_EMBED_ACTIVE)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(_SAN4_EMBED_CPP "${CMAKE_BINARY_DIR}/generated/embedded_game_data.cpp")
  add_custom_command(
    OUTPUT "${_SAN4_EMBED_CPP}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_embedded_vfs.py"
            --game-dir "${SAN4_EMBED_GAME_DIR_ABS}"
            --output "${_SAN4_EMBED_CPP}"
    DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_embedded_vfs.py"
      ${SAN4_EMBED_DEPENDS_FILES}
    COMMENT "Packing 43 verified SAN4 files into compressed in-memory VFS (personal use only)"
    VERBATIM
  )
  set(_SAN4_RUNTIME_EMBED_SRC "${_SAN4_EMBED_CPP}")
else()
  set(_SAN4_RUNTIME_EMBED_SRC "src/dos/embedded_vfs_stub.cpp")
endif()

# Primary executable (uses portable $ORIGIN-relative BUILD_RPATH only when
# bundled third_party/ libraries are active, and never bakes third_party paths
# into INSTALL_RPATH).
add_executable(san4-linux src/main.cpp "${_SAN4_RUNTIME_EMBED_SRC}")
target_link_libraries(san4-linux PRIVATE san4_core)
target_compile_options(san4-linux PRIVATE
  ${SAN4_WARNING_FLAGS}
  ${SAN4_ARCH_FLAGS}
  ${SAN4_LTO_COMPILE_FLAGS}
  ${SAN4_PGO_COMPILE_FLAGS}
  ${SAN4_SANITIZER_COMPILE_FLAGS}
  ${SAN4_COVERAGE_COMPILE_FLAGS}
)
target_link_options(san4-linux PRIVATE
  ${SAN4_PGO_LINK_FLAGS}
  ${SAN4_BOLT_LINK_FLAGS}
  ${SAN4_SANITIZER_LINK_FLAGS}
  ${SAN4_COVERAGE_LINK_FLAGS}
)
set_target_properties(san4-linux PROPERTIES
  BUILD_WITH_INSTALL_RPATH FALSE
  INSTALL_RPATH ""
  BUILD_RPATH "${SAN4_BUILD_RPATHS}"
)
if(WIN32)
  set_source_files_properties(src/win32/san4.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/win32/san4.manifest"
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/win32"
  )
  target_sources(san4-linux PRIVATE src/win32/san4.rc)
  set_target_properties(san4-linux PROPERTIES
    WIN32_EXECUTABLE TRUE
    OUTPUT_NAME "san4"
  )
endif()

# Clean-room BYOG 4x HD asset pack generator CLI
add_executable(san4-hdpack-gen
  src/hd_gen/hdpack_gen_main.cpp
  src/dos/embedded_vfs_stub.cpp
)
target_link_libraries(san4-hdpack-gen PRIVATE san4_core)
target_compile_options(san4-hdpack-gen PRIVATE
  ${SAN4_WARNING_FLAGS}
  ${SAN4_ARCH_FLAGS}
  ${SAN4_LTO_COMPILE_FLAGS}
  ${SAN4_PGO_COMPILE_FLAGS}
  ${SAN4_SANITIZER_COMPILE_FLAGS}
  ${SAN4_COVERAGE_COMPILE_FLAGS}
)
target_link_options(san4-hdpack-gen PRIVATE
  ${SAN4_PGO_LINK_FLAGS}
  ${SAN4_SANITIZER_LINK_FLAGS}
  ${SAN4_COVERAGE_LINK_FLAGS}
)
set_target_properties(san4-hdpack-gen PROPERTIES
  BUILD_WITH_INSTALL_RPATH FALSE
  INSTALL_RPATH ""
  BUILD_RPATH "${SAN4_BUILD_RPATHS}"
)
if(WIN32)
  set_source_files_properties(src/win32/san4_console.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/win32/san4.manifest"
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/win32"
  )
  target_sources(san4-hdpack-gen PRIVATE src/win32/san4_console.rc)
endif()

# Synthetic conformance, emulation, concurrency & performance test suite
enable_testing()
add_executable(san4_tests
  tests/san4_tests.cpp
  tests/reference/hd_art_upscaler_ref.cpp
  tests/reference/hd_composite_ref.cpp
  src/dos/embedded_vfs_stub.cpp
)
target_include_directories(san4_tests PRIVATE tests)
target_link_libraries(san4_tests PRIVATE san4_core)
target_compile_options(san4_tests PRIVATE
  ${SAN4_WARNING_FLAGS}
  ${SAN4_ARCH_FLAGS}
  ${SAN4_LTO_COMPILE_FLAGS}
  ${SAN4_PGO_COMPILE_FLAGS}
  ${SAN4_SANITIZER_COMPILE_FLAGS}
  ${SAN4_COVERAGE_COMPILE_FLAGS}
)
target_link_options(san4_tests PRIVATE
  ${SAN4_PGO_LINK_FLAGS}
  ${SAN4_SANITIZER_LINK_FLAGS}
  ${SAN4_COVERAGE_LINK_FLAGS}
)
set_target_properties(san4_tests PROPERTIES
  BUILD_WITH_INSTALL_RPATH FALSE
  INSTALL_RPATH ""
  BUILD_RPATH "${SAN4_BUILD_RPATHS}"
)

add_test(NAME clean_repo_hygiene
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_clean_repo.sh")
if(NOT CMAKE_CROSSCOMPILING)
  add_test(NAME cmake_lto_options_contract
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cmake_lto_options.sh")
  add_test(NAME san4_cli_entrypoints
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli_entrypoints.sh"
      $<TARGET_FILE:san4-linux>
      $<TARGET_FILE:san4-hdpack-gen>)
  if(SAN4_ENABLE_TSAN)
    set_tests_properties(san4_cli_entrypoints PROPERTIES ENVIRONMENT
      "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/tsan.supp")
  endif()
endif()
if(NOT CMAKE_CROSSCOMPILING OR CMAKE_CROSSCOMPILING_EMULATOR)
  add_test(NAME san4_unit_conformance COMMAND san4_tests)
  if(SAN4_ENABLE_TSAN)
    set_tests_properties(san4_unit_conformance PROPERTIES ENVIRONMENT
      "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/tsan.supp")
  endif()
endif()

if(SAN4_LTO_ACTIVE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT WIN32)
  set(_san4_lto_verify_cmd
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_clang_lto.sh"
    --build-dir "${CMAKE_BINARY_DIR}")
  if(SAN4_STATIC)
    list(APPEND _san4_lto_verify_cmd --static-prefix "${SAN4_STATIC_PREFIX}")
    if(SAN4_PGO_X86EMU_LIB)
      list(APPEND _san4_lto_verify_cmd --x86emu-lib "${SAN4_PGO_X86EMU_LIB}")
    endif()
  endif()
  if(_san4_pgo_mode_upper STREQUAL "USE")
    list(APPEND _san4_lto_verify_cmd --verify-pgo "${SAN4_PGO_PROFILE_FILE}")
  endif()
  list(APPEND _san4_lto_verify_cmd
    $<TARGET_FILE:san4-linux>
    $<TARGET_FILE:san4-hdpack-gen>
    $<TARGET_FILE:san4_tests>)
  add_test(NAME clang_lto_bitcode_and_elf_verify COMMAND ${_san4_lto_verify_cmd})
endif()

if(SAN4_STATIC)
  add_executable(san4_static_verify_probe tests/static_verify_probe.cpp)
  target_compile_options(san4_static_verify_probe PRIVATE ${SAN4_LTO_COMPILE_FLAGS})
  target_link_options(san4_static_verify_probe PRIVATE ${SAN4_LTO_LINK_FLAGS})
  if(WIN32)
    target_link_options(san4_static_verify_probe PRIVATE
      -Wl,--high-entropy-va
      -Wl,--dynamicbase
      -Wl,--nxcompat
      -Wl,--no-insert-timestamp
      -Wl,--stack,16777216
    )
    set(SAN4_WIN64_VERIFIER "${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_win64_binary.sh")
    set(_san4_win64_verify_cmd
      "${SAN4_WIN64_VERIFIER}"
      --expect-gui $<TARGET_FILE:san4-linux>
      --expect-manifest $<TARGET_FILE:san4-linux>
      --expect-cui $<TARGET_FILE:san4-hdpack-gen>
      --expect-manifest $<TARGET_FILE:san4-hdpack-gen>
    )
    if(SAN4_LTO_ACTIVE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      list(APPEND _san4_win64_verify_cmd
        --verify-lto
        --build-dir "${CMAKE_BINARY_DIR}"
        --static-prefix "${SAN4_STATIC_PREFIX}"
      )
    endif()
    list(APPEND _san4_win64_verify_cmd
      $<TARGET_FILE:san4-linux>
      $<TARGET_FILE:san4-hdpack-gen>
      $<TARGET_FILE:san4_tests>
    )
    add_test(NAME win64_static_pe_portability COMMAND ${_san4_win64_verify_cmd})
    add_test(NAME win64_verifier_rejects_dynamic_cxx_runtime
      COMMAND "${SAN4_WIN64_VERIFIER}" $<TARGET_FILE:san4_static_verify_probe>)
    set_tests_properties(win64_verifier_rejects_dynamic_cxx_runtime PROPERTIES WILL_FAIL TRUE)
  else()
    # Portability gates for the self-contained build: the shipped executables may
    # only need glibc core libraries, must carry no RPATH, and must start with an
    # empty environment. The probe links the C++ runtime dynamically so the
    # verifier is proven to reject foreign dependencies.
    set(SAN4_STATIC_VERIFIER "${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_static_binary.sh")
    add_test(NAME static_portable_dependencies
      COMMAND "${SAN4_STATIC_VERIFIER}"
        $<TARGET_FILE:san4-linux> $<TARGET_FILE:san4-hdpack-gen> $<TARGET_FILE:san4_tests>)
    add_test(NAME static_verifier_rejects_dynamic_cxx_runtime
      COMMAND "${SAN4_STATIC_VERIFIER}" $<TARGET_FILE:san4_static_verify_probe>)
    set_tests_properties(static_verifier_rejects_dynamic_cxx_runtime PROPERTIES WILL_FAIL TRUE)
    add_test(NAME static_runs_with_empty_environment
      COMMAND env -i $<TARGET_FILE:san4-linux> --help)
    set_tests_properties(static_runs_with_empty_environment PROPERTIES
      PASS_REGULAR_EXPRESSION "--headless-script")
  endif()
endif()
