#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "hd_pack.h"
#include "upscaler.h"
#include "video.h"

namespace san4 {

// Test-only access to HdPackManager internals (declared a friend in
// hd_pack.h).
struct HdPackManagerTestPeer {
  // The original serial composite_overlays loop (scalar per-pixel blend,
  // per-pixel LUT switching), keeping the Stage 2 used-color-mask LUT
  // semantics: LUTs come from the production builder keyed by
  // (palette, bank, effective used mask), but are cached locally so the
  // manager's LUT cache is untouched.
  static void composite_overlays_reference(const HdPackManager& hd,
                                           std::vector<std::uint32_t>& canvas,
                                           int out_w, int out_h, int scale,
                                           const VesaPlanarVideo& video,
                                           UpscaleDirtyRect& inout_dirty_rect);

  struct SyntheticRegionSpec {
    std::uint64_t seed = 1;
    // Logical rectangle that receives HD pixels.
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int num_regions = 16;
    int num_palettes = 4;
    // When true every region gets a distinct used mask and bank 0, so each
    // region needs its own grading LUT.
    bool unique_masks = false;
  };

  // Replaces all HD canvas and metadata with synthetic regions: runs of
  // equal regions broken by per-pixel noise, random banks, palettes
  // (including ungraded), used masks, mixed alpha (0, 255 and translucent)
  // and ~10% shadow-VRAM guard mismatches.
  static void seed_synthetic_regions(HdPackManager& hd,
                                     const VesaPlanarVideo& video,
                                     const SyntheticRegionSpec& spec);

  static std::uint16_t region_used_mask_at(const HdPackManager& hd, int x,
                                           int y);
  static std::size_t tile_dict_index_size(const HdPackManager& hd);

  // Replaces the dynamic-CG 4x re-upscaler (sync refreshes and async jobs
  // enqueued afterwards).
  using CgUpscaleFn = void (*)(const std::uint8_t* vga_slots, int w, int h,
                               const std::array<std::uint32_t, 16>& palette,
                               std::vector<std::uint32_t>& out_argb);
  static void set_cg_upscale_fn(HdPackManager& hd, CgUpscaleFn fn);
  // The production re-upscaler.
  static CgUpscaleFn default_cg_upscale_fn();

  // True when logical pixel (x, y) carries no HD region or a palette id that
  // names an interned palette.
  static bool pal_id_valid_at(const HdPackManager& hd, int x, int y);
};

}  // namespace san4
