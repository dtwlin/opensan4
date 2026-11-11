#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

using Bit8u = std::uint8_t;
using Bit8s = std::int8_t;
using Bit16u = std::uint16_t;
using Bit16s = std::int16_t;
using Bit32u = std::uint32_t;
using Bit32s = std::int32_t;
using Bit64u = std::uint64_t;
using Bit64s = std::int64_t;
using Bitu = std::uintptr_t;
using Bits = std::intptr_t;

#ifndef INLINE
#if defined(__GNUC__) || defined(__clang__)
#define INLINE inline __attribute__((always_inline))
#else
#define INLINE inline
#endif
#endif

#ifndef GCC_UNLIKELY
#if defined(__GNUC__) || defined(__clang__)
#define GCC_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define GCC_UNLIKELY(x) (x)
#endif
#endif
