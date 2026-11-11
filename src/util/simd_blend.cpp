#include "util/simd_blend.h"

#include <atomic>

#if (defined(__x86_64__) || defined(__i386__)) && \
    (defined(__GNUC__) || defined(__clang__))
#define SAN4_SIMD_HAS_AVX2_KERNELS 1
#include <immintrin.h>
#else
#define SAN4_SIMD_HAS_AVX2_KERNELS 0
#endif

namespace san4::simd {
namespace {

// ---------------------------------------------------------------------------
// Scalar reference kernels (bit-exact copies of the compositor math).
// ---------------------------------------------------------------------------

inline void blend_pixel_scalar(std::uint32_t& dst, std::uint32_t spx,
                               const GradeFn* grade) {
  const std::uint32_t sa = (spx >> 24) & 0xFFu;
  if (sa == 0u) {
    return;
  }
  std::uint32_t graded_rgb = spx & 0x00FFFFFFu;
  if (grade != nullptr) {
    graded_rgb = (*grade)(graded_rgb);
  }
  if (sa == 255u) {
    dst = 0xFF000000u | graded_rgb;
    return;
  }
  const std::uint32_t dpx = dst;
  const std::uint32_t inv_sa = 255u - sa;
  const std::uint32_t r =
      (((graded_rgb >> 16) & 0xFFu) * sa + ((dpx >> 16) & 0xFFu) * inv_sa +
       127u) /
      255u;
  const std::uint32_t g =
      (((graded_rgb >> 8) & 0xFFu) * sa + ((dpx >> 8) & 0xFFu) * inv_sa +
       127u) /
      255u;
  const std::uint32_t b =
      ((graded_rgb & 0xFFu) * sa + (dpx & 0xFFu) * inv_sa + 127u) / 255u;
  dst = 0xFF000000u | (r << 16) | (g << 8) | b;
}

inline std::uint32_t box_sample_scalar(const std::uint32_t* hd_row0,
                                       std::ptrdiff_t hd_stride, int ds,
                                       int i) {
  std::uint32_t sum_a = 0, sum_r = 0, sum_g = 0, sum_b = 0;
  std::uint32_t cnt_a = 0;
  for (int by = 0; by < ds; ++by) {
    const std::uint32_t* hrow = hd_row0 + by * hd_stride + i * ds;
    for (int bx = 0; bx < ds; ++bx) {
      const std::uint32_t p = hrow[bx];
      const std::uint32_t pa = (p >> 24) & 0xFFu;
      sum_a += pa;
      if (pa > 0u) {
        sum_r += ((p >> 16) & 0xFFu) * pa;
        sum_g += ((p >> 8) & 0xFFu) * pa;
        sum_b += (p & 0xFFu) * pa;
        cnt_a += pa;
      }
    }
  }
  const std::uint32_t nsub = static_cast<std::uint32_t>(ds * ds);
  const std::uint32_t avg_a = (sum_a + nsub / 2u) / nsub;
  if (avg_a == 0u || cnt_a == 0u) {
    return 0u;
  }
  const std::uint32_t r = (sum_r + cnt_a / 2u) / cnt_a;
  const std::uint32_t g = (sum_g + cnt_a / 2u) / cnt_a;
  const std::uint32_t b = (sum_b + cnt_a / 2u) / cnt_a;
  return (avg_a << 24) | (r << 16) | (g << 8) | b;
}

void blend_row_4x_scalar(std::uint32_t* dst, const std::uint32_t* hd, int n,
                         const GradeFn* grade) {
  for (int i = 0; i < n; ++i) {
    blend_pixel_scalar(dst[i], hd[i], grade);
  }
}

void blend_row_box_scalar(std::uint32_t* dst, const std::uint32_t* hd_row0,
                          std::ptrdiff_t hd_stride, int ds, int n,
                          const GradeFn* grade) {
  for (int i = 0; i < n; ++i) {
    blend_pixel_scalar(dst[i], box_sample_scalar(hd_row0, hd_stride, ds, i),
                       grade);
  }
}

void div255_batch_scalar(const std::uint16_t* in, std::uint16_t* out,
                         std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = static_cast<std::uint16_t>(div255(in[i]));
  }
}

#if SAN4_SIMD_HAS_AVX2_KERNELS

// ---------------------------------------------------------------------------
// AVX2 kernels: 8 pixels per iteration, scalar tails.
// ---------------------------------------------------------------------------

// floor(v / 255) on 16 unsigned 16-bit lanes, exact for all of [0, 65535]:
// (v * 0x8081) >> 23 == mulhi_epu16(v, 0x8081) >> 7.
__attribute__((target("avx2"))) inline __m256i div255_epu16(__m256i v) {
  return _mm256_srli_epi16(
      _mm256_mulhi_epu16(v, _mm256_set1_epi16(static_cast<short>(0x8081))),
      7);
}

// Blends 4 pixels' worth of 16-bit channels: (s * a + d * (255 - a) + 127) /
// 255 per lane, where `a16` holds each pixel's alpha replicated over its
// four channel lanes. Every intermediate fits in 16 bits (max 65152).
__attribute__((target("avx2"))) inline __m256i blend_epu16(__m256i s16,
                                                         __m256i d16,
                                                         __m256i a16) {
  const __m256i inv = _mm256_sub_epi16(_mm256_set1_epi16(255), a16);
  __m256i v = _mm256_add_epi16(_mm256_mullo_epi16(s16, a16),
                               _mm256_mullo_epi16(d16, inv));
  v = _mm256_add_epi16(v, _mm256_set1_epi16(127));
  return div255_epu16(v);
}

// Blends 8 source pixels `spx` (alpha in bits 24..31, graded RGB below) onto
// `dst`, leaving lanes with zero alpha untouched.
__attribute__((target("avx2"))) inline void blend8_store(std::uint32_t* dst,
                                                        __m256i spx,
                                                        __m256i zero_alpha) {
  const __m256i zero = _mm256_setzero_si256();
  const __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst));
  const __m256i s_lo = _mm256_unpacklo_epi8(spx, zero);
  const __m256i s_hi = _mm256_unpackhi_epi8(spx, zero);
  const __m256i d_lo = _mm256_unpacklo_epi8(d, zero);
  const __m256i d_hi = _mm256_unpackhi_epi8(d, zero);
  const __m256i a_lo =
      _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(s_lo, 0xFF), 0xFF);
  const __m256i a_hi =
      _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(s_hi, 0xFF), 0xFF);
  const __m256i r_lo = blend_epu16(s_lo, d_lo, a_lo);
  const __m256i r_hi = blend_epu16(s_hi, d_hi, a_hi);
  const __m256i blended = _mm256_or_si256(
      _mm256_packus_epi16(r_lo, r_hi),
      _mm256_set1_epi32(static_cast<int>(0xFF000000u)));
  _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst),
                      _mm256_blendv_epi8(blended, d, zero_alpha));
}

// Replaces the RGB of every non-zero-alpha lane with grade(rgb), keeping the
// alpha byte. The LUT lookup itself stays scalar.
__attribute__((target("avx2"))) inline __m256i grade8(__m256i spx,
                                                     const GradeFn& grade) {
  alignas(32) std::uint32_t px[8];
  _mm256_store_si256(reinterpret_cast<__m256i*>(px), spx);
  for (std::uint32_t& p : px) {
    if ((p >> 24) != 0u) {
      p = (p & 0xFF000000u) | grade(p & 0x00FFFFFFu);
    }
  }
  return _mm256_load_si256(reinterpret_cast<const __m256i*>(px));
}

__attribute__((target("avx2"))) inline void blend8(std::uint32_t* dst,
                                                  __m256i spx,
                                                  const GradeFn* grade) {
  const __m256i alpha = _mm256_srli_epi32(spx, 24);
  const __m256i zero_alpha = _mm256_cmpeq_epi32(alpha, _mm256_setzero_si256());
  if (_mm256_movemask_epi8(zero_alpha) == -1) {
    return;
  }
  if (grade != nullptr) {
    spx = grade8(spx, *grade);
  } else {
    const __m256i opaque = _mm256_cmpeq_epi32(alpha, _mm256_set1_epi32(255));
    if (_mm256_movemask_epi8(opaque) == -1) {
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst), spx);
      return;
    }
  }
  blend8_store(dst, spx, zero_alpha);
}

__attribute__((target("avx2"))) void blend_row_4x_avx2(std::uint32_t* dst,
                                                      const std::uint32_t* hd,
                                                      int n,
                                                      const GradeFn* grade) {
  int i = 0;
  for (; i + 8 <= n; i += 8) {
    blend8(dst + i,
           _mm256_loadu_si256(reinterpret_cast<const __m256i*>(hd + i)),
           grade);
  }
  blend_row_4x_scalar(dst + i, hd + i, n - i, grade);
}

struct BoxSums {
  __m256i a;
  __m256i r;
  __m256i g;
  __m256i b;
};

// Splits 8 ARGB pixels into alpha and alpha-weighted channels (32-bit lanes).
__attribute__((target("avx2"))) inline BoxSums weigh8(__m256i p) {
  const __m256i mask = _mm256_set1_epi32(0xFF);
  const __m256i a = _mm256_srli_epi32(p, 24);
  return BoxSums{
      a,
      _mm256_mullo_epi32(_mm256_and_si256(_mm256_srli_epi32(p, 16), mask), a),
      _mm256_mullo_epi32(_mm256_and_si256(_mm256_srli_epi32(p, 8), mask), a),
      _mm256_mullo_epi32(_mm256_and_si256(p, mask), a),
  };
}

__attribute__((target("avx2"))) inline BoxSums hadd_sums(const BoxSums& x,
                                                        const BoxSums& y) {
  return BoxSums{_mm256_hadd_epi32(x.a, y.a), _mm256_hadd_epi32(x.r, y.r),
                 _mm256_hadd_epi32(x.g, y.g), _mm256_hadd_epi32(x.b, y.b)};
}

__attribute__((target("avx2"))) inline BoxSums permute_sums(const BoxSums& x,
                                                           __m256i idx) {
  return BoxSums{_mm256_permutevar8x32_epi32(x.a, idx),
                 _mm256_permutevar8x32_epi32(x.r, idx),
                 _mm256_permutevar8x32_epi32(x.g, idx),
                 _mm256_permutevar8x32_epi32(x.b, idx)};
}

__attribute__((target("avx2"))) inline void accumulate(BoxSums& acc,
                                                      const BoxSums& x) {
  acc.a = _mm256_add_epi32(acc.a, x.a);
  acc.r = _mm256_add_epi32(acc.r, x.r);
  acc.g = _mm256_add_epi32(acc.g, x.g);
  acc.b = _mm256_add_epi32(acc.b, x.b);
}

// Loads and weighs the k-th group of 8 pixels starting at `p`.
__attribute__((target("avx2"))) inline BoxSums load_weighed(
    const std::uint32_t* p, int k) {
  return weigh8(
      _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + 8 * k)));
}

// Horizontal box sums of one HD row for 8 consecutive output pixels.
__attribute__((target("avx2"))) inline BoxSums row_box_sums(
    const std::uint32_t* p, int ds) {
  if (ds == 2) {
    // hadd(v0, v1) lanes: [v0 01, v0 23, v1 01, v1 23 | v0 45, v0 67, v1 45,
    // v1 67] -> qword order (0, 2, 1, 3) restores output pixel order.
    return permute_sums(hadd_sums(load_weighed(p, 0), load_weighed(p, 1)),
                        _mm256_setr_epi32(0, 1, 4, 5, 2, 3, 6, 7));
  }
  // ds == 4: two hadd levels give [v0 0-3, v1 0-3, v2 0-3, v3 0-3 | v0 4-7,
  // v1 4-7, v2 4-7, v3 4-7].
  const BoxSums q =
      hadd_sums(hadd_sums(load_weighed(p, 0), load_weighed(p, 1)),
                hadd_sums(load_weighed(p, 2), load_weighed(p, 3)));
  return permute_sums(q, _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7));
}

// (sum + cnt / 2) / cnt for cnt in [1, 4080] and sum <= 255 * cnt. Both
// operands are exact in float and the true quotient is at least 1 / cnt away
// from the next integer, far more than one float ulp at <= 256, so
// truncating the correctly rounded float quotient is exact.
__attribute__((target("avx2"))) inline __m256i div_round_epu32(__m256i sum,
                                                              __m256i cnt,
                                                              __m256 cnt_f) {
  const __m256i num = _mm256_add_epi32(sum, _mm256_srli_epi32(cnt, 1));
  return _mm256_cvttps_epi32(_mm256_div_ps(_mm256_cvtepi32_ps(num), cnt_f));
}

__attribute__((target("avx2"))) void blend_row_box_avx2(
    std::uint32_t* dst, const std::uint32_t* hd_row0, std::ptrdiff_t hd_stride,
    int ds, int n, const GradeFn* grade) {
  const __m256i zero = _mm256_setzero_si256();
  const int shift = (ds == 2) ? 2 : 4;  // log2(ds * ds)
  const __m256i half_n = _mm256_set1_epi32((ds * ds) / 2);
  int i = 0;
  for (; i + 8 <= n; i += 8) {
    BoxSums acc{zero, zero, zero, zero};
    for (int by = 0; by < ds; ++by) {
      accumulate(acc, row_box_sums(hd_row0 + by * hd_stride + i * ds, ds));
    }
    const __m256i avg_a = _mm256_srli_epi32(_mm256_add_epi32(acc.a, half_n),
                                            shift);
    const __m256i empty = _mm256_cmpeq_epi32(avg_a, zero);
    if (_mm256_movemask_epi8(empty) == -1) {
      continue;
    }
    // Lanes with avg_a == 0 are zeroed below; give them a safe divisor.
    const __m256i cnt = _mm256_max_epi32(acc.a, _mm256_set1_epi32(1));
    const __m256 cnt_f = _mm256_cvtepi32_ps(cnt);
    const __m256i r = div_round_epu32(acc.r, cnt, cnt_f);
    const __m256i g = div_round_epu32(acc.g, cnt, cnt_f);
    const __m256i b = div_round_epu32(acc.b, cnt, cnt_f);
    __m256i spx = _mm256_or_si256(
        _mm256_or_si256(_mm256_slli_epi32(avg_a, 24), _mm256_slli_epi32(r, 16)),
        _mm256_or_si256(_mm256_slli_epi32(g, 8), b));
    spx = _mm256_andnot_si256(empty, spx);
    blend8(dst + i, spx, grade);
  }
  for (; i < n; ++i) {
    blend_pixel_scalar(dst[i], box_sample_scalar(hd_row0, hd_stride, ds, i),
                       grade);
  }
}

__attribute__((target("avx2"))) void div255_batch_avx2(const std::uint16_t* in,
                                                      std::uint16_t* out,
                                                      std::size_t n) {
  std::size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    const __m256i v =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(in + i));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + i), div255_epu16(v));
  }
  div255_batch_scalar(in + i, out + i, n - i);
}

SimdLevel effective_level(SimdLevel requested) {
  return (requested == SimdLevel::Avx2 && detected_level() == SimdLevel::Avx2)
             ? SimdLevel::Avx2
             : SimdLevel::Scalar;
}

bool is_supported_box(int ds) { return ds == 2 || ds == 4; }

#endif  // SAN4_SIMD_HAS_AVX2_KERNELS

SimdLevel detect_level_once() {
#if SAN4_SIMD_HAS_AVX2_KERNELS
  __builtin_cpu_init();
  if (__builtin_cpu_supports("avx2")) {
    return SimdLevel::Avx2;
  }
#endif
  return SimdLevel::Scalar;
}

// Cap set by ScopedLevelOverride (-1 = none). Process-wide because the
// level-less kernels run on pool workers, not only on the overriding thread.
std::atomic<int> g_level_cap{-1};

}  // namespace

SimdLevel detected_level() {
  static const SimdLevel level = detect_level_once();
  return level;
}

SimdLevel active_level() {
  const int cap = g_level_cap.load(std::memory_order_relaxed);
  const SimdLevel detected = detected_level();
  if (cap < 0 || cap >= static_cast<int>(detected)) {
    return detected;
  }
  return static_cast<SimdLevel>(cap);
}

ScopedLevelOverride::ScopedLevelOverride(SimdLevel cap)
    : prev_(g_level_cap.exchange(static_cast<int>(cap),
                                 std::memory_order_relaxed)) {}

ScopedLevelOverride::~ScopedLevelOverride() {
  g_level_cap.store(prev_, std::memory_order_relaxed);
}

const char* level_name(SimdLevel level) {
  return (level == SimdLevel::Avx2) ? "avx2" : "scalar";
}

void div255_batch(SimdLevel level, const std::uint16_t* in, std::uint16_t* out,
                  std::size_t n) {
#if SAN4_SIMD_HAS_AVX2_KERNELS
  if (effective_level(level) == SimdLevel::Avx2) {
    div255_batch_avx2(in, out, n);
    return;
  }
#else
  (void)level;
#endif
  div255_batch_scalar(in, out, n);
}

void blend_row_4x(std::uint32_t* dst, const std::uint32_t* hd, int n,
                  const GradeFn* grade) {
  blend_row_4x(active_level(), dst, hd, n, grade);
}

void blend_row_4x(SimdLevel level, std::uint32_t* dst, const std::uint32_t* hd,
                  int n, const GradeFn* grade) {
  if (n <= 0) {
    return;
  }
#if SAN4_SIMD_HAS_AVX2_KERNELS
  if (effective_level(level) == SimdLevel::Avx2) {
    blend_row_4x_avx2(dst, hd, n, grade);
    return;
  }
#else
  (void)level;
#endif
  blend_row_4x_scalar(dst, hd, n, grade);
}

void blend_row_box(std::uint32_t* dst, const std::uint32_t* hd_row0,
                   std::ptrdiff_t hd_stride, int ds, int n,
                   const GradeFn* grade) {
  blend_row_box(active_level(), dst, hd_row0, hd_stride, ds, n, grade);
}

void blend_row_box(SimdLevel level, std::uint32_t* dst,
                   const std::uint32_t* hd_row0, std::ptrdiff_t hd_stride,
                   int ds, int n, const GradeFn* grade) {
  if (n <= 0) {
    return;
  }
  if (ds == 1) {
    blend_row_4x(level, dst, hd_row0, n, grade);
    return;
  }
#if SAN4_SIMD_HAS_AVX2_KERNELS
  if (is_supported_box(ds) && effective_level(level) == SimdLevel::Avx2) {
    blend_row_box_avx2(dst, hd_row0, hd_stride, ds, n, grade);
    return;
  }
#else
  (void)level;
#endif
  blend_row_box_scalar(dst, hd_row0, hd_stride, ds, n, grade);
}

}  // namespace san4::simd
