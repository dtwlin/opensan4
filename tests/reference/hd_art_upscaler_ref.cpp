// Frozen copy of the pre-LUT `upscale_art_4x_argb` (per-pixel std::pow /
// std::sqrt sRGB math). Used only by san4_tests to measure the optimized
// implementation against the original output and timing.
#include "reference/hd_art_upscaler_ref.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace san4::test_ref {
namespace {

struct Vec3 {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;

  Vec3 operator+(const Vec3& o) const { return {r + o.r, g + o.g, b + o.b}; }
  Vec3 operator-(const Vec3& o) const { return {r - o.r, g - o.g, b - o.b}; }
  Vec3 operator*(float s) const { return {r * s, g * s, b * s}; }
};

float srgb8_to_linear(std::uint8_t c8) {
  const float s = static_cast<float>(c8) * (1.0f / 255.0f);
  return (s <= 0.04045f) ? (s * (1.0f / 12.92f))
                         : std::pow((s + 0.055f) * (1.0f / 1.055f), 2.4f);
}

std::uint8_t linear_to_srgb8(float lin) {
  const float c = std::clamp(lin, 0.0f, 1.0f);
  const float s = (c <= 0.0031308f)
                      ? (c * 12.92f)
                      : (1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
  return static_cast<std::uint8_t>(
      std::clamp(static_cast<int>(std::lround(s * 255.0f)), 0, 255));
}

Vec3 argb_to_linear(std::uint32_t argb) {
  return Vec3{
      srgb8_to_linear(static_cast<std::uint8_t>((argb >> 16) & 0xFFu)),
      srgb8_to_linear(static_cast<std::uint8_t>((argb >> 8) & 0xFFu)),
      srgb8_to_linear(static_cast<std::uint8_t>(argb & 0xFFu)),
  };
}

float linear_luma(const Vec3& c) {
  return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

// Perceptual color distance in approximate sRGB/YCbCr space (returns >= 0).
float color_dist(const Vec3& a, const Vec3& b) {
  const float dr = std::sqrt(std::max(0.0f, a.r)) - std::sqrt(std::max(0.0f, b.r));
  const float dg = std::sqrt(std::max(0.0f, a.g)) - std::sqrt(std::max(0.0f, b.g));
  const float db = std::sqrt(std::max(0.0f, a.b)) - std::sqrt(std::max(0.0f, b.b));
  const float dy = 0.299f * dr + 0.587f * dg + 0.114f * db;
  const float dcb = 0.5f * (db - dy);
  const float dcr = 0.5f * (dr - dy);
  return std::sqrt(4.0f * dy * dy + 1.5f * dcb * dcb + 1.8f * dcr * dcr);
}

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

// Continuous-RGB 2x xBR step (`sw x sh` -> `(sw*2) x (sh*2)`) supporting
// optional periodic border wrap for seamless terrain tiles and 1px catchlight
// preservation on the first 1x->2x pass (so the second 2x->4x pass rounds the
// 1px dot into a crisp anti-aliased circular highlight instead of erasing it).
void xbr_2x_vec3(const std::vector<Vec3>& src, int sw, int sh, bool wrap_x,
                 bool wrap_y, bool preserve_isolated_1px,
                 std::vector<Vec3>& dst) {
  const int dw = sw * 2;
  const int dh = sh * 2;
  dst.resize(static_cast<std::size_t>(dw) * static_cast<std::size_t>(dh));

  auto at = [&](int x, int y) -> const Vec3& {
    const int cx = wrap_or_clamp(x, sw, wrap_x);
    const int cy = wrap_or_clamp(y, sh, wrap_y);
    return src[static_cast<std::size_t>(cy) * static_cast<std::size_t>(sw) +
               static_cast<std::size_t>(cx)];
  };

  constexpr float kEqThr = 0.045f;

  for (int y = 0; y < sh; ++y) {
    for (int x = 0; x < sw; ++x) {
      // 5x5 neighborhood around E = (x, y)
      const Vec3& a1 = at(x - 1, y - 2);
      const Vec3& b1 = at(x, y - 2);
      const Vec3& c1 = at(x + 1, y - 2);

      const Vec3& a0 = at(x - 2, y - 1);
      const Vec3& pa = at(x - 1, y - 1);
      const Vec3& pb = at(x, y - 1);
      const Vec3& pc = at(x + 1, y - 1);
      const Vec3& c4 = at(x + 2, y - 1);

      const Vec3& d0 = at(x - 2, y);
      const Vec3& pd = at(x - 1, y);
      const Vec3& pe = at(x, y);
      const Vec3& pf = at(x + 1, y);
      const Vec3& f4 = at(x + 2, y);

      const Vec3& g0 = at(x - 2, y + 1);
      const Vec3& pg = at(x - 1, y + 1);
      const Vec3& ph = at(x, y + 1);
      const Vec3& pi = at(x + 1, y + 1);
      const Vec3& i4 = at(x + 2, y + 1);

      const Vec3& g5 = at(x - 1, y + 2);
      const Vec3& h5 = at(x, y + 2);
      const Vec3& i5 = at(x + 1, y + 2);

      Vec3 e0 = pe;
      Vec3 e1 = pe;
      Vec3 e2 = pe;
      Vec3 e3 = pe;

      const bool is_isolated_dot =
          preserve_isolated_1px && color_dist(pe, pa) >= kEqThr &&
          color_dist(pe, pb) >= kEqThr && color_dist(pe, pc) >= kEqThr &&
          color_dist(pe, pd) >= kEqThr && color_dist(pe, pf) >= kEqThr &&
          color_dist(pe, pg) >= kEqThr && color_dist(pe, ph) >= kEqThr &&
          color_dist(pe, pi) >= kEqThr;

      auto eval_corner = [&](Vec3& corner, Vec3& adj_cw, Vec3& adj_ccw,
                             const Vec3& H, const Vec3& F, const Vec3& I,
                             const Vec3& G, const Vec3& C, const Vec3& D,
                             const Vec3& B, const Vec3& F4, const Vec3& I4,
                             const Vec3& H5, const Vec3& I5, const Vec3& G5,
                             const Vec3& C4) {
        const float d_ef = color_dist(pe, F);
        const float d_eh = color_dist(pe, H);
        if (d_ef < 0.004f && d_eh < 0.004f) {
          return;
        }
        const float wd1 = color_dist(pe, C) + color_dist(pe, G) +
                          color_dist(I, H5) + color_dist(I, F4) +
                          4.0f * color_dist(H, F);
        const float wd2 = color_dist(H, D) + color_dist(H, I5) +
                          color_dist(F, I4) + color_dist(F, B) +
                          4.0f * color_dist(pe, I);
        if (wd1 >= wd2) {
          return;
        }
        const Vec3 nc = (d_ef <= d_eh) ? F : H;
        const float dfg = color_dist(F, G);
        const float dch = color_dist(C, H);
        const bool fg_steep =
            (2.0f * dfg <= dch) && (color_dist(pe, G) >= kEqThr) &&
            (color_dist(D, G) >= kEqThr || color_dist(pe, D) < kEqThr);
        const bool ch_steep =
            (2.0f * dch <= dfg) && (color_dist(pe, C) >= kEqThr) &&
            (color_dist(B, C) >= kEqThr || color_dist(pe, B) < kEqThr);
        (void)G5;
        (void)C4;

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
        eval_corner(e3, e1, e2, ph, pf, pi, pg, pc, pd, pb, f4, i4, h5, i5, g5,
                    c4);
        eval_corner(e2, e3, e0, pd, ph, pg, pa, pi, pb, pf, h5, g5, d0, g0, i5,
                    a0);
        eval_corner(e1, e0, e3, pf, pb, pc, pi, pa, ph, pd, b1, c1, f4, c4, i4,
                    a1);
        eval_corner(e0, e2, e1, pb, pd, pa, pc, pg, pf, ph, d0, a0, b1, a1, c1,
                    g0);
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
}

void xbr_2x_scalar(const std::vector<float>& src, int sw, int sh,
                   bool preserve_isolated_1px, std::vector<float>& dst) {
  const int dw = sw * 2;
  const int dh = sh * 2;
  dst.resize(static_cast<std::size_t>(dw) * static_cast<std::size_t>(dh));

  auto at = [&](int x, int y) -> float {
    const int cx = std::clamp(x, 0, sw - 1);
    const int cy = std::clamp(y, 0, sh - 1);
    return src[static_cast<std::size_t>(cy) * static_cast<std::size_t>(sw) +
               static_cast<std::size_t>(cx)];
  };

  for (int y = 0; y < sh; ++y) {
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
        const bool fg_steep = (2.0f * dfg <= dch) && (std::fabs(pe - G) > 0.1f);
        const bool ch_steep = (2.0f * dch <= dfg) && (std::fabs(pe - C) > 0.1f);
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
}

}  // namespace

float srgb8_to_linear_reference(std::uint8_t c8) { return srgb8_to_linear(c8); }

std::uint8_t linear_to_srgb8_reference(float lin) {
  return linear_to_srgb8(lin);
}

void upscale_art_4x_argb_reference(const std::uint8_t* vga_slots,
                                   const std::uint8_t* opaque_mask, int w,
                                   int h,
                                   const std::array<std::uint32_t, 16>& slot_palette,
                                   hd_gen::ArtAssetKind kind,
                                   std::vector<std::uint32_t>& out_argb_4x) {
  if (w <= 0 || h <= 0 || vga_slots == nullptr) {
    out_argb_4x.clear();
    return;
  }

  const int out_w = w * 4;
  const int out_h = h * 4;
  const std::size_t n_src =
      static_cast<std::size_t>(w) * static_cast<std::size_t>(h);

  std::array<Vec3, 16> pal_lin{};
  std::array<float, 16> pal_luma{};
  for (std::size_t i = 0; i < 16u; ++i) {
    pal_lin[i] = argb_to_linear(slot_palette[i]);
    pal_luma[i] = linear_luma(pal_lin[i]);
  }

  auto is_opaque = [&](int x, int y) -> bool {
    if (opaque_mask == nullptr) return true;
    return opaque_mask[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                       static_cast<std::size_t>(x)] != 0u;
  };

  // Detect seamless periodic edges on opaque TerrainTile assets (e.g. ocean,
  // plains, grass, city ground) so 16x16 tiles stitch with zero grid seams!
  bool wrap_x = false;
  bool wrap_y = false;
  if (kind == hd_gen::ArtAssetKind::TerrainTile && opaque_mask == nullptr && w >= 8 &&
      h >= 8) {
    int match_lr = 0;
    for (int y = 0; y < h; ++y) {
      const std::uint8_t s0 = vga_slots[y * w + 0] & 0x0Fu;
      const std::uint8_t s1 = vga_slots[y * w + (w - 1)] & 0x0Fu;
      const std::uint8_t s1_up =
          vga_slots[std::max(0, y - 1) * w + (w - 1)] & 0x0Fu;
      const std::uint8_t s1_dn =
          vga_slots[std::min(h - 1, y + 1) * w + (w - 1)] & 0x0Fu;
      if (color_dist(pal_lin[s0], pal_lin[s1]) < 0.22f || s0 == s1_up ||
          s0 == s1_dn) {
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
      if (color_dist(pal_lin[s0], pal_lin[s1]) < 0.22f || s0 == s1_lt ||
          s0 == s1_rt) {
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
      (kind == hd_gen::ArtAssetKind::UiChrome) ? 0.28f : 0.62f;

  std::vector<Vec3> dedith_pass1 = base_lin;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const std::size_t idx =
          static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
          static_cast<std::size_t>(x);
      if (!is_opaque(x, y)) {
        continue;
      }

      const std::uint8_t c0 = slot_at(x, y);
      const Vec3& c0_lin = pal_lin[c0];

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
      const std::uint8_t cnw = opq_at(x - 1, y - 1) ? slot_at(x - 1, y - 1) : c0;
      const std::uint8_t cne = opq_at(x + 1, y - 1) ? slot_at(x + 1, y - 1) : c0;
      const std::uint8_t csw = opq_at(x - 1, y + 1) ? slot_at(x - 1, y + 1) : c0;
      const std::uint8_t cse = opq_at(x + 1, y + 1) ? slot_at(x + 1, y + 1) : c0;

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
          if (color_dist(c0_lin, pal_lin[other]) <= max_pair_dist) {
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

      // 2) Detect 25% / 75% Bayer stipple (all 4 cardinals == B != c0, and at
      //    least one diagonal or step-2 cardinal == c0 so isolated 1px eye
      //    catchlights are NEVER blurred!).
      if (partner < 0 && kind != hd_gen::ArtAssetKind::UiChrome) {
        if (cn == cs && cw == ce && cn == cw && cn != c0 &&
            color_dist(c0_lin, pal_lin[cn]) <= max_pair_dist) {
          const bool periodic_repeat =
              (cnw == c0 || cne == c0 || csw == c0 || cse == c0 ||
               (opq_at(x - 2, y) && slot_at(x - 2, y) == c0) ||
               (opq_at(x + 2, y) && slot_at(x + 2, y) == c0) ||
               (opq_at(x, y - 2) && slot_at(x, y - 2) == c0) ||
               (opq_at(x, y + 2) && slot_at(x, y + 2) == c0));
          // Extra guard for bright 1px eye catchlights in portraits:
          const bool isolated_catchlight =
              (kind == hd_gen::ArtAssetKind::Portrait && pal_luma[c0] > 0.65f &&
               pal_luma[cn] < 0.20f && cnw != c0 && cne != c0 && csw != c0 &&
               cse != c0);
          if (periodic_repeat && !isolated_catchlight) {
            partner = static_cast<int>(cn);
          }
        }
      }

      // 3) Detect 1D horizontal/vertical halftone (`cw == ce != c0` with
      //    alternating or matching `{c0, cw}` above/below, NOT a 1px vertical
      //    bevel line!).
      if (partner < 0 && kind != hd_gen::ArtAssetKind::UiChrome) {
        if (cw == ce && cw != c0 &&
            color_dist(c0_lin, pal_lin[cw]) <= max_pair_dist * 0.85f) {
          const bool repeat_x =
              (opq_at(x - 2, y) && slot_at(x - 2, y) == c0) ||
              (opq_at(x + 2, y) && slot_at(x + 2, y) == c0);
          const bool not_solid_line = (cn == cw || cs == cw);
          if (repeat_x && not_solid_line) {
            partner = static_cast<int>(cw);
          }
        } else if (cn == cs && cn != c0 &&
                   color_dist(c0_lin, pal_lin[cn]) <= max_pair_dist * 0.85f) {
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
      const Vec3& cp_lin = pal_lin[static_cast<std::size_t>(partner)];
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
          const Vec3& nl = pal_lin[ns];
          // Accept pixels belonging to `{c0, partner}` or perceptually close
          // intermediate shades in a multi-band halftone ramp.
          if (ns == c0 || ns == static_cast<std::uint8_t>(partner) ||
              (color_dist(nl, c0_lin) <= max_pair_dist * 0.7f &&
               color_dist(nl, cp_lin) <= max_pair_dist * 0.7f)) {
            const float kw = kKernel[ky + 1][kx + 1];
            accum = accum + nl * kw;
            wsum += kw;
          }
        }
      }
      if (wsum > 0.0f) {
        dedith_pass1[idx] = accum * (1.0f / wsum);
      }
    }
  }

  // Gentle second-pass bilateral polish purely inside detected halftone zones
  // so 25% -> 50% -> 75% Bayer bands transition without step banding.
  std::vector<Vec3> stage1_lin = dedith_pass1;
  for (int y = 0; y < h; ++y) {
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
          const float d = color_dist(center, nv);
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

  // =========================================================================
  // STAGE 2: Pre-Dilated RGB + Hybrid `4x` xBR + Clamped Catmull-Rom Blending
  // =========================================================================
  if (opaque_mask != nullptr) {
    std::vector<std::uint8_t> valid(opaque_mask, opaque_mask + n_src);
    for (int iter = 0; iter < 3; ++iter) {
      std::vector<Vec3> next_rgb = stage1_lin;
      std::vector<std::uint8_t> next_valid = valid;
      for (int y = 0; y < h; ++y) {
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
      stage1_lin = std::move(next_rgb);
      valid = std::move(next_valid);
    }
  }

  // 2a) Two-pass continuous-RGB xBR (`1x -> 2x -> 4x`)
  std::vector<Vec3> xbr2x;
  std::vector<Vec3> xbr4x;
  xbr_2x_vec3(stage1_lin, w, h, wrap_x, wrap_y, true, xbr2x);
  xbr_2x_vec3(xbr2x, w * 2, h * 2, wrap_x, wrap_y, false, xbr4x);

  // 2b) Separable Clamped Catmull-Rom (`4x`) + local edge-strength fusion
  static constexpr std::array<float, 4> kSubOffsets = {
      0.125f, 0.375f, 0.625f, 0.875f,
  };
  auto sample_stage1 = [&](int x, int y) -> const Vec3& {
    const int cx = wrap_or_clamp(x, w, wrap_x);
    const int cy = wrap_or_clamp(y, h, wrap_y);
    return stage1_lin[static_cast<std::size_t>(cy) * static_cast<std::size_t>(w) +
                      static_cast<std::size_t>(cx)];
  };

  std::vector<Vec3> fused4x(
      static_cast<std::size_t>(out_w) * static_cast<std::size_t>(out_h));
  const float bicubic_cap = (kind == hd_gen::ArtAssetKind::UiChrome) ? 0.20f : 0.72f;

  for (int oy = 0; oy < out_h; ++oy) {
    const float fy = (static_cast<float>(oy) + 0.5f) * 0.25f - 0.5f;
    const int iy = static_cast<int>(std::floor(fy));
    const float ty = fy - static_cast<float>(iy);
    const float wy[4] = {
        catmull_rom_weight(-1.0f - ty),
        catmull_rom_weight(0.0f - ty),
        catmull_rom_weight(1.0f - ty),
        catmull_rom_weight(2.0f - ty),
    };
    (void)kSubOffsets;

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
          row_acc = row_acc + sample_stage1(ix - 1 + i, iy - 1 + j) * wx[i];
        }
        bic = bic + row_acc * wy[j];
      }

      // Clamp strictly within local 2x2 neighborhood -> zero ringing/halos!
      const Vec3& p00 = sample_stage1(ix, iy);
      const Vec3& p10 = sample_stage1(ix + 1, iy);
      const Vec3& p01 = sample_stage1(ix, iy + 1);
      const Vec3& p11 = sample_stage1(ix + 1, iy + 1);
      const Vec3 lo = min3(min3(p00, p10), min3(p01, p11));
      const Vec3 hi = max3(max3(p00, p10), max3(p01, p11));
      bic = clamp3(bic, lo, hi);

      // Local contrast in the 2x2 cell drives blend between smooth Catmull-Rom
      // (low-gradient shading) and directional xBR (crisp contours & lines).
      const float max_d =
          std::max({color_dist(p00, p10), color_dist(p00, p01),
                    color_dist(p10, p11), color_dist(p01, p11),
                    color_dist(p00, p11), color_dist(p10, p01)});
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
    xbr_2x_scalar(a1x, w, h, true, a2x);
    xbr_2x_scalar(a2x, w * 2, h * 2, false, alpha4x);
  }

  out_argb_4x.resize(static_cast<std::size_t>(out_w) *
                     static_cast<std::size_t>(out_h));

  auto fused_at = [&](int x, int y) -> const Vec3& {
    const int cx = wrap_or_clamp(x, out_w, wrap_x);
    const int cy = wrap_or_clamp(y, out_h, wrap_y);
    return fused4x[static_cast<std::size_t>(cy) * static_cast<std::size_t>(out_w) +
                   static_cast<std::size_t>(cx)];
  };

  for (int oy = 0; oy < out_h; ++oy) {
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

      const std::uint32_t r8 = linear_to_srgb8(sharp.r);
      const std::uint32_t g8 = linear_to_srgb8(sharp.g);
      const std::uint32_t b8 = linear_to_srgb8(sharp.b);
      out_argb_4x[oidx] =
          (static_cast<std::uint32_t>(a8) << 24) | (r8 << 16) | (g8 << 8) | b8;
    }
  }
}

}  // namespace san4::test_ref
