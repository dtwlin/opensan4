#pragma once

#include <cstddef>
#include <cstdint>

namespace san4::simd {

enum class SimdLevel : std::uint8_t {
  Scalar = 0,
  Avx2 = 1,
};

// Highest level supported by this build and CPU (detected once).
SimdLevel detected_level();
const char* level_name(SimdLevel level);

// Level used by the overloads without a `level` argument: detected_level(),
// capped by an active ScopedLevelOverride.
SimdLevel active_level();

// Test hook: caps active_level() at `cap` process-wide for the object's
// lifetime, so production callers (e.g. the HD compositor) can be run on the
// scalar kernels on an AVX2 host. Overrides nest; the kernels must not be
// running on other threads while one is created or destroyed.
class ScopedLevelOverride {
 public:
  explicit ScopedLevelOverride(SimdLevel cap);
  ~ScopedLevelOverride();

  ScopedLevelOverride(const ScopedLevelOverride&) = delete;
  ScopedLevelOverride& operator=(const ScopedLevelOverride&) = delete;

 private:
  int prev_;
};

// Optional per-pixel RGB color grading hook (e.g. a 3D LUT lookup). Applied to
// `rgb & 0x00FFFFFF` of every source pixel with non-zero alpha before the
// blend; must be a pure function of its input.
struct GradeFn {
  std::uint32_t (*fn)(const void* ctx, std::uint32_t rgb) = nullptr;
  const void* ctx = nullptr;

  std::uint32_t operator()(std::uint32_t rgb) const { return fn(ctx, rgb); }
};

// Exact floor(v / 255) for every v in [0, 65535].
constexpr std::uint32_t div255(std::uint32_t v) {
  return (v * 0x8081u) >> 23;
}

// Applies div255 to `n` values using the kernel lane arithmetic of `level`
// (16-bit lanes for Avx2). Exposed so the vector math can be verified
// exhaustively.
void div255_batch(SimdLevel level, const std::uint16_t* in, std::uint16_t* out,
                  std::size_t n);

// Source-over blends `n` HD pixels (ARGB, straight alpha) onto opaque `dst`
// at 1:1 scale:
//   a == 0   -> dst unchanged
//   a == 255 -> dst = 0xFF000000 | graded_rgb
//   else     -> per channel (g * a + d * (255 - a) + 127) / 255, alpha 0xFF
// `grade` may be nullptr (no grading).
void blend_row_4x(std::uint32_t* dst, const std::uint32_t* hd, int n,
                  const GradeFn* grade);
void blend_row_4x(SimdLevel level, std::uint32_t* dst, const std::uint32_t* hd,
                  int n, const GradeFn* grade);

// Same blend, but each output pixel `i` is first reduced from the `ds x ds`
// HD box at `hd_row0[by * hd_stride + i * ds + bx]` (`ds` is 2 or 4) with the
// alpha-weighted average:
//   a   = (sum_a + n/2) / n                 (n = ds * ds)
//   rgb = (sum(c * a_i) + sum_a/2) / sum_a  (0 when a == 0)
void blend_row_box(std::uint32_t* dst, const std::uint32_t* hd_row0,
                   std::ptrdiff_t hd_stride, int ds, int n,
                   const GradeFn* grade);
void blend_row_box(SimdLevel level, std::uint32_t* dst,
                   const std::uint32_t* hd_row0, std::ptrdiff_t hd_stride,
                   int ds, int n, const GradeFn* grade);

}  // namespace san4::simd
