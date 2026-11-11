#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "hd_gen/hd_art_upscaler.h"

namespace san4::test_ref {

// Original per-call std::pow sRGB transfer functions.
float srgb8_to_linear_reference(std::uint8_t c8);
std::uint8_t linear_to_srgb8_reference(float lin);

// Original (pre-sRGB-LUT) implementation of hd_gen::upscale_art_4x_argb.
void upscale_art_4x_argb_reference(
    const std::uint8_t* vga_slots, const std::uint8_t* opaque_mask, int w,
    int h, const std::array<std::uint32_t, 16>& slot_palette,
    hd_gen::ArtAssetKind kind, std::vector<std::uint32_t>& out_argb_4x);

}  // namespace san4::test_ref
