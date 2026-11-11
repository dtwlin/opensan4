# Clang & GCC Link-Time Optimization (LTO), LLVM IR Profile-Guided Optimization
# (PGO), and llvm-bolt relocation toolchain configuration.
#
# Expected inputs (already defined by CMakeLists.txt):
#   CMAKE_BUILD_TYPE, SAN4_ENABLE_SANITIZERS, SAN4_ENABLE_TSAN, SAN4_STATIC
#
# Outputs defined in the caller's scope:
#   SAN4_ENABLE_LTO        (CACHE BOOL)
#   SAN4_LTO_MODE          (CACHE STRING: FULL or THIN)
#   SAN4_LTO_ACTIVE        (BOOL: ON when LTO flags are applied to targets)
#   SAN4_LTO_COMPILE_FLAGS (LIST of compile options)
#   SAN4_LTO_LINK_FLAGS    (LIST of link options)
#   SAN4_PGO_MODE          (CACHE STRING: OFF, GENERATE, or USE)
#   SAN4_PGO_PROFILE_DIR   (CACHE PATH)
#   SAN4_PGO_PROFILE_FILE  (CACHE FILEPATH)
#   SAN4_BOLT_RELOCS       (CACHE BOOL)
#   SAN4_LLVM_PROFDATA     (CACHE FILEPATH)
#   SAN4_LLVM_STRIP        (CACHE FILEPATH)
#   SAN4_PGO_COMPILE_FLAGS (LIST of compile options)
#   SAN4_PGO_LINK_FLAGS    (LIST of link options)
#   SAN4_BOLT_LINK_FLAGS   (LIST of link options)

string(TOUPPER "${CMAKE_BUILD_TYPE}" _san4_build_type_upper)
set(_san4_default_lto OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang"
   AND NOT _san4_build_type_upper STREQUAL "DEBUG"
   AND NOT SAN4_ENABLE_SANITIZERS
   AND NOT SAN4_ENABLE_TSAN)
  set(_san4_default_lto ON)
endif()

option(SAN4_ENABLE_LTO "Enable Link-Time Optimization (LTO)" ${_san4_default_lto})
set(SAN4_LTO_MODE "FULL" CACHE STRING "LTO mode when SAN4_ENABLE_LTO is ON: FULL or THIN")
set_property(CACHE SAN4_LTO_MODE PROPERTY STRINGS FULL THIN)

set(SAN4_PGO_MODE "OFF" CACHE STRING "Profile-Guided Optimization mode: OFF, GENERATE, or USE")
set_property(CACHE SAN4_PGO_MODE PROPERTY STRINGS OFF GENERATE USE)
set(SAN4_PGO_PROFILE_DIR "${CMAKE_BINARY_DIR}/pgo_profiles" CACHE PATH
  "Directory for raw PGO profiles when SAN4_PGO_MODE=GENERATE")
set(SAN4_PGO_PROFILE_FILE "${CMAKE_BINARY_DIR}/san4.profdata" CACHE FILEPATH
  "Merged .profdata file when SAN4_PGO_MODE=USE")
option(SAN4_BOLT_RELOCS "Emit linker relocations (-Wl,--emit-relocs) for llvm-bolt optimization" OFF)

string(TOUPPER "${SAN4_LTO_MODE}" SAN4_LTO_MODE_UPPER)
if(NOT SAN4_LTO_MODE_UPPER STREQUAL "FULL" AND NOT SAN4_LTO_MODE_UPPER STREQUAL "THIN")
  message(FATAL_ERROR
    "Invalid SAN4_LTO_MODE='${SAN4_LTO_MODE}'. Expected FULL or THIN.")
endif()

string(TOUPPER "${SAN4_PGO_MODE}" _san4_pgo_mode_upper)
if(NOT _san4_pgo_mode_upper STREQUAL "OFF"
   AND NOT _san4_pgo_mode_upper STREQUAL "GENERATE"
   AND NOT _san4_pgo_mode_upper STREQUAL "USE")
  message(FATAL_ERROR
    "Invalid SAN4_PGO_MODE='${SAN4_PGO_MODE}'. Expected OFF, GENERATE, or USE.")
endif()

if(WIN32 AND SAN4_BOLT_RELOCS)
  message(FATAL_ERROR
    "SAN4_BOLT_RELOCS=ON (-Wl,--emit-relocs / llvm-bolt) is supported only on "
    "Linux ELF targets, not Windows PE32+.")
endif()

if(NOT _san4_pgo_mode_upper STREQUAL "OFF")
  if(WIN32)
    message(FATAL_ERROR
      "SAN4_PGO_MODE='${SAN4_PGO_MODE}' is supported only on Linux ELF targets "
      "(cross-compiling to Windows PE32+ lacks host libclang_rt.profile.lib).")
  endif()
  if(SAN4_ENABLE_SANITIZERS OR SAN4_ENABLE_TSAN)
    message(FATAL_ERROR
      "SAN4_PGO_MODE='${SAN4_PGO_MODE}' cannot be combined with "
      "SAN4_ENABLE_SANITIZERS or SAN4_ENABLE_TSAN.")
  endif()
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
      "SAN4_PGO_MODE='${SAN4_PGO_MODE}' requires Clang, but the configured "
      "compiler is ${CMAKE_CXX_COMPILER_ID} ('${CMAKE_CXX_COMPILER}'). "
      "Pass -DSAN4_USE_CLANG=ON or configure a Clang toolchain.")
  endif()
endif()

set(_san4_llvm_bindir "")
set(_san4_llvm_libdir "")
set(_san4_clang_major "")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  file(REAL_PATH "${CMAKE_CXX_COMPILER}" _san4_clang_real)
  get_filename_component(_san4_llvm_bindir "${_san4_clang_real}" DIRECTORY)
  get_filename_component(_san4_llvm_prefix "${_san4_llvm_bindir}" DIRECTORY)
  set(_san4_llvm_libdir "${_san4_llvm_prefix}/lib")
  string(REGEX MATCH "^[0-9]+" _san4_clang_major "${CMAKE_CXX_COMPILER_VERSION}")
endif()

# Auto-discover matching llvm-profdata and llvm-strip (preserving symlink names
# such as llvm-strip -> llvm-objcopy).
if(NOT SAN4_LLVM_PROFDATA OR NOT EXISTS "${SAN4_LLVM_PROFDATA}")
  set(_san4_profdata_bin "")
  if(_san4_llvm_bindir AND EXISTS "${_san4_llvm_bindir}/llvm-profdata")
    set(_san4_profdata_bin "${_san4_llvm_bindir}/llvm-profdata")
  else()
    unset(_san4_found_profdata CACHE)
    if(_san4_clang_major)
      find_program(_san4_found_profdata NAMES "llvm-profdata-${_san4_clang_major}" "llvm-profdata")
    else()
      find_program(_san4_found_profdata NAMES "llvm-profdata")
    endif()
    if(_san4_found_profdata)
      set(_san4_profdata_bin "${_san4_found_profdata}")
    endif()
    unset(_san4_found_profdata CACHE)
  endif()
  set(SAN4_LLVM_PROFDATA "${_san4_profdata_bin}" CACHE FILEPATH
    "LLVM profile data merger (llvm-profdata)" FORCE)
endif()

if(NOT SAN4_LLVM_STRIP OR NOT EXISTS "${SAN4_LLVM_STRIP}")
  set(_san4_strip_bin "")
  if(_san4_llvm_bindir AND EXISTS "${_san4_llvm_bindir}/llvm-strip")
    set(_san4_strip_bin "${_san4_llvm_bindir}/llvm-strip")
  else()
    unset(_san4_found_strip CACHE)
    if(_san4_clang_major)
      find_program(_san4_found_strip NAMES "llvm-strip-${_san4_clang_major}" "llvm-strip" "strip")
    else()
      find_program(_san4_found_strip NAMES "llvm-strip" "strip")
    endif()
    if(_san4_found_strip)
      set(_san4_strip_bin "${_san4_found_strip}")
    endif()
    unset(_san4_found_strip CACHE)
  endif()
  set(SAN4_LLVM_STRIP "${_san4_strip_bin}" CACHE FILEPATH
    "Strip utility (llvm-strip or strip)" FORCE)
endif()

if(NOT _san4_pgo_mode_upper STREQUAL "OFF"
   AND (NOT SAN4_LLVM_PROFDATA OR NOT EXISTS "${SAN4_LLVM_PROFDATA}"))
  message(FATAL_ERROR
    "SAN4_PGO_MODE=${_san4_pgo_mode_upper} requires llvm-profdata matching Clang "
    "${CMAKE_CXX_COMPILER_VERSION}, but none was found (install llvm-${_san4_clang_major}).")
endif()

set(SAN4_PGO_COMPILE_FLAGS "")
set(SAN4_PGO_LINK_FLAGS "")
if(_san4_pgo_mode_upper STREQUAL "GENERATE")
  file(MAKE_DIRECTORY "${SAN4_PGO_PROFILE_DIR}")
  set(SAN4_PGO_COMPILE_FLAGS "-fprofile-generate=${SAN4_PGO_PROFILE_DIR}")
  set(SAN4_PGO_LINK_FLAGS "-fprofile-generate=${SAN4_PGO_PROFILE_DIR}")
elseif(_san4_pgo_mode_upper STREQUAL "USE")
  if(NOT SAN4_PGO_PROFILE_FILE OR NOT EXISTS "${SAN4_PGO_PROFILE_FILE}")
    message(FATAL_ERROR
      "SAN4_PGO_MODE=USE requires an existing merged profile at "
      "SAN4_PGO_PROFILE_FILE='${SAN4_PGO_PROFILE_FILE}'.")
  endif()
  set(SAN4_PGO_COMPILE_FLAGS
    "-fprofile-use=${SAN4_PGO_PROFILE_FILE}"
    -Wno-backend-plugin
    -Wno-profile-instr-unprofiled
    -Wno-profile-instr-out-of-date
  )
  set(SAN4_PGO_LINK_FLAGS
    "-fprofile-use=${SAN4_PGO_PROFILE_FILE}"
    -Wno-backend-plugin
    -Wno-profile-instr-unprofiled
    -Wno-profile-instr-out-of-date
  )
endif()

set(SAN4_BOLT_LINK_FLAGS "")
if(SAN4_BOLT_RELOCS)
  set(SAN4_BOLT_LINK_FLAGS "-Wl,--emit-relocs")
  set(_san4_bolt_relocs_summary "ON")
else()
  set(_san4_bolt_relocs_summary "OFF")
endif()

set(SAN4_LTO_ACTIVE OFF)
set(SAN4_LTO_COMPILE_FLAGS "")
set(SAN4_LTO_LINK_FLAGS "")
set(_san4_lto_linker_summary "default")
set(_san4_lto_status_summary "OFF")

if(SAN4_ENABLE_LTO)
  if(SAN4_ENABLE_SANITIZERS OR SAN4_ENABLE_TSAN)
    message(WARNING
      "SAN4_ENABLE_LTO is ON while sanitizer instrumentation is enabled; "
      "skipping LTO so sanitizer runtime linking remains clean.")
    set(_san4_lto_status_summary "OFF (skipped for sanitizers)")
  else()
    set(SAN4_LTO_ACTIVE ON)
  endif()
endif()

if(SAN4_LTO_ACTIVE)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Prefer the exact sibling llvm-ar / llvm-ranlib from the resolved LLVM
    # toolchain directory so bitcode symbol tables always match the compiler's
    # LLVM IR version.
    set(_san4_llvm_ar "")
    if(EXISTS "${_san4_llvm_bindir}/llvm-ar")
      set(_san4_llvm_ar "${_san4_llvm_bindir}/llvm-ar")
    else()
      unset(_san4_found_ar CACHE)
      find_program(_san4_found_ar NAMES "llvm-ar-${_san4_clang_major}" "llvm-ar")
      if(_san4_found_ar)
        set(_san4_llvm_ar "${_san4_found_ar}")
      endif()
      unset(_san4_found_ar CACHE)
    endif()

    set(_san4_llvm_ranlib "")
    if(EXISTS "${_san4_llvm_bindir}/llvm-ranlib")
      set(_san4_llvm_ranlib "${_san4_llvm_bindir}/llvm-ranlib")
    else()
      unset(_san4_found_ranlib CACHE)
      find_program(_san4_found_ranlib NAMES "llvm-ranlib-${_san4_clang_major}" "llvm-ranlib")
      if(_san4_found_ranlib)
        set(_san4_llvm_ranlib "${_san4_found_ranlib}")
      endif()
      unset(_san4_found_ranlib CACHE)
    endif()

    if(_san4_llvm_ar)
      set(CMAKE_AR "${_san4_llvm_ar}" CACHE FILEPATH "LLVM bitcode archiver" FORCE)
      set(CMAKE_C_COMPILER_AR "${_san4_llvm_ar}" CACHE FILEPATH "LLVM C archiver" FORCE)
      set(CMAKE_CXX_COMPILER_AR "${_san4_llvm_ar}" CACHE FILEPATH "LLVM CXX archiver" FORCE)
    endif()
    if(_san4_llvm_ranlib)
      set(CMAKE_RANLIB "${_san4_llvm_ranlib}" CACHE FILEPATH "LLVM bitcode ranlib" FORCE)
      set(CMAKE_C_COMPILER_RANLIB "${_san4_llvm_ranlib}" CACHE FILEPATH "LLVM C ranlib" FORCE)
      set(CMAKE_CXX_COMPILER_RANLIB "${_san4_llvm_ranlib}" CACHE FILEPATH "LLVM CXX ranlib" FORCE)
    endif()

    if(SAN4_LTO_MODE_UPPER STREQUAL "THIN")
      set(_san4_clang_lto_flag "-flto=thin")
    else()
      set(_san4_clang_lto_flag "-flto")
    endif()
    list(APPEND SAN4_LTO_COMPILE_FLAGS "${_san4_clang_lto_flag}")
    list(APPEND SAN4_LTO_LINK_FLAGS "${_san4_clang_lto_flag}")

    if("${CMAKE_EXE_LINKER_FLAGS} $ENV{LDFLAGS}" MATCHES "(-fuse-ld=|--ld-path=)")
      set(_san4_lto_linker_summary "user-specified (CMAKE_EXE_LINKER_FLAGS/LDFLAGS)")
    else()
      set(_san4_lld_bin "")
      if(EXISTS "${_san4_llvm_bindir}/ld.lld")
        set(_san4_lld_bin "${_san4_llvm_bindir}/ld.lld")
      else()
        unset(_san4_found_lld CACHE)
        find_program(_san4_found_lld NAMES "ld.lld-${_san4_clang_major}" "ld.lld")
        if(_san4_found_lld)
          set(_san4_lld_bin "${_san4_found_lld}")
        endif()
        unset(_san4_found_lld CACHE)
      endif()

      if(_san4_lld_bin)
        if(WIN32)
          list(APPEND SAN4_LTO_LINK_FLAGS "-fuse-ld=lld" "--ld-path=${_san4_lld_bin}")
        else()
          list(APPEND SAN4_LTO_LINK_FLAGS "--ld-path=${_san4_lld_bin}")
        endif()
        set(_san4_lto_linker_summary "${_san4_lld_bin}")
      elseif(EXISTS "${_san4_llvm_libdir}/LLVMgold.so")
        set(_san4_lto_linker_summary "system ld + ${_san4_llvm_libdir}/LLVMgold.so")
      else()
        unset(_san4_gold_bin CACHE)
        find_program(_san4_gold_bin NAMES ld.gold)
        if(_san4_gold_bin AND EXISTS "/usr/lib/bfd-plugins/LLVMgold-${_san4_clang_major}.so")
          list(APPEND SAN4_LTO_LINK_FLAGS
            "-fuse-ld=gold"
            "-Wl,-plugin,/usr/lib/bfd-plugins/LLVMgold-${_san4_clang_major}.so")
          set(_san4_lto_linker_summary "${_san4_gold_bin} + LLVMgold-${_san4_clang_major}.so")
        else()
          set(_san4_lto_linker_summary "system ld (no matching ld.lld or LLVMgold.so detected)")
        endif()
        unset(_san4_gold_bin CACHE)
      endif()
    endif()
    set(_san4_lto_status_summary "ON (${SAN4_LTO_MODE_UPPER})")
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _san4_gcc_ipo_ok OUTPUT _san4_gcc_ipo_err LANGUAGES C CXX)
    if(NOT _san4_gcc_ipo_ok)
      message(FATAL_ERROR
        "SAN4_ENABLE_LTO=ON with GCC failed CheckIPOSupported:\n${_san4_gcc_ipo_err}\n"
        "Disable LTO with -DSAN4_ENABLE_LTO=OFF.")
    endif()
    if(CMAKE_CXX_COMPILER_AR)
      set(CMAKE_AR "${CMAKE_CXX_COMPILER_AR}" CACHE FILEPATH "GCC LTO archiver" FORCE)
    endif()
    if(CMAKE_CXX_COMPILER_RANLIB)
      set(CMAKE_RANLIB "${CMAKE_CXX_COMPILER_RANLIB}" CACHE FILEPATH "GCC LTO ranlib" FORCE)
    endif()
    list(APPEND SAN4_LTO_COMPILE_FLAGS "-flto=auto")
    list(APPEND SAN4_LTO_LINK_FLAGS "-flto=auto")
    set(_san4_lto_linker_summary "GCC LTO plugin (-flto=auto)")
    set(_san4_lto_status_summary "ON (GCC -flto=auto)")
  else()
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _san4_generic_ipo_ok OUTPUT _san4_generic_ipo_err LANGUAGES C CXX)
    if(NOT _san4_generic_ipo_ok)
      message(FATAL_ERROR
        "SAN4_ENABLE_LTO=ON is not supported by ${CMAKE_CXX_COMPILER_ID}:\n${_san4_generic_ipo_err}\n"
        "Disable LTO with -DSAN4_ENABLE_LTO=OFF.")
    endif()
    list(APPEND SAN4_LTO_COMPILE_FLAGS "-flto")
    list(APPEND SAN4_LTO_LINK_FLAGS "-flto")
    set(_san4_lto_status_summary "ON")
  endif()

  # Configure-time LTO verification probe: compiles a target("avx2") +
  # __builtin_cpu_supports("avx2") translation unit with the active LTO flags,
  # archives it into a static .a library using CMAKE_AR / CMAKE_RANLIB, and
  # links an executable (including -static-libstdc++ -static-libgcc when
  # SAN4_STATIC=ON).
  set(_san4_probe_extra_link "")
  if(SAN4_STATIC)
    if(WIN32)
      set(_san4_probe_extra_link -pthread -static -static-libstdc++ -static-libgcc)
    else()
      set(_san4_probe_extra_link -static-libstdc++ -static-libgcc -Wl,--as-needed)
    endif()
  endif()
  set(_san4_probe_key
    "${CMAKE_SYSTEM_NAME}|${CMAKE_SYSROOT}|${CMAKE_CXX_COMPILER_TARGET}|${CMAKE_CXX_COMPILER}|${CMAKE_CXX_COMPILER_VERSION}|${CMAKE_AR}|${CMAKE_RANLIB}|${SAN4_LTO_COMPILE_FLAGS}|${SAN4_LTO_LINK_FLAGS}|${_san4_probe_extra_link}")

  if(NOT DEFINED _SAN4_LTO_PROBE_PASSED_KEY
     OR NOT "${_SAN4_LTO_PROBE_PASSED_KEY}" STREQUAL "${_san4_probe_key}")
    set(_san4_probe_root "${CMAKE_BINARY_DIR}/CMakeFiles/san4_lto_probe")
    set(_san4_probe_src "${_san4_probe_root}/src")
    set(_san4_probe_bin "${_san4_probe_root}/build")
    file(REMOVE_RECURSE "${_san4_probe_root}")
    file(MAKE_DIRECTORY "${_san4_probe_src}")

    file(WRITE "${_san4_probe_src}/probe_lib.cpp"
"#include <cstdint>
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
__attribute__((target(\"avx2\"))) int san4_lto_probe_avx2(int x) {
  const __m256i v = _mm256_set1_epi32(x);
  return _mm256_extract_epi32(v, 0) + 1;
}
#endif
int san4_lto_probe_fn(int x) {
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
  __builtin_cpu_init();
  if (__builtin_cpu_supports(\"avx2\")) {
    return san4_lto_probe_avx2(x);
  }
#endif
  return x + 1;
}
")

    file(WRITE "${_san4_probe_src}/probe_main.cpp"
"#include <string>
int san4_lto_probe_fn(int x);
int main() {
  std::string tag = \"san4\";
  return (san4_lto_probe_fn(41) == 42 && tag.size() == 4u) ? 0 : 1;
}
")

    set(_san4_probe_c_flags_str "")
    foreach(_f IN LISTS SAN4_LTO_COMPILE_FLAGS)
      string(APPEND _san4_probe_c_flags_str " \"${_f}\"")
    endforeach()
    set(_san4_probe_l_flags_str "")
    foreach(_f IN LISTS SAN4_LTO_LINK_FLAGS _san4_probe_extra_link)
      string(APPEND _san4_probe_l_flags_str " \"${_f}\"")
    endforeach()

    file(WRITE "${_san4_probe_src}/CMakeLists.txt"
"cmake_minimum_required(VERSION 3.20)
project(san4_lto_probe LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_library(probe_lib STATIC probe_lib.cpp)
target_compile_options(probe_lib PRIVATE${_san4_probe_c_flags_str})
add_executable(probe_exe probe_main.cpp)
target_compile_options(probe_exe PRIVATE${_san4_probe_c_flags_str})
target_link_libraries(probe_exe PRIVATE probe_lib)
target_link_options(probe_exe PRIVATE${_san4_probe_l_flags_str})
")

    set(_san4_probe_cmake_flags
      "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
      "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DCMAKE_AR=${CMAKE_AR}"
      "-DCMAKE_RANLIB=${CMAKE_RANLIB}"
      "-DCMAKE_C_COMPILER_AR=${CMAKE_C_COMPILER_AR}"
      "-DCMAKE_CXX_COMPILER_AR=${CMAKE_CXX_COMPILER_AR}"
      "-DCMAKE_C_COMPILER_RANLIB=${CMAKE_C_COMPILER_RANLIB}"
      "-DCMAKE_CXX_COMPILER_RANLIB=${CMAKE_CXX_COMPILER_RANLIB}"
      "-DCMAKE_BUILD_TYPE=Release"
    )
    if(CMAKE_EXE_LINKER_FLAGS)
      list(APPEND _san4_probe_cmake_flags "-DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}")
    endif()
    if(CMAKE_CROSSCOMPILING OR WIN32)
      list(APPEND _san4_probe_cmake_flags
        "-DCMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}"
        "-DCMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}"
        "-DCMAKE_C_LINK_DEPENDS_USE_LINKER=FALSE"
        "-DCMAKE_CXX_LINK_DEPENDS_USE_LINKER=FALSE"
      )
      if(CMAKE_SYSROOT)
        list(APPEND _san4_probe_cmake_flags "-DCMAKE_SYSROOT=${CMAKE_SYSROOT}")
      endif()
      if(CMAKE_C_COMPILER_TARGET)
        list(APPEND _san4_probe_cmake_flags "-DCMAKE_C_COMPILER_TARGET=${CMAKE_C_COMPILER_TARGET}")
      endif()
      if(CMAKE_CXX_COMPILER_TARGET)
        list(APPEND _san4_probe_cmake_flags "-DCMAKE_CXX_COMPILER_TARGET=${CMAKE_CXX_COMPILER_TARGET}")
      endif()
      if(CMAKE_C_FLAGS)
        list(APPEND _san4_probe_cmake_flags "-DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}")
      endif()
      if(CMAKE_CXX_FLAGS)
        list(APPEND _san4_probe_cmake_flags "-DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}")
      endif()
    endif()

    if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.25")
      try_compile(_san4_lto_probe_ok
        PROJECT san4_lto_probe
        SOURCE_DIR "${_san4_probe_src}"
        BINARY_DIR "${_san4_probe_bin}"
        CMAKE_FLAGS ${_san4_probe_cmake_flags}
        OUTPUT_VARIABLE _san4_lto_probe_log
      )
    else()
      try_compile(_san4_lto_probe_ok
        "${_san4_probe_bin}"
        "${_san4_probe_src}"
        san4_lto_probe
        CMAKE_FLAGS ${_san4_probe_cmake_flags}
        OUTPUT_VARIABLE _san4_lto_probe_log
      )
    endif()

    if(NOT _san4_lto_probe_ok)
      set(_san4_pkg_hint "matching lld or binutils LTO plugin")
      if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND _san4_clang_major)
        set(_san4_pkg_hint
          "lld-${_san4_clang_major} (preferred) or llvm-${_san4_clang_major}-dev (LLVMgold.so) plus llvm-${_san4_clang_major} (llvm-ar)")
      endif()
      message(FATAL_ERROR
        "Configure-time LTO verification probe failed for "
        "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} "
        "(mode=${SAN4_LTO_MODE_UPPER}, linker=${_san4_lto_linker_summary}, ar=${CMAKE_AR}).\n"
        "Install ${_san4_pkg_hint}, or disable LTO with -DSAN4_ENABLE_LTO=OFF.\n"
        "Probe output:\n${_san4_lto_probe_log}")
    endif()

    set(_SAN4_LTO_PROBE_PASSED_KEY "${_san4_probe_key}" CACHE INTERNAL
      "Last verified LTO toolchain configuration")
  endif()
endif()

message(STATUS
  "[san4-toolchain] compiler=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} "
  "(${CMAKE_CXX_COMPILER}) | LTO=${_san4_lto_status_summary} | "
  "linker=${_san4_lto_linker_summary} | ar=${CMAKE_AR} | "
  "PGO=${_san4_pgo_mode_upper} | BOLT_RELOCS=${_san4_bolt_relocs_summary}")
