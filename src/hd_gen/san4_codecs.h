#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace san4::codecs {

// Converts 4-bit PC-98 [Blue, Red, Green] triples (48 bytes = 16 colors) into
// 16 ARGB8888 (0xFFRRGGBB) entries indexed by GRP600C VGA slot (0..15).
std::array<std::uint32_t, 16> decode_brg48_palette(
    std::span<const std::uint8_t> brg48);

// Overlays an 8-color (24-byte [B, R, G]) sub-palette onto game colors
// [start_game_color .. start_game_color + count - 1] of `base_slot_pal`.
std::array<std::uint32_t, 16> apply_brg_subpalette(
    const std::array<std::uint32_t, 16>& base_slot_pal,
    std::span<const std::uint8_t> brg_sub, int start_game_color, int count);

// Converts a 16-entry NPK016 0x0GRB u16_le palette (32 bytes at hdr[16..47])
// into 16 ARGB8888 entries indexed by GRP600C VGA slot (0..15).
std::array<std::uint32_t, 16> decode_npk_palette(
    std::span<const std::uint8_t> raw32);

// Standard fallback palette if a game executable palette table is unavailable.
std::array<std::uint32_t, 16> default_san4_master_palette();

// LS11 archive decompression (Format A: GRAPH.S4, MAINCG.S4, TOSHICG.S4,
// HEXGRP.S4, HKGRP.S4, HUNIT.S4, HIGRP.S4).
bool is_ls11_archive(std::span<const std::uint8_t> archive);
bool decompress_ls11_archive(std::span<const std::uint8_t> archive,
                             std::vector<std::vector<std::uint8_t>>& out_records);

// KAODATA.S4 officer portrait decompression (Format B1: 340 x 64x80 3-plane ->
// game colors 8..15, mapped through kGrpSlotMap).
struct KaodataPortrait {
  int index = 0;
  int w = 64;
  int h = 80;
  std::vector<std::uint8_t> vga_slots;  // size = w * h, values in 8..15
};
bool decode_kaodata_archive(std::span<const std::uint8_t> archive,
                            std::vector<KaodataPortrait>& out_portraits);

// MONDATA.S4 montage officer parts (Format B2: 16 templates x 20 parts = 320
// parts -> 8,192 composite 64x80 portraits).
inline constexpr std::array<int, 16> kDefaultMondataEyeY = {
    20, 26, 20, 22, 26, 21, 25, 26, 23, 25, 25, 24, 21, 20, 21, 20,
};
inline constexpr std::array<int, 16> kDefaultMondataMouthY = {
    41, 46, 38, 43, 47, 40, 46, 43, 41, 45, 45, 43, 44, 41, 43, 42,
};

struct MondataPart {
  int w = 0;
  int h = 0;
  std::vector<std::uint8_t> pix3;       // 0..7 raw 3-plane pixel value
  std::vector<std::uint8_t> vga_slots;  // kGrpSlotMap[8 + pix3]
  std::vector<std::uint8_t> mask;       // 1 where pix3 != 0 (for eye/mouth OR)
};

struct MondataTemplate {
  int template_id = 0;
  int eye_y = 20;
  int mouth_y = 41;
  std::array<MondataPart, 4> tops;    // 64x36 at y=0..35
  std::array<MondataPart, 4> bots;    // 64x44 at y=36..79
  std::array<MondataPart, 8> eyes;    // 64x16 OR'd at y=eye_y..eye_y+15
  std::array<MondataPart, 4> mouths;  // 64x20 OR'd at y=mouth_y..mouth_y+19
};

bool decode_mondata_archive(
    std::span<const std::uint8_t> archive,
    const std::array<int, 16>& eye_y_table,
    const std::array<int, 16>& mouth_y_table,
    std::vector<MondataTemplate>& out_templates);

// Composites one 64x80 4-bit VGA-slot portrait from a MondataTemplate.
void composite_mondata_4bit(const MondataTemplate& tmpl, int top_idx,
                            int bot_idx, int eye_idx, int mouth_idx,
                            std::vector<std::uint8_t>& out_vga_slots);

// NPK016 / NPK16\0 decompression (Format C: 290 full-screen & event CGs).
struct NpkImage {
  std::size_t header_offset = 0;
  std::size_t bitstream_offset = 0;
  std::size_t compressed_end = 0;
  int scr_w = 0;
  int scr_h = 0;
  int w = 0;
  int h = 0;
  bool has_default_pc98_palette = false;
  std::array<std::uint32_t, 16> slot_palette{};
  std::vector<std::uint8_t> vga_slots;  // size = w * h
};

// Decompresses a raw NPK016 bitstream (starting immediately after the 48-byte
// header, matching GRP600C AH=0x19 ES:DI!) for dimensions (w, h).
bool decompress_npk_bitstream(std::span<const std::uint8_t> stream, int w,
                              int h, std::vector<std::uint8_t>& out_vga_slots,
                              std::size_t* out_bytes_consumed = nullptr);

// Scans a container file (OPENDAT.S4, ENDDAT.S4, PACKGRP*.S4, HIBACK.S4,
// LOGO.S4, REVGRP.S4, HEGRP.S4, PREGRP.S4) for all valid NPK016 / NPK16\0
// streams and decodes them.
void scan_and_decode_npk_images(std::span<const std::uint8_t> file_bytes,
                                std::vector<NpkImage>& out_images);

// Planar sprite decoders:
// 1) Plane-sequential 4-plane tile (e.g. 16x16 = 128B: P0[32B], P1[32B], P2[32B], P3[32B]).
bool decode_plane_sequential_4p(std::span<const std::uint8_t> raw, int w, int h,
                                std::vector<std::uint8_t>& out_game_colors,
                                std::vector<std::uint8_t>& out_vga_slots);

// 2) Byte-interleaved 3-plane or 4-plane bitmap (matching GRP600C AH=0x0F ES:BP):
//    per 8-pixel column: [P0, P1, P2] (3-plane) or [P0, P1, P2, P3] (4-plane).
//    When planes == 3 and p3_fill_mode == 1, game Plane 3 is filled with p3_fill_byte.
bool decode_byte_interleaved(std::span<const std::uint8_t> raw, int w, int h,
                             int planes, std::uint8_t p3_fill_mode,
                             std::uint8_t p3_fill_byte,
                             std::vector<std::uint8_t>& out_game_colors,
                             std::vector<std::uint8_t>& out_vga_slots);

// 3) 2-plane Kanji button label in GRAPH.S4 (32x16 = 128B: [P0, P1] per 8px ->
//    expanded by MAIN.EXE 0x2C5FB into 3-plane [P0, P1, P1] with Plane 3 = 0xFF).
bool decode_graph_kanji_label(std::span<const std::uint8_t> raw128,
                              std::vector<std::uint8_t>& out_game_colors,
                              std::vector<std::uint8_t>& out_vga_slots);

// 4) 1-bit MSB-first mask / glyph / wallpaper (matching GRP600C AH=0x13 ES:BP).
bool decode_mono_1bit(std::span<const std::uint8_t> raw, int w, int h,
                      std::vector<std::uint8_t>& out_bits);

// Canonical source hash for masked (color-0 transparent, rop_mode == 4) sprites
// so masked sprites match identically across ANY background terrain!
// Mixes marker 0xA5, <HH (w, h), and per-pixel (mask[i] ? (vga_slot[i] & 0x0F) : 0xFF).
std::uint64_t hash_masked_sprite_4bit(int w, int h,
                                      const std::uint8_t* vga_slots,
                                      const std::uint8_t* opaque_mask);

// Binary record format for `tiles/tile_dict.bin` (compact 144-byte entry per
// 16x16 tile so HdPackManager can dynamically composite arbitrary 16x16 cells).
struct PackedTileDictEntry {
  std::uint64_t hd_hash = 0;         // Key in hd_manifest.tsv for 64x64 HD PNG
  std::uint8_t archive_group = 0;    // 0=MAINCG, 1=TOSHICG, 2=HEXGRP, 3=HKGRP, 4=UNIT/FX
  std::uint8_t is_overlay = 0;       // 0=opaque base, 1=color-0 transparent overlay
  std::uint8_t non_zero_count = 0;   // Number of pixels with game_color != 0 (0..255 clamped)
  std::uint8_t full_zero_256 = 0;    // 1 if all 256 pixels are non-zero (non_zero_count == 256)
  std::uint8_t reserved[4]{};
  std::uint8_t packed_slots[128]{};  // 256 4-bit VGA slots (2 pixels/byte: lo=even x, hi=odd x)
  std::uint8_t zero_mask[32]{};      // 256 1-bit flags: bit (x&7) of byte (i>>3) = 1 iff game_color != 0
};
static_assert(sizeof(PackedTileDictEntry) == 176,
              "Unexpected PackedTileDictEntry size");

}  // namespace san4::codecs
