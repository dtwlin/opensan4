#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace san4 {
class ThreadPool;
}  // namespace san4

namespace san4::hd_gen {

enum class ArtAssetKind : std::uint8_t {
  CgIllustration = 0,  // NPK016 full-screen / event CGs
  Portrait = 1,        // KAODATA 64x80 portraits & MONDATA parts
  TerrainTile = 2,     // 16x16 map/city/battle base tiles (auto-periodic wrap)
  MaskedSprite = 3,    // Color-0 transparent units, duelists, overlays, FX
  UiChrome = 4,        // 32x32 command buttons, plaques, borders, Kanji labels
};

// Table-driven sRGB transfer functions, bit-identical to evaluating the
// IEC 61966-2-1 piecewise formulas in float with std::pow:
//   srgb8_to_linear: c/12.92 or ((c + 0.055) / 1.055)^2.4 for c = c8 / 255
//   linear_to_srgb8: lround(255 * (12.92 l or 1.055 l^(1/2.4) - 0.055)) with
//                    l clamped to [0, 1] (NaN maps to 0)
float srgb8_to_linear(std::uint8_t c8);
std::uint8_t linear_to_srgb8(float linear);

// Upscales a 4-bit VGA-slot image (`w x h`) by `4x` (`(w*4) x (h*4)`) into
// 32-bit ARGB8888 (`0xAARRGGBB`) using the 3-stage artistic pipeline:
//   Stage 1: Linear-RGB edge-preserving halftone/checkerboard de-dithering
//            (locking ink contours, 1px eye catchlights, and UI bevels)
//   Stage 2: Pre-dilated foreground RGB + hybrid `4x` directional xBR contour
//            reconstruction + clamped Catmull-Rom smooth interior gradients
//   Stage 3: Halo-free clamped micro-contrast + `4x` anti-aliased 8-bit alpha
// `opaque_mask` may be nullptr (all pixels opaque alpha=255) or size `w * h`
// (`1` = opaque, `0` = transparent).
void upscale_art_4x_argb(const std::uint8_t* vga_slots,
                         const std::uint8_t* opaque_mask, int w, int h,
                         const std::array<std::uint32_t, 16>& slot_palette,
                         ArtAssetKind kind,
                         std::vector<std::uint32_t>& out_argb_4x);

// Same as above; when `pool` is non-null, each pass is split across the pool
// in row strips. The output is identical with or without a pool.
void upscale_art_4x_argb(const std::uint8_t* vga_slots,
                         const std::uint8_t* opaque_mask, int w, int h,
                         const std::array<std::uint32_t, 16>& slot_palette,
                         ArtAssetKind kind,
                         std::vector<std::uint32_t>& out_argb_4x,
                         ThreadPool* pool);

// High-DPI `4x` rasterizer for 1-bit MSB-decoded monochrome bitmaps (`AH=0x13`
// Big5 `.16P` glyphs, `FONT.S4` Gaiji, `8x16` ASCII, and `64x48` UI patterns).
// When `transparent_zero` is true (`mono_rop == 4`), `0`-bits produce alpha=0
// and `1`-bits produce crisp anti-aliased `fg_argb` strokes (`alpha` 0..255).
// When `transparent_zero` is false (`mono_rop == 0` or `5`), blends cleanly
// between `bg_argb` and `fg_argb` with alpha=255.
void rasterize_mono_1bit_4x(const std::uint8_t* bits_1bit, int w, int h,
                            std::uint32_t fg_argb, std::uint32_t bg_argb,
                            bool transparent_zero,
                            std::vector<std::uint32_t>& out_argb_4x);

}  // namespace san4::hd_gen
