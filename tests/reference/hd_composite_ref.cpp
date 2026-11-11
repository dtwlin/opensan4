#include "reference/hd_composite_ref.h"

#include <algorithm>
#include <array>
#include <unordered_map>

namespace san4 {
namespace {

// Copy of hd_pack.cpp's trilinear 17^3 LUT sampler.
std::uint32_t sample_lut_trilinear_ref(
    const std::array<std::uint32_t, 17 * 17 * 17>& grid, std::uint32_t rgb) {
  const int r = static_cast<int>((rgb >> 16) & 0xFFu);
  const int g = static_cast<int>((rgb >> 8) & 0xFFu);
  const int b = static_cast<int>(rgb & 0xFFu);

  const int ri = (r * 16) / 255;
  const int gi = (g * 16) / 255;
  const int bi = (b * 16) / 255;
  const int r1 = std::min(ri + 1, 16);
  const int g1 = std::min(gi + 1, 16);
  const int b1 = std::min(bi + 1, 16);

  const int rf = r * 16 - ri * 255;
  const int gf = g * 16 - gi * 255;
  const int bf = b * 16 - bi * 255;

  auto idx = [](int ir, int ig, int ib) {
    return static_cast<std::size_t>((ir * 17 + ig) * 17 + ib);
  };

  const std::uint32_t c000 = grid[idx(ri, gi, bi)];
  const std::uint32_t c001 = grid[idx(ri, gi, b1)];
  const std::uint32_t c010 = grid[idx(ri, g1, bi)];
  const std::uint32_t c011 = grid[idx(ri, g1, b1)];
  const std::uint32_t c100 = grid[idx(r1, gi, bi)];
  const std::uint32_t c101 = grid[idx(r1, gi, b1)];
  const std::uint32_t c110 = grid[idx(r1, g1, bi)];
  const std::uint32_t c111 = grid[idx(r1, g1, b1)];

  auto interp_ch = [&](int shift) -> std::uint32_t {
    const int v000 = static_cast<int>((c000 >> shift) & 0xFFu);
    const int v001 = static_cast<int>((c001 >> shift) & 0xFFu);
    const int v010 = static_cast<int>((c010 >> shift) & 0xFFu);
    const int v011 = static_cast<int>((c011 >> shift) & 0xFFu);
    const int v100 = static_cast<int>((c100 >> shift) & 0xFFu);
    const int v101 = static_cast<int>((c101 >> shift) & 0xFFu);
    const int v110 = static_cast<int>((c110 >> shift) & 0xFFu);
    const int v111 = static_cast<int>((c111 >> shift) & 0xFFu);

    const int v00 = (v000 * (255 - bf) + v001 * bf + 127) / 255;
    const int v01 = (v010 * (255 - bf) + v011 * bf + 127) / 255;
    const int v10 = (v100 * (255 - bf) + v101 * bf + 127) / 255;
    const int v11 = (v110 * (255 - bf) + v111 * bf + 127) / 255;

    const int v0 = (v00 * (255 - gf) + v01 * gf + 127) / 255;
    const int v1 = (v10 * (255 - gf) + v11 * gf + 127) / 255;

    const int v = (v0 * (255 - rf) + v1 * rf + 127) / 255;
    return static_cast<std::uint32_t>(std::clamp(v, 0, 255));
  };

  return (interp_ch(16) << 16) | (interp_ch(8) << 8) | interp_ch(0);
}

// xorshift64* for synthetic data.
class SeedRng {
 public:
  explicit SeedRng(std::uint64_t seed) : s_(seed != 0u ? seed : 1u) {}
  std::uint32_t next() {
    s_ ^= s_ >> 12;
    s_ ^= s_ << 25;
    s_ ^= s_ >> 27;
    return static_cast<std::uint32_t>((s_ * 0x2545F4914F6CDD1Dull) >> 32);
  }
  std::uint32_t below(std::uint32_t n) { return next() % n; }

 private:
  std::uint64_t s_;
};

}  // namespace

void HdPackManagerTestPeer::composite_overlays_reference(
    const HdPackManager& hd, std::vector<std::uint32_t>& canvas, int out_w,
    int out_h, int scale, const VesaPlanarVideo& video,
    UpscaleDirtyRect& inout_dirty_rect) {
  using Lut = HdPackManager::PaletteGradingLut;
  constexpr int kHdScale = HdPackManager::kHdScale;
  constexpr int kCanvasW = HdPackManager::kCanvasW;

  hd.sync_vram_occlusions(video);
  if (hd.active_region_count_ == 0u || scale <= 0 || out_w <= 0 ||
      out_h <= 0) {
    return;
  }
  hd.hd_dirty_rects_.clear();
  hd.refresh_dynamic_cgs(video);
  for (const UpscaleDirtyRect& r : hd.hd_dirty_rects_) {
    const int ox0 = r.x * scale;
    const int oy0 = r.y * scale;
    const int ox1 = (r.x + r.w) * scale;
    const int oy1 = (r.y + r.h) * scale;
    if (!inout_dirty_rect.has_changes) {
      inout_dirty_rect.has_changes = true;
      inout_dirty_rect.x = ox0;
      inout_dirty_rect.y = oy0;
      inout_dirty_rect.w = ox1 - ox0;
      inout_dirty_rect.h = oy1 - oy0;
    } else {
      const int ux0 = std::min(inout_dirty_rect.x, ox0);
      const int uy0 = std::min(inout_dirty_rect.y, oy0);
      const int ux1 = std::max(inout_dirty_rect.x + inout_dirty_rect.w, ox1);
      const int uy1 = std::max(inout_dirty_rect.y + inout_dirty_rect.h, oy1);
      inout_dirty_rect.x = ux0;
      inout_dirty_rect.y = uy0;
      inout_dirty_rect.w = ux1 - ux0;
      inout_dirty_rect.h = uy1 - uy0;
    }
  }
  hd.hd_dirty_rects_.clear();
  if (!inout_dirty_rect.has_changes) {
    return;
  }

  static const Lut kIdentityLut{};
  std::unordered_map<std::uint64_t, Lut> luts;
  auto get_lut = [&](std::uint16_t pal_id, std::uint8_t bank,
                     std::uint16_t used_mask) -> const Lut& {
    if (pal_id >= hd.ref_palettes_.size()) {
      return kIdentityLut;
    }
    const std::uint16_t eff =
        HdPackManager::effective_grading_mask(bank, used_mask);
    const std::uint64_t key = HdPackManager::grading_lut_key(pal_id, bank, eff);
    auto it = luts.find(key);
    if (it == luts.end()) {
      it = luts.emplace(key, Lut{}).first;
      hd.build_grading_lut(pal_id, eff, video, it->second);
    }
    return it->second;
  };

  const auto& lvram = video.latched_vram();
  const int clip_ox0 = std::clamp(inout_dirty_rect.x, 0, out_w);
  const int clip_oy0 = std::clamp(inout_dirty_rect.y, 0, out_h);
  const int clip_ox1 =
      std::clamp(inout_dirty_rect.x + inout_dirty_rect.w, 0, out_w);
  const int clip_oy1 =
      std::clamp(inout_dirty_rect.y + inout_dirty_rect.h, 0, out_h);
  if (clip_ox0 >= clip_ox1 || clip_oy0 >= clip_oy1) {
    return;
  }

  const int lx0 = std::clamp(clip_ox0 / scale, 0, VesaPlanarVideo::kWidth);
  const int ly0 = std::clamp(clip_oy0 / scale, 0, VesaPlanarVideo::kHeight);
  const int lx1 = std::clamp((clip_ox1 + scale - 1) / scale, 0,
                             VesaPlanarVideo::kWidth);
  const int ly1 = std::clamp((clip_oy1 + scale - 1) / scale, 0,
                             VesaPlanarVideo::kHeight);

  std::uint16_t prev_pal_id = 0xFFFEu;
  std::uint8_t prev_bank = 0xFFu;
  std::uint32_t prev_reg = 0u;
  const Lut* cur_lut = nullptr;

  const int ds = kHdScale / std::clamp(scale, 1, kHdScale);

  for (int ly = ly0; ly < ly1; ++ly) {
    for (int lx = lx0; lx < lx1; ++lx) {
      const std::size_t lidx =
          static_cast<std::size_t>(ly) * VesaPlanarVideo::kWidth +
          static_cast<std::size_t>(lx);
      const std::uint32_t reg = hd.hd_region_id_[lidx];
      if (reg == 0u) {
        continue;
      }
      if (lvram[lidx] != hd.hd_snap_slot_[lidx]) {
        continue;
      }

      const std::uint16_t pal_id = hd.hd_ref_pal_id_[lidx];
      const std::uint8_t bank = hd.hd_bank_hint_[lidx];
      if (pal_id != prev_pal_id || bank != prev_bank || reg != prev_reg) {
        prev_pal_id = pal_id;
        prev_bank = bank;
        prev_reg = reg;
        cur_lut = &get_lut(pal_id, bank, hd.region_used_mask_[reg]);
      }
      const bool do_grade = (cur_lut != nullptr && !cur_lut->is_identity);

      const int ox_base = lx * scale;
      const int oy_base = ly * scale;
      const int hx_base = lx * kHdScale;
      const int hy_base = ly * kHdScale;

      for (int sy = 0; sy < scale; ++sy) {
        const int oy = oy_base + sy;
        if (oy < clip_oy0 || oy >= clip_oy1) continue;
        std::uint32_t* crow =
            canvas.data() +
            static_cast<std::size_t>(oy) * static_cast<std::size_t>(out_w);

        for (int sx = 0; sx < scale; ++sx) {
          const int ox = ox_base + sx;
          if (ox < clip_ox0 || ox >= clip_ox1) continue;

          std::uint32_t spx = 0u;
          if (ds == 1) {
            spx = hd.hd_canvas_argb_[static_cast<std::size_t>(hy_base + sy) *
                                         static_cast<std::size_t>(kCanvasW) +
                                     static_cast<std::size_t>(hx_base + sx)];
          } else {
            std::uint32_t sum_a = 0, sum_r = 0, sum_g = 0, sum_b = 0;
            std::uint32_t cnt_a = 0;
            for (int by = 0; by < ds; ++by) {
              const std::uint32_t* hrow =
                  hd.hd_canvas_argb_.data() +
                  static_cast<std::size_t>(hy_base + sy * ds + by) *
                      static_cast<std::size_t>(kCanvasW) +
                  static_cast<std::size_t>(hx_base + sx * ds);
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
            if (avg_a > 0u && cnt_a > 0u) {
              const std::uint32_t r = (sum_r + cnt_a / 2u) / cnt_a;
              const std::uint32_t g = (sum_g + cnt_a / 2u) / cnt_a;
              const std::uint32_t b = (sum_b + cnt_a / 2u) / cnt_a;
              spx = (avg_a << 24) | (r << 16) | (g << 8) | b;
            }
          }

          const std::uint32_t sa = (spx >> 24) & 0xFFu;
          if (sa == 0u) {
            continue;
          }

          std::uint32_t graded_rgb = spx & 0x00FFFFFFu;
          if (do_grade) {
            graded_rgb = sample_lut_trilinear_ref(cur_lut->grid, graded_rgb);
          }

          if (sa == 255u) {
            crow[ox] = 0xFF000000u | graded_rgb;
          } else {
            const std::uint32_t dpx = crow[ox];
            const std::uint32_t inv_sa = 255u - sa;
            const std::uint32_t r = (((graded_rgb >> 16) & 0xFFu) * sa +
                                     ((dpx >> 16) & 0xFFu) * inv_sa + 127u) /
                                    255u;
            const std::uint32_t g = (((graded_rgb >> 8) & 0xFFu) * sa +
                                     ((dpx >> 8) & 0xFFu) * inv_sa + 127u) /
                                    255u;
            const std::uint32_t b =
                ((graded_rgb & 0xFFu) * sa + (dpx & 0xFFu) * inv_sa + 127u) /
                255u;
            crow[ox] = 0xFF000000u | (r << 16) | (g << 8) | b;
          }
        }
      }
    }
  }
}

void HdPackManagerTestPeer::seed_synthetic_regions(
    HdPackManager& hd, const VesaPlanarVideo& video,
    const SyntheticRegionSpec& spec) {
  SeedRng rng(spec.seed);
  hd.clear_overlays();
  hd.clear_pending_ = false;

  std::vector<std::uint16_t> pal_ids;
  for (int p = 0; p < spec.num_palettes; ++p) {
    std::array<std::uint32_t, 16> pal{};
    for (std::uint32_t& c : pal) {
      c = 0xFF000000u | (rng.next() & 0x00FFFFFFu);
    }
    pal_ids.push_back(hd.intern_palette(pal));
  }
  pal_ids.push_back(HdPackManager::kDirectUngradedPalId);

  const auto n_regions = static_cast<std::size_t>(spec.num_regions);
  std::vector<std::uint16_t> region_pal(n_regions + 1u);
  std::vector<std::uint8_t> region_bank(n_regions + 1u);
  hd.region_refcounts_.assign(n_regions + 1u, 0u);
  hd.region_used_mask_.assign(n_regions + 1u, 0u);
  hd.free_region_ids_.clear();
  for (std::size_t r = 1; r <= n_regions; ++r) {
    region_pal[r] = pal_ids[rng.below(static_cast<std::uint32_t>(pal_ids.size()))];
    if (spec.unique_masks) {
      region_bank[r] = 0u;
      hd.region_used_mask_[r] = static_cast<std::uint16_t>(r);
    } else {
      region_bank[r] = static_cast<std::uint8_t>(rng.below(4u));
      // Zero masks exercise the whole-bank fallback.
      hd.region_used_mask_[r] =
          (rng.below(5u) == 0u) ? 0u : static_cast<std::uint16_t>(rng.next());
    }
  }

  const auto& lvram = video.latched_vram();
  for (int ly = spec.y; ly < spec.y + spec.h; ++ly) {
    for (int lx = spec.x; lx < spec.x + spec.w; ++lx) {
      const std::size_t lidx =
          static_cast<std::size_t>(ly) * VesaPlanarVideo::kWidth +
          static_cast<std::size_t>(lx);
      std::uint32_t reg = 1u + static_cast<std::uint32_t>(
                                   ((lx / 6) * 7 + (ly / 3) * 13) %
                                   static_cast<int>(n_regions));
      const std::uint32_t roll = rng.below(10u);
      if (roll == 0u) {
        reg = 0u;
      } else if (roll == 1u) {
        reg = 1u + rng.below(static_cast<std::uint32_t>(n_regions));
      }
      hd.hd_region_id_[lidx] = reg;
      if (reg != 0u) {
        ++hd.region_refcounts_[reg];
        hd.hd_ref_pal_id_[lidx] =
            (rng.below(20u) == 0u)
                ? pal_ids[rng.below(static_cast<std::uint32_t>(pal_ids.size()))]
                : region_pal[reg];
        hd.hd_bank_hint_[lidx] = region_bank[reg];
      }
      hd.hd_snap_slot_[lidx] = (rng.below(10u) == 0u)
                                   ? static_cast<std::uint8_t>(lvram[lidx] ^ 1u)
                                   : lvram[lidx];
      for (int sy = 0; sy < HdPackManager::kHdScale; ++sy) {
        std::uint32_t* row =
            hd.hd_canvas_argb_.data() +
            static_cast<std::size_t>(ly * HdPackManager::kHdScale + sy) *
                HdPackManager::kCanvasW +
            static_cast<std::size_t>(lx * HdPackManager::kHdScale);
        for (int sx = 0; sx < HdPackManager::kHdScale; ++sx) {
          const std::uint32_t a_roll = rng.below(20u);
          const std::uint32_t a = (a_roll < 5u)    ? 0u
                                  : (a_roll < 12u) ? 255u
                                                   : 1u + rng.below(254u);
          row[sx] = (a << 24) | (rng.next() & 0x00FFFFFFu);
        }
      }
    }
  }
  hd.active_region_count_ = static_cast<std::size_t>(
      std::count_if(hd.region_refcounts_.begin() + 1, hd.region_refcounts_.end(),
                    [](std::uint32_t c) { return c > 0u; }));
}

std::uint16_t HdPackManagerTestPeer::region_used_mask_at(
    const HdPackManager& hd, int x, int y) {
  const std::uint32_t reg =
      hd.hd_region_id_[static_cast<std::size_t>(y) * VesaPlanarVideo::kWidth +
                       static_cast<std::size_t>(x)];
  return (reg == 0u) ? 0u : hd.region_used_mask_[reg];
}

std::size_t HdPackManagerTestPeer::tile_dict_index_size(
    const HdPackManager& hd) {
  return hd.tile_dict_index_.size();
}

void HdPackManagerTestPeer::set_cg_upscale_fn(HdPackManager& hd,
                                              CgUpscaleFn fn) {
  hd.cg_upscale_fn_ = fn;
}

HdPackManagerTestPeer::CgUpscaleFn
HdPackManagerTestPeer::default_cg_upscale_fn() {
  return &HdPackManager::upscale_cg_4x;
}

bool HdPackManagerTestPeer::pal_id_valid_at(const HdPackManager& hd, int x,
                                            int y) {
  const std::size_t lidx =
      static_cast<std::size_t>(y) * VesaPlanarVideo::kWidth +
      static_cast<std::size_t>(x);
  const std::uint16_t pal = hd.hd_ref_pal_id_[lidx];
  return hd.hd_region_id_[lidx] == 0u ||
         pal == HdPackManager::kDirectUngradedPalId ||
         pal < hd.ref_palettes_.size();
}

}  // namespace san4
