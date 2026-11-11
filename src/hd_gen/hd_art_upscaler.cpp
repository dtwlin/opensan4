#include "hd_gen/hd_art_upscaler.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>

#include "util/function_ref.h"
#include "util/thread_pool.h"

namespace san4::hd_gen {
namespace {

struct Vec3 {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;

  Vec3 operator+(const Vec3& o) const { return {r + o.r, g + o.g, b + o.b}; }
  Vec3 operator-(const Vec3& o) const { return {r - o.r, g - o.g, b - o.b}; }
  Vec3 operator*(float s) const { return {r * s, g * s, b * s}; }
};

// Exact float sRGB transfer functions. Only evaluated to build SrgbTables.
float srgb8_to_linear_exact(std::uint8_t c8) {
  const float s = static_cast<float>(c8) * (1.0f / 255.0f);
  return (s <= 0.04045f) ? (s * (1.0f / 12.92f))
                         : std::pow((s + 0.055f) * (1.0f / 1.055f), 2.4f);
}

std::uint8_t linear_to_srgb8_exact(float lin) {
  const float c = std::clamp(lin, 0.0f, 1.0f);
  const float s = (c <= 0.0031308f)
                      ? (c * 12.92f)
                      : (1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
  return static_cast<std::uint8_t>(
      std::clamp(static_cast<int>(std::lround(s * 255.0f)), 0, 255));
}

// Table-driven sRGB conversion, bit-identical to the exact functions above:
// a 4096-entry coarse table gives a starting code that is corrected against
// the exact per-code decision thresholds (the smallest linear value that
// rounds to each code), so no per-pixel std::pow is needed.
class SrgbTables {
 public:
  static const SrgbTables& get() {
    static const SrgbTables tables;
    return tables;
  }

  float to_linear(std::uint8_t c8) const { return to_linear_[c8]; }

  std::uint8_t to_srgb8(float lin) const {
    const float c = (lin > 0.0f) ? std::min(lin, 1.0f) : 0.0f;
    int v = coarse_[static_cast<std::size_t>(c * kCoarseMax + 0.5f)];
    while (v < 255 && c >= threshold_[static_cast<std::size_t>(v) + 1u]) {
      ++v;
    }
    while (v > 0 && c < threshold_[static_cast<std::size_t>(v)]) {
      --v;
    }
    return static_cast<std::uint8_t>(v);
  }

 private:
  static constexpr std::size_t kCoarseSize = 4096u;
  static constexpr float kCoarseMax = static_cast<float>(kCoarseSize - 1u);

  SrgbTables() {
    for (std::size_t i = 0; i < to_linear_.size(); ++i) {
      to_linear_[i] = srgb8_to_linear_exact(static_cast<std::uint8_t>(i));
    }
    // Non-negative floats order like their bit patterns, so each threshold
    // is a lower-bound search over [0.0f, 1.0f].
    constexpr std::uint32_t kOneBits = 0x3F800000u;
    threshold_[0] = 0.0f;
    for (int v = 1; v < 256; ++v) {
      std::uint32_t lo = 0u;
      std::uint32_t hi = kOneBits;
      while (lo < hi) {
        const std::uint32_t mid = lo + (hi - lo) / 2u;
        if (linear_to_srgb8_exact(std::bit_cast<float>(mid)) >= v) {
          hi = mid;
        } else {
          lo = mid + 1u;
        }
      }
      threshold_[static_cast<std::size_t>(v)] = std::bit_cast<float>(lo);
    }
    for (std::size_t i = 0; i < kCoarseSize; ++i) {
      coarse_[i] = linear_to_srgb8_exact(static_cast<float>(i) / kCoarseMax);
    }
  }

  std::array<float, 256> to_linear_{};
  std::array<float, 256> threshold_{};
  std::array<std::uint8_t, kCoarseSize> coarse_{};
};

Vec3 argb_to_linear(std::uint32_t argb) {
  const SrgbTables& t = SrgbTables::get();
  return Vec3{
      t.to_linear(static_cast<std::uint8_t>((argb >> 16) & 0xFFu)),
      t.to_linear(static_cast<std::uint8_t>((argb >> 8) & 0xFFu)),
      t.to_linear(static_cast<std::uint8_t>(argb & 0xFFu)),
  };
}

float linear_luma(const Vec3& c) {
  return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

// Per-channel sqrt of a linear color: the perceptual coordinates used by
// perc_dist. Computed once per pixel instead of once per distance query.
Vec3 perceptual(const Vec3& c) {
  return {std::sqrt(std::max(0.0f, c.r)), std::sqrt(std::max(0.0f, c.g)),
          std::sqrt(std::max(0.0f, c.b))};
}

// Perceptual color distance in approximate sRGB/YCbCr space (returns >= 0)
// between two colors given in perceptual() coordinates.
float perc_dist(const Vec3& a, const Vec3& b) {
  const float dr = a.r - b.r;
  const float dg = a.g - b.g;
  const float db = a.b - b.b;
  const float dy = 0.299f * dr + 0.587f * dg + 0.114f * db;
  const float dcb = 0.5f * (db - dy);
  const float dcr = 0.5f * (dr - dy);
  return std::sqrt(4.0f * dy * dy + 1.5f * dcb * dcb + 1.8f * dcr * dcr);
}

// A linear color together with its perceptual coordinates.
struct Px {
  const Vec3* v;
  const Vec3* p;
};

float dist(const Px& a, const Px& b) { return perc_dist(*a.p, *b.p); }

Vec3 lerp3(const Vec3& a, const Vec3& b, float t) {
  return a + (b - a) * t;
}

Vec3 min3(const Vec3& a, const Vec3& b) {
  return {std::min(a.r, b.r), std::min(a.g, b.g), std::min(a.b, b.b)};
}

Vec3 max3(const Vec3& a, const Vec3& b) {
  return {std::max(a.r, b.r), std::max(a.g, b.g), std::max(a.b, b.b)};
}

Vec3 clamp3(const Vec3& v, const Vec3& lo, const Vec3& hi) {
  return {std::clamp(v.r, lo.r, hi.r), std::clamp(v.g, lo.g, hi.g),
          std::clamp(v.b, lo.b, hi.b)};
}

int wrap_or_clamp(int v, int limit, bool wrap) {
  if (wrap) {
    int m = v % limit;
    return (m < 0) ? (m + limit) : m;
  }
  return std::clamp(v, 0, limit - 1);
}

float catmull_rom_weight(float x) {
  const float ax = std::fabs(x);
  if (ax <= 1.0f) {
    return 1.5f * ax * ax * ax - 2.5f * ax * ax + 1.0f;
  }
  if (ax < 2.0f) {
    return -0.5f * ax * ax * ax + 2.5f * ax * ax - 4.0f * ax + 2.0f;
  }
  return 0.0f;
}

// Images smaller than this many pixels per pass run on the calling thread.
constexpr int kMinParallelPixels = 16384;

// Runs `fn(row_begin, row_end)` over [0, rows), split across `pool` when one
// is given and the pass is large enough. Every pass writes only its own rows.
void for_rows(ThreadPool* pool, int rows, int row_px,
              FunctionRef<void(int, int)> fn) {
  if (pool == nullptr || rows * row_px < kMinParallelPixels) {
    fn(0, rows);
    return;
  }
  const int lanes = std::max(1, static_cast<int>(pool->size()) * 4);
  pool->parallel_for(0, rows, std::max(1, (rows + lanes - 1) / lanes), fn);
}

std::vector<Vec3> to_perceptual(const std::vector<Vec3>& lin, int w, int h,
                                ThreadPool* pool) {
  std::vector<Vec3> out(lin.size());
  for_rows(pool, h, w, [&](int y0, int y1) {
    const std::size_t b = static_cast<std::size_t>(y0) * static_cast<std::size_t>(w);
    const std::size_t e = static_cast<std::size_t>(y1) * static_cast<std::size_t>(w);
    for (std::size_t i = b; i < e; ++i) {
      out[i] = perceptual(lin[i]);
    }
  });
  return out;
}

// Continuous-RGB 2x xBR step (`sw x sh` -> `(sw*2) x (sh*2)`) supporting
// optional periodic border wrap for seamless terrain tiles and 1px catchlight
// preservation on the first 1x->2x pass (so the second 2x->4x pass rounds the
// 1px dot into a crisp anti-aliased circular highlight instead of erasing it).
// `src_perc` holds perceptual() of every `src` pixel.
void xbr_2x_vec3(const std::vector<Vec3>& src, const std::vector<Vec3>& src_perc,
                 int sw, int sh, bool wrap_x, bool wrap_y,
                 bool preserve_isolated_1px, std::vector<Vec3>& dst,
                 ThreadPool* pool) {
  const int dw = sw * 2;
  const int dh = sh * 2;
  dst.resize(static_cast<std::size_t>(dw) * static_cast<std::size_t>(dh));

  auto at = [&](int x, int y) -> Px {
    const int cx = wrap_or_clamp(x, sw, wrap_x);
    const int cy = wrap_or_clamp(y, sh, wrap_y);
    const std::size_t i =
        static_cast<std::size_t>(cy) * static_cast<std::size_t>(sw) +
        static_cast<std::size_t>(cx);
    return Px{&src[i], &src_perc[i]};
  };

  constexpr float kEqThr = 0.045f;

  for_rows(pool, sh, sw, [&](int y_begin, int y_end) {
    for (int y = y_begin; y < y_end; ++y) {
      for (int x = 0; x < sw; ++x) {
        // 5x5 neighborhood around E = (x, y)
        const Px a1 = at(x - 1, y - 2);
        const Px b1 = at(x, y - 2);
        const Px c1 = at(x + 1, y - 2);

        const Px a0 = at(x - 2, y - 1);
        const Px pa = at(x - 1, y - 1);
        const Px pb = at(x, y - 1);
        const Px pc = at(x + 1, y - 1);
        const Px c4 = at(x + 2, y - 1);

        const Px d0 = at(x - 2, y);
        const Px pd = at(x - 1, y);
        const Px pe = at(x, y);
        const Px pf = at(x + 1, y);
        const Px f4 = at(x + 2, y);

        const Px g0 = at(x - 2, y + 1);
        const Px pg = at(x - 1, y + 1);
        const Px ph = at(x, y + 1);
        const Px pi = at(x + 1, y + 1);
        const Px i4 = at(x + 2, y + 1);

        const Px g5 = at(x - 1, y + 2);
        const Px h5 = at(x, y + 2);
        const Px i5 = at(x + 1, y + 2);

        Vec3 e0 = *pe.v;
        Vec3 e1 = *pe.v;
        Vec3 e2 = *pe.v;
        Vec3 e3 = *pe.v;

        const bool is_isolated_dot =
            preserve_isolated_1px && dist(pe, pa) >= kEqThr &&
            dist(pe, pb) >= kEqThr && dist(pe, pc) >= kEqThr &&
            dist(pe, pd) >= kEqThr && dist(pe, pf) >= kEqThr &&
            dist(pe, pg) >= kEqThr && dist(pe, ph) >= kEqThr &&
            dist(pe, pi) >= kEqThr;

        auto eval_corner = [&](Vec3& corner, Vec3& adj_cw, Vec3& adj_ccw,
                               const Px& H, const Px& F, const Px& I,
                               const Px& G, const Px& C, const Px& D,
                               const Px& B, const Px& F4, const Px& I4,
                               const Px& H5, const Px& I5) {
          const float d_ef = dist(pe, F);
          const float d_eh = dist(pe, H);
          if (d_ef < 0.004f && d_eh < 0.004f) {
            return;
          }
          const float wd1 = dist(pe, C) + dist(pe, G) + dist(I, H5) +
                            dist(I, F4) + 4.0f * dist(H, F);
          const float wd2 = dist(H, D) + dist(H, I5) + dist(F, I4) +
                            dist(F, B) + 4.0f * dist(pe, I);
          if (wd1 >= wd2) {
            return;
          }
          const Vec3 nc = (d_ef <= d_eh) ? *F.v : *H.v;
          const float dfg = dist(F, G);
          const float dch = dist(C, H);
          const bool fg_steep =
              (2.0f * dfg <= dch) && (dist(pe, G) >= kEqThr) &&
              (dist(D, G) >= kEqThr || dist(pe, D) < kEqThr);
          const bool ch_steep =
              (2.0f * dch <= dfg) && (dist(pe, C) >= kEqThr) &&
              (dist(B, C) >= kEqThr || dist(pe, B) < kEqThr);

          if (fg_steep && ch_steep) {
            corner = lerp3(corner, nc, 0.875f);
            adj_cw = lerp3(adj_cw, nc, 0.25f);
            adj_ccw = lerp3(adj_ccw, nc, 0.25f);
          } else if (fg_steep) {
            corner = lerp3(corner, nc, 0.75f);
            adj_ccw = lerp3(adj_ccw, nc, 0.25f);
          } else if (ch_steep) {
            corner = lerp3(corner, nc, 0.75f);
            adj_cw = lerp3(adj_cw, nc, 0.25f);
          } else {
            corner = lerp3(corner, nc, 0.50f);
          }
        };

        if (!is_isolated_dot) {
          // Bottom-right (e3), bottom-left (e2), top-right (e1), top-left (e0)
          eval_corner(e3, e1, e2, ph, pf, pi, pg, pc, pd, pb, f4, i4, h5, i5);
          eval_corner(e2, e3, e0, pd, ph, pg, pa, pi, pb, pf, h5, g5, d0, g0);
          eval_corner(e1, e0, e3, pf, pb, pc, pi, pa, ph, pd, b1, c1, f4, c4);
          eval_corner(e0, e2, e1, pb, pd, pa, pc, pg, pf, ph, d0, a0, b1, a1);
        }

        const std::size_t r0 =
            static_cast<std::size_t>(y * 2) * static_cast<std::size_t>(dw) +
            static_cast<std::size_t>(x * 2);
        const std::size_t r1 = r0 + static_cast<std::size_t>(dw);
        dst[r0 + 0] = e0;
        dst[r0 + 1] = e1;
        dst[r1 + 0] = e2;
        dst[r1 + 1] = e3;
      }
    }
  });
}

// Scalar (alpha-channel) 2x xBR step, same stencil as xbr_2x_vec3 with
// absolute-difference distances and edge clamping.
void xbr_2x_scalar(const std::vector<float>& src, int sw, int sh,
                   bool preserve_isolated_1px, std::vector<float>& dst,
                   ThreadPool* pool) {
  const int dw = sw * 2;
  const int dh = sh * 2;
  dst.resize(static_cast<std::size_t>(dw) * static_cast<std::size_t>(dh));

  auto at = [&](int x, int y) -> float {
    const int cx = std::clamp(x, 0, sw - 1);
    const int cy = std::clamp(y, 0, sh - 1);
    return src[static_cast<std::size_t>(cy) * static_cast<std::size_t>(sw) +
               static_cast<std::size_t>(cx)];
  };

  for_rows(pool, sh, sw, [&](int y_begin, int y_end) {
    for (int y = y_begin; y < y_end; ++y) {
      for (int x = 0; x < sw; ++x) {
        const float b1 = at(x, y - 2);
        const float c1 = at(x + 1, y - 2);
        const float a1 = at(x - 1, y - 2);
        const float a0 = at(x - 2, y - 1);
        const float pa = at(x - 1, y - 1);
        const float pb = at(x, y - 1);
        const float pc = at(x + 1, y - 1);
        const float c4 = at(x + 2, y - 1);
        const float d0 = at(x - 2, y);
        const float pd = at(x - 1, y);
        const float pe = at(x, y);
        const float pf = at(x + 1, y);
        const float f4 = at(x + 2, y);
        const float g0 = at(x - 2, y + 1);
        const float pg = at(x - 1, y + 1);
        const float ph = at(x, y + 1);
        const float pi = at(x + 1, y + 1);
        const float i4 = at(x + 2, y + 1);
        const float g5 = at(x - 1, y + 2);
        const float h5 = at(x, y + 2);
        const float i5 = at(x + 1, y + 2);

        float e0 = pe;
        float e1 = pe;
        float e2 = pe;
        float e3 = pe;

        auto eval_c = [&](float& corner, float& adj_cw, float& adj_ccw, float H,
                          float F, float I, float G, float C, float D, float B,
                          float F4, float I4, float H5, float I5) {
          const float def = std::fabs(pe - F);
          const float deh = std::fabs(pe - H);
          if (def < 0.01f && deh < 0.01f) return;
          const float wd1 = std::fabs(pe - C) + std::fabs(pe - G) +
                            std::fabs(I - H5) + std::fabs(I - F4) +
                            4.0f * std::fabs(H - F);
          const float wd2 = std::fabs(H - D) + std::fabs(H - I5) +
                            std::fabs(F - I4) + std::fabs(F - B) +
                            4.0f * std::fabs(pe - I);
          if (wd1 >= wd2) return;
          const float nc = (def <= deh) ? F : H;
          const float dfg = std::fabs(F - G);
          const float dch = std::fabs(C - H);
          const bool fg_steep =
              (2.0f * dfg <= dch) && (std::fabs(pe - G) > 0.1f);
          const bool ch_steep =
              (2.0f * dch <= dfg) && (std::fabs(pe - C) > 0.1f);
          if (fg_steep && ch_steep) {
            corner = corner + (nc - corner) * 0.875f;
            adj_cw = adj_cw + (nc - adj_cw) * 0.25f;
            adj_ccw = adj_ccw + (nc - adj_ccw) * 0.25f;
          } else if (fg_steep) {
            corner = corner + (nc - corner) * 0.75f;
            adj_ccw = adj_ccw + (nc - adj_ccw) * 0.25f;
          } else if (ch_steep) {
            corner = corner + (nc - corner) * 0.75f;
            adj_cw = adj_cw + (nc - adj_cw) * 0.25f;
          } else {
            corner = corner + (nc - corner) * 0.50f;
          }
        };

        const bool is_isolated_dot =
            preserve_isolated_1px && std::fabs(pe - pa) > 0.2f &&
            std::fabs(pe - pb) > 0.2f && std::fabs(pe - pc) > 0.2f &&
            std::fabs(pe - pd) > 0.2f && std::fabs(pe - pf) > 0.2f &&
            std::fabs(pe - pg) > 0.2f && std::fabs(pe - ph) > 0.2f &&
            std::fabs(pe - pi) > 0.2f;

        if (!is_isolated_dot) {
          eval_c(e3, e1, e2, ph, pf, pi, pg, pc, pd, pb, f4, i4, h5, i5);
          eval_c(e2, e3, e0, pd, ph, pg, pa, pi, pb, pf, h5, g5, d0, g0);
          eval_c(e1, e0, e3, pf, pb, pc, pi, pa, ph, pd, b1, c1, f4, c4);
          eval_c(e0, e2, e1, pb, pd, pa, pc, pg, pf, ph, d0, a0, b1, a1);
        }

        const std::size_t r0 =
            static_cast<std::size_t>(y * 2) * static_cast<std::size_t>(dw) +
            static_cast<std::size_t>(x * 2);
        dst[r0 + 0] = e0;
        dst[r0 + 1] = e1;
        dst[r0 + static_cast<std::size_t>(dw) + 0] = e2;
        dst[r0 + static_cast<std::size_t>(dw) + 1] = e3;
      }
    }
  });
}

}  // namespace

float srgb8_to_linear(std::uint8_t c8) {
  return SrgbTables::get().to_linear(c8);
}

std::uint8_t linear_to_srgb8(float linear) {
  return SrgbTables::get().to_srgb8(linear);
}

void upscale_art_4x_argb(const std::uint8_t* vga_slots,
                         const std::uint8_t* opaque_mask, int w, int h,
                         const std::array<std::uint32_t, 16>& slot_palette,
                         ArtAssetKind kind,
                         std::vector<std::uint32_t>& out_argb_4x) {
  upscale_art_4x_argb(vga_slots, opaque_mask, w, h, slot_palette, kind,
                      out_argb_4x, nullptr);
}

void upscale_art_4x_argb(const std::uint8_t* vga_slots,
                         const std::uint8_t* opaque_mask, int w, int h,
                         const std::array<std::uint32_t, 16>& slot_palette,
                         ArtAssetKind kind,
                         std::vector<std::uint32_t>& out_argb_4x,
                         ThreadPool* pool) {
  if (w <= 0 || h <= 0 || vga_slots == nullptr) {
    out_argb_4x.clear();
    return;
  }

  const SrgbTables& srgb = SrgbTables::get();
  const int out_w = w * 4;
  const int out_h = h * 4;
  const std::size_t n_src =
      static_cast<std::size_t>(w) * static_cast<std::size_t>(h);

  std::array<Vec3, 16> pal_lin{};
  std::array<Vec3, 16> pal_perc{};
  std::array<float, 16> pal_luma{};
  for (std::size_t i = 0; i < 16u; ++i) {
    pal_lin[i] = argb_to_linear(slot_palette[i]);
    pal_perc[i] = perceptual(pal_lin[i]);
    pal_luma[i] = linear_luma(pal_lin[i]);
  }
  auto pal_dist = [&](std::size_t a, std::size_t b) {
    return perc_dist(pal_perc[a], pal_perc[b]);
  };

  auto is_opaque = [&](int x, int y) -> bool {
    if (opaque_mask == nullptr) return true;
    return opaque_mask[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                       static_cast<std::size_t>(x)] != 0u;
  };

  // Detect seamless periodic edges on opaque TerrainTile assets (e.g. ocean,
  // plains, grass, city ground) so 16x16 tiles stitch with zero grid seams!
  bool wrap_x = false;
  bool wrap_y = false;
  if (kind == ArtAssetKind::TerrainTile && opaque_mask == nullptr && w >= 8 &&
      h >= 8) {
    int match_lr = 0;
    for (int y = 0; y < h; ++y) {
      const std::uint8_t s0 = vga_slots[y * w + 0] & 0x0Fu;
      const std::uint8_t s1 = vga_slots[y * w + (w - 1)] & 0x0Fu;
      const std::uint8_t s1_up =
          vga_slots[std::max(0, y - 1) * w + (w - 1)] & 0x0Fu;
      const std::uint8_t s1_dn =
          vga_slots[std::min(h - 1, y + 1) * w + (w - 1)] & 0x0Fu;
      if (pal_dist(s0, s1) < 0.22f || s0 == s1_up || s0 == s1_dn) {
        ++match_lr;
      }
    }
    int match_tb = 0;
    for (int x = 0; x < w; ++x) {
      const std::uint8_t s0 = vga_slots[0 * w + x] & 0x0Fu;
      const std::uint8_t s1 = vga_slots[(h - 1) * w + x] & 0x0Fu;
      const std::uint8_t s1_lt =
          vga_slots[(h - 1) * w + std::max(0, x - 1)] & 0x0Fu;
      const std::uint8_t s1_rt =
          vga_slots[(h - 1) * w + std::min(w - 1, x + 1)] & 0x0Fu;
      if (pal_dist(s0, s1) < 0.22f || s0 == s1_lt || s0 == s1_rt) {
        ++match_tb;
      }
    }
    wrap_x = (match_lr * 4 >= h * 3);
    wrap_y = (match_tb * 4 >= w * 3);
  }

  auto slot_at = [&](int x, int y) -> std::uint8_t {
    const int cx = wrap_or_clamp(x, w, wrap_x);
    const int cy = wrap_or_clamp(y, h, wrap_y);
    return static_cast<std::uint8_t>(
        vga_slots[static_cast<std::size_t>(cy) * static_cast<std::size_t>(w) +
                  static_cast<std::size_t>(cx)] &
        0x0Fu);
  };
  auto opq_at = [&](int x, int y) -> bool {
    if (opaque_mask == nullptr) return true;
    if (!wrap_x && (x < 0 || x >= w)) return false;
    if (!wrap_y && (y < 0 || y >= h)) return false;
    const int cx = wrap_or_clamp(x, w, wrap_x);
    const int cy = wrap_or_clamp(y, h, wrap_y);
    return is_opaque(cx, cy);
  };

  // =========================================================================
  // STAGE 1: Linear-RGB Edge-Preserving Halftone / Checkerboard De-Dithering
  // =========================================================================
  std::vector<Vec3> base_lin(n_src);
  std::vector<std::uint8_t> dither_flag(n_src, 0u);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const std::size_t idx =
          static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
          static_cast<std::size_t>(x);
      base_lin[idx] = pal_lin[slot_at(x, y)];
    }
  }

  // Maximum perceptual color distance for two palette colors to be treated as
  // an intentional halftone/dither pair (prevents averaging ink lines with
  // bright highlights or UI text strokes with button backgrounds).
  const float max_pair_dist =
      (kind == ArtAssetKind::UiChrome) ? 0.28f : 0.62f;

  std::vector<Vec3> dedith_pass1 = base_lin;
  for_rows(pool, h, w, [&](int y_begin, int y_end) {
    for (int y = y_begin; y < y_end; ++y) {
      for (int x = 0; x < w; ++x) {
        const std::size_t idx =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
            static_cast<std::size_t>(x);
        if (!is_opaque(x, y)) {
          continue;
        }

        const std::uint8_t c0 = slot_at(x, y);

        // Check 4-cardinal and 4-diagonal neighbors
        const bool ok_n = opq_at(x, y - 1);
        const bool ok_s = opq_at(x, y + 1);
        const bool ok_w = opq_at(x - 1, y);
        const bool ok_e = opq_at(x + 1, y);
        if (!ok_n || !ok_s || !ok_w || !ok_e) {
          // Silhouette boundary pixel: lock crisp contour!
          continue;
        }

        const std::uint8_t cn = slot_at(x, y - 1);
        const std::uint8_t cs = slot_at(x, y + 1);
        const std::uint8_t cw = slot_at(x - 1, y);
        const std::uint8_t ce = slot_at(x + 1, y);
        const std::uint8_t cnw =
            opq_at(x - 1, y - 1) ? slot_at(x - 1, y - 1) : c0;
        const std::uint8_t cne =
            opq_at(x + 1, y - 1) ? slot_at(x + 1, y - 1) : c0;
        const std::uint8_t csw =
            opq_at(x - 1, y + 1) ? slot_at(x - 1, y + 1) : c0;
        const std::uint8_t cse =
            opq_at(x + 1, y + 1) ? slot_at(x + 1, y + 1) : c0;

        // 1) Detect 2x2 checkerboard cell participation around (x, y)
        int partner = -1;
        auto check_2x2 = [&](int x0, int y0) -> int {
          if (!opq_at(x0, y0) || !opq_at(x0 + 1, y0) || !opq_at(x0, y0 + 1) ||
              !opq_at(x0 + 1, y0 + 1)) {
            return -1;
          }
          const std::uint8_t p00 = slot_at(x0, y0);
          const std::uint8_t p10 = slot_at(x0 + 1, y0);
          const std::uint8_t p01 = slot_at(x0, y0 + 1);
          const std::uint8_t p11 = slot_at(x0 + 1, y0 + 1);
          if (p00 == p11 && p10 == p01 && p00 != p10) {
            const std::uint8_t other = (p00 == c0) ? p10 : p00;
            if (pal_dist(c0, other) <= max_pair_dist) {
              return static_cast<int>(other);
            }
          }
          return -1;
        };

        for (int dy = -1; dy <= 0 && partner < 0; ++dy) {
          for (int dx = -1; dx <= 0 && partner < 0; ++dx) {
            partner = check_2x2(x + dx, y + dy);
          }
        }

        // 2) Detect 25% / 75% Bayer stipple (all 4 cardinals == B != c0, and
        //    at least one diagonal or step-2 cardinal == c0 so isolated 1px
        //    eye catchlights are NEVER blurred!).
        if (partner < 0 && kind != ArtAssetKind::UiChrome) {
          if (cn == cs && cw == ce && cn == cw && cn != c0 &&
              pal_dist(c0, cn) <= max_pair_dist) {
            const bool periodic_repeat =
                (cnw == c0 || cne == c0 || csw == c0 || cse == c0 ||
                 (opq_at(x - 2, y) && slot_at(x - 2, y) == c0) ||
                 (opq_at(x + 2, y) && slot_at(x + 2, y) == c0) ||
                 (opq_at(x, y - 2) && slot_at(x, y - 2) == c0) ||
                 (opq_at(x, y + 2) && slot_at(x, y + 2) == c0));
            // Extra guard for bright 1px eye catchlights in portraits:
            const bool isolated_catchlight =
                (kind == ArtAssetKind::Portrait && pal_luma[c0] > 0.65f &&
                 pal_luma[cn] < 0.20f && cnw != c0 && cne != c0 &&
                 csw != c0 && cse != c0);
            if (periodic_repeat && !isolated_catchlight) {
              partner = static_cast<int>(cn);
            }
          }
        }

        // 3) Detect 1D horizontal/vertical halftone (`cw == ce != c0` with
        //    alternating or matching `{c0, cw}` above/below, NOT a 1px
        //    vertical bevel line!).
        if (partner < 0 && kind != ArtAssetKind::UiChrome) {
          if (cw == ce && cw != c0 &&
              pal_dist(c0, cw) <= max_pair_dist * 0.85f) {
            const bool repeat_x =
                (opq_at(x - 2, y) && slot_at(x - 2, y) == c0) ||
                (opq_at(x + 2, y) && slot_at(x + 2, y) == c0);
            const bool not_solid_line = (cn == cw || cs == cw);
            if (repeat_x && not_solid_line) {
              partner = static_cast<int>(cw);
            }
          } else if (cn == cs && cn != c0 &&
                     pal_dist(c0, cn) <= max_pair_dist * 0.85f) {
            const bool repeat_y =
                (opq_at(x, y - 2) && slot_at(x, y - 2) == c0) ||
                (opq_at(x, y + 2) && slot_at(x, y + 2) == c0);
            const bool not_solid_line = (cw == cn || ce == cn);
            if (repeat_y && not_solid_line) {
              partner = static_cast<int>(cn);
            }
          }
        }

        if (partner < 0) {
          continue;
        }

        dither_flag[idx] = 1u;
        const std::size_t partner_slot = static_cast<std::size_t>(partner);
        static constexpr float kKernel[3][3] = {
            {1.0f, 2.0f, 1.0f},
            {2.0f, 4.0f, 2.0f},
            {1.0f, 2.0f, 1.0f},
        };
        Vec3 accum{};
        float wsum = 0.0f;
        for (int ky = -1; ky <= 1; ++ky) {
          for (int kx = -1; kx <= 1; ++kx) {
            if (!opq_at(x + kx, y + ky)) continue;
            const std::uint8_t ns = slot_at(x + kx, y + ky);
            // Accept pixels belonging to `{c0, partner}` or perceptually
            // close intermediate shades in a multi-band halftone ramp.
            if (ns == c0 || ns == static_cast<std::uint8_t>(partner) ||
                (pal_dist(ns, c0) <= max_pair_dist * 0.7f &&
                 pal_dist(ns, partner_slot) <= max_pair_dist * 0.7f)) {
              const float kw = kKernel[ky + 1][kx + 1];
              accum = accum + pal_lin[ns] * kw;
              wsum += kw;
            }
          }
        }
        if (wsum > 0.0f) {
          dedith_pass1[idx] = accum * (1.0f / wsum);
        }
      }
    }
  });

  // Gentle second-pass bilateral polish purely inside detected halftone zones
  // so 25% -> 50% -> 75% Bayer bands transition without step banding.
  const std::vector<Vec3> pass1_perc = to_perceptual(dedith_pass1, w, h, pool);
  std::vector<Vec3> stage1_lin = dedith_pass1;
  for_rows(pool, h, w, [&](int y_begin, int y_end) {
    for (int y = y_begin; y < y_end; ++y) {
      for (int x = 0; x < w; ++x) {
        const std::size_t idx =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
            static_cast<std::size_t>(x);
        if (dither_flag[idx] == 0u) {
          continue;
        }
        const Vec3 center = dedith_pass1[idx];
        Vec3 sum = center * 4.0f;
        float wsum = 4.0f;
        for (int ky = -1; ky <= 1; ++ky) {
          for (int kx = -1; kx <= 1; ++kx) {
            if (kx == 0 && ky == 0) continue;
            if (!opq_at(x + kx, y + ky)) continue;
            const int nx = wrap_or_clamp(x + kx, w, wrap_x);
            const int ny = wrap_or_clamp(y + ky, h, wrap_y);
            const std::size_t nidx =
                static_cast<std::size_t>(ny) * static_cast<std::size_t>(w) +
                static_cast<std::size_t>(nx);
            const Vec3& nv = dedith_pass1[nidx];
            const float d = perc_dist(pass1_perc[idx], pass1_perc[nidx]);
            if (d < 0.24f) {
              const float kw = ((kx == 0 || ky == 0) ? 1.5f : 0.75f) *
                               (1.0f - d * (1.0f / 0.24f));
              sum = sum + nv * kw;
              wsum += kw;
            }
          }
        }
        stage1_lin[idx] = sum * (1.0f / wsum);
      }
    }
  });

  // =========================================================================
  // STAGE 2: Pre-Dilated RGB + Hybrid `4x` xBR + Clamped Catmull-Rom Blending
  // =========================================================================
  if (opaque_mask != nullptr) {
    std::vector<std::uint8_t> valid(opaque_mask, opaque_mask + n_src);
    for (int iter = 0; iter < 3; ++iter) {
      std::vector<Vec3> next_rgb = stage1_lin;
      std::vector<std::uint8_t> next_valid = valid;
      for_rows(pool, h, w, [&](int y_begin, int y_end) {
        for (int y = y_begin; y < y_end; ++y) {
          for (int x = 0; x < w; ++x) {
            const std::size_t idx =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                static_cast<std::size_t>(x);
            if (valid[idx] != 0u) continue;
            Vec3 sum{};
            float cnt = 0.0f;
            for (int dy = -1; dy <= 1; ++dy) {
              for (int dx = -1; dx <= 1; ++dx) {
                const int nx = x + dx;
                const int ny = y + dy;
                if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                const std::size_t nidx =
                    static_cast<std::size_t>(ny) * static_cast<std::size_t>(w) +
                    static_cast<std::size_t>(nx);
                if (valid[nidx] != 0u) {
                  const float wt = (dx == 0 || dy == 0) ? 2.0f : 1.0f;
                  sum = sum + stage1_lin[nidx] * wt;
                  cnt += wt;
                }
              }
            }
            if (cnt > 0.0f) {
              next_rgb[idx] = sum * (1.0f / cnt);
              next_valid[idx] = 1u;
            }
          }
        }
      });
      stage1_lin = std::move(next_rgb);
      valid = std::move(next_valid);
    }
  }

  // 2a) Two-pass continuous-RGB xBR (`1x -> 2x -> 4x`)
  const std::vector<Vec3> stage1_perc = to_perceptual(stage1_lin, w, h, pool);
  std::vector<Vec3> xbr2x;
  std::vector<Vec3> xbr4x;
  xbr_2x_vec3(stage1_lin, stage1_perc, w, h, wrap_x, wrap_y, true, xbr2x, pool);
  {
    const std::vector<Vec3> xbr2x_perc =
        to_perceptual(xbr2x, w * 2, h * 2, pool);
    xbr_2x_vec3(xbr2x, xbr2x_perc, w * 2, h * 2, wrap_x, wrap_y, false, xbr4x,
                pool);
  }

  // 2b) Separable Clamped Catmull-Rom (`4x`) + local edge-strength fusion
  auto stage1_index = [&](int x, int y) -> std::size_t {
    const int cx = wrap_or_clamp(x, w, wrap_x);
    const int cy = wrap_or_clamp(y, h, wrap_y);
    return static_cast<std::size_t>(cy) * static_cast<std::size_t>(w) +
           static_cast<std::size_t>(cx);
  };

  std::vector<Vec3> fused4x(
      static_cast<std::size_t>(out_w) * static_cast<std::size_t>(out_h));
  const float bicubic_cap = (kind == ArtAssetKind::UiChrome) ? 0.20f : 0.72f;

  for_rows(pool, out_h, out_w, [&](int oy_begin, int oy_end) {
    for (int oy = oy_begin; oy < oy_end; ++oy) {
      const float fy = (static_cast<float>(oy) + 0.5f) * 0.25f - 0.5f;
      const int iy = static_cast<int>(std::floor(fy));
      const float ty = fy - static_cast<float>(iy);
      const float wy[4] = {
          catmull_rom_weight(-1.0f - ty),
          catmull_rom_weight(0.0f - ty),
          catmull_rom_weight(1.0f - ty),
          catmull_rom_weight(2.0f - ty),
      };

      for (int ox = 0; ox < out_w; ++ox) {
        const float fx = (static_cast<float>(ox) + 0.5f) * 0.25f - 0.5f;
        const int ix = static_cast<int>(std::floor(fx));
        const float tx = fx - static_cast<float>(ix);
        const float wx[4] = {
            catmull_rom_weight(-1.0f - tx),
            catmull_rom_weight(0.0f - tx),
            catmull_rom_weight(1.0f - tx),
            catmull_rom_weight(2.0f - tx),
        };

        Vec3 bic{};
        for (int j = 0; j < 4; ++j) {
          Vec3 row_acc{};
          for (int i = 0; i < 4; ++i) {
            row_acc = row_acc +
                      stage1_lin[stage1_index(ix - 1 + i, iy - 1 + j)] * wx[i];
          }
          bic = bic + row_acc * wy[j];
        }

        // Clamp strictly within local 2x2 neighborhood -> zero ringing/halos!
        const std::size_t i00 = stage1_index(ix, iy);
        const std::size_t i10 = stage1_index(ix + 1, iy);
        const std::size_t i01 = stage1_index(ix, iy + 1);
        const std::size_t i11 = stage1_index(ix + 1, iy + 1);
        const Vec3& p00 = stage1_lin[i00];
        const Vec3& p10 = stage1_lin[i10];
        const Vec3& p01 = stage1_lin[i01];
        const Vec3& p11 = stage1_lin[i11];
        const Vec3 lo = min3(min3(p00, p10), min3(p01, p11));
        const Vec3 hi = max3(max3(p00, p10), max3(p01, p11));
        bic = clamp3(bic, lo, hi);

        // Local contrast in the 2x2 cell drives blend between smooth
        // Catmull-Rom (low-gradient shading) and directional xBR (crisp
        // contours & lines).
        const Vec3& q00 = stage1_perc[i00];
        const Vec3& q10 = stage1_perc[i10];
        const Vec3& q01 = stage1_perc[i01];
        const Vec3& q11 = stage1_perc[i11];
        const float max_d =
            std::max({perc_dist(q00, q10), perc_dist(q00, q01),
                      perc_dist(q10, q11), perc_dist(q01, q11),
                      perc_dist(q00, q11), perc_dist(q10, q01)});
        const float edge_t =
            std::clamp((max_d - 0.06f) * (1.0f / 0.26f), 0.0f, 1.0f);
        const float smooth_t = edge_t * edge_t * (3.0f - 2.0f * edge_t);
        const float w_bic = bicubic_cap * (1.0f - smooth_t);

        const std::size_t oidx =
            static_cast<std::size_t>(oy) * static_cast<std::size_t>(out_w) +
            static_cast<std::size_t>(ox);
        fused4x[oidx] = lerp3(xbr4x[oidx], bic, w_bic);
      }
    }
  });

  // =========================================================================
  // STAGE 3: Halo-Free Micro-Contrast + `4x` Contour-Smoothed 8-Bit Alpha
  // =========================================================================
  std::vector<float> alpha4x;
  if (opaque_mask != nullptr) {
    std::vector<float> a1x(n_src);
    for (std::size_t i = 0; i < n_src; ++i) {
      a1x[i] = (opaque_mask[i] != 0u) ? 1.0f : 0.0f;
    }
    std::vector<float> a2x;
    xbr_2x_scalar(a1x, w, h, true, a2x, pool);
    xbr_2x_scalar(a2x, w * 2, h * 2, false, alpha4x, pool);
  }

  out_argb_4x.resize(static_cast<std::size_t>(out_w) *
                     static_cast<std::size_t>(out_h));

  auto fused_at = [&](int x, int y) -> const Vec3& {
    const int cx = wrap_or_clamp(x, out_w, wrap_x);
    const int cy = wrap_or_clamp(y, out_h, wrap_y);
    return fused4x[static_cast<std::size_t>(cy) * static_cast<std::size_t>(out_w) +
                   static_cast<std::size_t>(cx)];
  };

  for_rows(pool, out_h, out_w, [&](int oy_begin, int oy_end) {
    for (int oy = oy_begin; oy < oy_end; ++oy) {
      for (int ox = 0; ox < out_w; ++ox) {
        const std::size_t oidx =
            static_cast<std::size_t>(oy) * static_cast<std::size_t>(out_w) +
            static_cast<std::size_t>(ox);

        std::uint8_t a8 = 255u;
        if (!alpha4x.empty()) {
          const float af =
              std::clamp((alpha4x[oidx] - 0.5f) * 1.55f + 0.5f, 0.0f, 1.0f);
          a8 = static_cast<std::uint8_t>(
              std::clamp(static_cast<int>(std::lround(af * 255.0f)), 0, 255));
          if (a8 == 0u) {
            out_argb_4x[oidx] = 0x00000000u;
            continue;
          }
        }

        const Vec3& c = fused4x[oidx];
        const Vec3& n = fused_at(ox, oy - 1);
        const Vec3& s = fused_at(ox, oy + 1);
        const Vec3& wv = fused_at(ox - 1, oy);
        const Vec3& ev = fused_at(ox + 1, oy);
        const Vec3 avg = (n + s + wv + ev) * 0.25f;
        const Vec3 lo = min3(c, min3(min3(n, s), min3(wv, ev)));
        const Vec3 hi = max3(c, max3(max3(n, s), max3(wv, ev)));
        const Vec3 sharp = clamp3(c + (c - avg) * 0.16f, lo, hi);

        const std::uint32_t r8 = srgb.to_srgb8(sharp.r);
        const std::uint32_t g8 = srgb.to_srgb8(sharp.g);
        const std::uint32_t b8 = srgb.to_srgb8(sharp.b);
        out_argb_4x[oidx] = (static_cast<std::uint32_t>(a8) << 24) |
                            (r8 << 16) | (g8 << 8) | b8;
      }
    }
  });
}


void rasterize_mono_1bit_4x(const std::uint8_t* bits_1bit, int w, int h,
                            std::uint32_t fg_argb, std::uint32_t bg_argb,
                            bool transparent_zero,
                            std::vector<std::uint32_t>& out_argb_4x) {
  if (w <= 0 || h <= 0 || bits_1bit == nullptr) {
    out_argb_4x.clear();
    return;
  }
  const int out_w = w * 4;
  const int out_h = h * 4;
  out_argb_4x.resize(static_cast<std::size_t>(out_w) *
                     static_cast<std::size_t>(out_h));

  auto bit_at = [&](int x, int y) -> std::uint8_t {
    if (x < 0 || x >= w || y < 0 || y >= h) return 0u;
    return (bits_1bit[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                      static_cast<std::size_t>(x)] != 0u)
               ? 1u
               : 0u;
  };

  const std::uint32_t fg_r = (fg_argb >> 16) & 0xFFu;
  const std::uint32_t fg_g = (fg_argb >> 8) & 0xFFu;
  const std::uint32_t fg_b = fg_argb & 0xFFu;
  const std::uint32_t bg_r = (bg_argb >> 16) & 0xFFu;
  const std::uint32_t bg_g = (bg_argb >> 8) & 0xFFu;
  const std::uint32_t bg_b = bg_argb & 0xFFu;

  // Fast, razor-sharp 4x CJK stroke & diagonal brush-edge rasterizer:
  // Preserves 1px horizontal/vertical radical spacing (no blur between parallel
  // strokes!) while smoothing genuine 45-degree brush diagonals (`harai`/`nagi`)
  // across the 4x4 subpixel block with clean anti-aliased bevel transitions!
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const std::uint8_t c = bit_at(x, y);
      const std::uint8_t n = bit_at(x, y - 1);
      const std::uint8_t s = bit_at(x, y + 1);
      const std::uint8_t wv = bit_at(x - 1, y);
      const std::uint8_t ev = bit_at(x + 1, y);
      const std::uint8_t nw = bit_at(x - 1, y - 1);
      const std::uint8_t ne = bit_at(x + 1, y - 1);
      const std::uint8_t sw = bit_at(x - 1, y + 1);
      const std::uint8_t se = bit_at(x + 1, y + 1);

      // Guard against bridging parallel strokes or 2x2 checkerboards:
      // Only fill a 0-corner when two orthogonal neighbors == 1 AND their
      // shared diagonal == 0!
      const bool diag_tl = (c == 0u && n == 1u && wv == 1u && nw == 0u);
      const bool diag_tr = (c == 0u && n == 1u && ev == 1u && ne == 0u);
      const bool diag_bl = (c == 0u && s == 1u && wv == 1u && sw == 0u);
      const bool diag_br = (c == 0u && s == 1u && ev == 1u && se == 0u);

      // Subtle rounding on convex 1-corners so strokes look clean at 3200x2400:
      const bool trim_tl = (c == 1u && n == 0u && wv == 0u && nw == 0u &&
                            (s == 1u || ev == 1u));
      const bool trim_tr = (c == 1u && n == 0u && ev == 0u && ne == 0u &&
                            (s == 1u || wv == 1u));
      const bool trim_bl = (c == 1u && s == 0u && wv == 0u && sw == 0u &&
                            (n == 1u || ev == 1u));
      const bool trim_br = (c == 1u && s == 0u && ev == 0u && se == 0u &&
                            (n == 1u || wv == 1u));

      for (int sy = 0; sy < 4; ++sy) {
        std::uint32_t* drow =
            out_argb_4x.data() +
            static_cast<std::size_t>(y * 4 + sy) *
                static_cast<std::size_t>(out_w) +
            static_cast<std::size_t>(x * 4);
        for (int sx = 0; sx < 4; ++sx) {
          std::uint32_t cov = (c != 0u) ? 255u : 0u;
          if (c == 0u) {
            if (diag_tl) {
              const int sum = sx + sy;
              if (sum == 0) cov = 230u;
              else if (sum == 1) cov = 112u;
            }
            if (diag_tr) {
              const int sum = (3 - sx) + sy;
              if (sum == 0) cov = std::max(cov, 230u);
              else if (sum == 1) cov = std::max(cov, 112u);
            }
            if (diag_bl) {
              const int sum = sx + (3 - sy);
              if (sum == 0) cov = std::max(cov, 230u);
              else if (sum == 1) cov = std::max(cov, 112u);
            }
            if (diag_br) {
              const int sum = (3 - sx) + (3 - sy);
              if (sum == 0) cov = std::max(cov, 230u);
              else if (sum == 1) cov = std::max(cov, 112u);
            }
          } else {
            if (trim_tl && sx == 0 && sy == 0) cov = 160u;
            if (trim_tr && sx == 3 && sy == 0) cov = 160u;
            if (trim_bl && sx == 0 && sy == 3) cov = 160u;
            if (trim_br && sx == 3 && sy == 3) cov = 160u;
          }

          if (transparent_zero) {
            drow[sx] = (cov == 0u)
                           ? 0x00000000u
                           : ((cov << 24) | (fg_r << 16) | (fg_g << 8) | fg_b);
          } else {
            const std::uint32_t inv = 255u - cov;
            const std::uint32_t r = (fg_r * cov + bg_r * inv + 127u) / 255u;
            const std::uint32_t g = (fg_g * cov + bg_g * inv + 127u) / 255u;
            const std::uint32_t b = (fg_b * cov + bg_b * inv + 127u) / 255u;
            drow[sx] = 0xFF000000u | (r << 16) | (g << 8) | b;
          }
        }
      }
    }
  }
}

}  // namespace san4::hd_gen
