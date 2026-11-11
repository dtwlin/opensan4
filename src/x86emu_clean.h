#pragma once

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

#include <x86emu.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

// Undefine short integer macros leaked by <x86emu.h> so C++20 headers and
// identifiers remain clean.
#ifdef u8
#undef u8
#endif
#ifdef u16
#undef u16
#endif
#ifdef u32
#undef u32
#endif
#ifdef u64
#undef u64
#endif
#ifdef s8
#undef s8
#endif
#ifdef s16
#undef s16
#endif
#ifdef s32
#undef s32
#endif
#ifdef s64
#undef s64
#endif
