#pragma once

#include <array>
#include <cstdint>
#include <span>

typedef struct x86emu_s x86emu_t;

namespace san4 {

class VesaPlanarVideo {
 public:
  static constexpr int kWidth = 800;
  static constexpr int kHeight = 600;
  static constexpr int kBytesPerLine = kWidth / 8;  // 100
  static constexpr int kVisibleBytes = kBytesPerLine * kHeight;  // 60000
  static constexpr int kPlaneSize = 65536;
  static constexpr int kTileSize = 16;
  static constexpr int kTilesX = kWidth / kTileSize;  // 50
  static constexpr int kTilesY = (kHeight + kTileSize - 1) / kTileSize;  // 38
  static constexpr int kTileCount = kTilesX * kTilesY;  // 1900

  VesaPlanarVideo();

  void reset_mode(std::uint16_t mode);
  std::uint16_t current_mode() const { return current_mode_; }
  int active_width() const { return (current_mode_ == 0x0012u) ? 640 : kWidth; }
  int active_height() const {
    return (current_mode_ == 0x0012u) ? 480 : kHeight;
  }
  int active_bytes_per_line() const { return active_width() / 8; }
  std::uint32_t active_visible_bytes() const {
    return static_cast<std::uint32_t>(active_bytes_per_line() * active_height());
  }
  int latched_width() const { return latched_w_; }
  int latched_height() const { return latched_h_; }

  // Byte-granular planar VRAM MMIO for 0xA0000..0xAFFFF.
  std::uint8_t vram_read8(std::uint32_t offset);
  void vram_write8(std::uint32_t offset, std::uint8_t val);

  // Byte-granular VGA I/O port handlers (0x3C0..0x3DF).
  bool handles_port(std::uint16_t port) const;
  std::uint8_t port_in8(std::uint16_t port);
  void port_out8(std::uint16_t port, std::uint8_t val);

  // INT 10h / VESA 4F00h/4F01h/4F02h/4F03h BIOS handler.
  // For AX=1130h BH=06h, sets ES:BP to rom_font_seg:rom_font_off.
  void handle_int10(std::uint16_t& ax, std::uint16_t& bx, std::uint16_t& cx,
                    std::uint16_t& dx, std::uint16_t& es, std::uint16_t& bp,
                    std::uint16_t di, std::span<std::uint8_t> guest_ram,
                    std::uint16_t rom_font_seg, std::uint16_t rom_font_off,
                    std::uint16_t rom_vbe_modes_off = 0x0500u,
                    std::uint16_t rom_vbe_oem_off = 0x0510u,
                    x86emu_t* emu = nullptr);

  // Glyph crispness mask management (called on GRP600C AH=0x0F/0x13/0x04/0x19).
  void mark_glyph_rect(int x, int y, int w, int h);
  void clear_glyph_rect(int x, int y, int w, int h);
  void clear_all_glyphs();
  void mark_all_dirty();

  // Smooth 24-bit host crossfade for GRP600C CS:0x2B35.
  void begin_smooth_fade(std::uint8_t start_idx, std::uint8_t count,
                         const std::uint8_t* src_rgb_4bit,
                         const std::uint8_t* dst_rgb_4bit,
                         const std::uint8_t* dac_slot_map,
                         double duration_sec);
  // Advances active smooth fade by dt_sec; returns true while fade is active.
  bool advance_fade(double dt_sec);
  bool is_fading() const { return fade_active_; }
  void finish_fade_immediately();

  // Sync-point frame latch: copies live shadow_vram_ & palette into latched
  // front state for tear-free upscaling.
  void latch_frame();

  // Accessors for Upscaler, HdPackManager, and tests.
  const std::array<std::uint8_t, kWidth * kHeight>& shadow_vram() const {
    return shadow_vram_;
  }
  const std::array<std::uint8_t, kWidth * kHeight>& latched_vram() const {
    return latched_vram_;
  }
  const std::array<std::uint32_t, 16>& live_palette_argb() const {
    return live_palette_argb_;
  }
  const std::array<std::uint32_t, 16>& latched_palette_argb() const {
    return latched_palette_argb_;
  }
  // Returns the target non-black 16-slot ARGB palette for dynamic-palette CGs:
  // during fade-in from black, returns the destination palette so dynamic CGs
  // upscale against their true scene palette before the first visible frame.
  std::array<std::uint32_t, 16> reference_nonblack_palette_argb() const;
  const std::array<std::uint8_t, kTileCount>& live_glyph_tiles() const {
    return live_glyph_tiles_;
  }
  const std::array<std::uint8_t, kTileCount>& latched_glyph_tiles() const {
    return latched_glyph_tiles_;
  }
  const std::array<std::uint64_t, kTileCount>& latched_vram_tile_gen()
      const noexcept {
    return latched_vram_tile_gen_;
  }
  const std::array<std::uint64_t, kTileCount>& latched_glyph_tile_gen()
      const noexcept {
    return latched_glyph_tile_gen_;
  }
  std::uint64_t latched_glyph_generation() const noexcept {
    return latched_glyph_gen_;
  }
  std::uint32_t palette_generation() const { return latched_palette_gen_; }

  // Consumes accumulated dirty tiles since the last upscale pass.
  bool consume_dirty_tiles(std::array<std::uint8_t, kTileCount>& out_dirty,
                           bool& out_palette_changed);
  // Schedules the non-zero entries of `tiles` for the next upscale pass
  // without touching VRAM or the latch (used for HD-overlay-only changes).
  void mark_upscale_tiles_dirty(const std::array<std::uint8_t, kTileCount>& tiles);

  // Activity counters for quiescent burst scheduling and VSync sync points.
  std::uint64_t consume_vram_activity() {
    std::uint64_t v = vram_activity_;
    vram_activity_ = 0;
    return v;
  }
  bool consume_vsync_edge() {
    bool v = vsync_edge_;
    vsync_edge_ = false;
    return v;
  }

  // Hardware-exact per-pixel write bitset (CPU thread only) for HD canvas occlusion.
  // Bit (y * kWidth + x) is set whenever vram_write8 modifies any plane of pixel (x, y),
  // even when overwriting with the identical 4-bit color index.
  static constexpr std::size_t kOccludeWords = (kWidth * kHeight) / 64;
  bool has_hd_occlude_bits() const noexcept { return hd_occlude_any_; }
  const std::array<std::uint64_t, kOccludeWords>& hd_occlude_bits() const noexcept {
    return hd_occlude_bits_;
  }
  void clear_hd_occlude_bits() const noexcept {
    if (hd_occlude_any_) {
      hd_occlude_bits_.fill(0u);
      hd_occlude_any_ = false;
    }
  }

  // Clean-room 8x16 VGA ROM font (256 glyphs * 16 bytes = 4096 bytes).
  static std::span<const std::uint8_t> builtin_font_8x16();

 private:
  void update_shadow_byte(std::uint32_t offset);
  void rebuild_live_palette();
  static std::uint8_t expand_6bit(std::uint8_t v6) {
    v6 &= 0x3Fu;
    return static_cast<std::uint8_t>((v6 << 2) | (v6 >> 4));
  }
  static std::uint8_t expand_4bit(std::uint8_t v4) {
    v4 &= 0x0Fu;
    return static_cast<std::uint8_t>((v4 << 4) | v4);
  }

  std::uint16_t current_mode_ = 0x0003;
  std::array<std::array<std::uint8_t, kPlaneSize>, 4> planes_{};
  std::array<std::uint8_t, 4> latches_{};

  std::uint8_t seq_index_ = 0;
  std::array<std::uint8_t, 8> seq_regs_{};

  std::uint8_t gc_index_ = 0;
  std::array<std::uint8_t, 16> gc_regs_{};

  bool attr_flipflop_data_ = false;
  std::uint8_t attr_index_ = 0;
  std::array<std::uint8_t, 32> attr_regs_{};

  std::uint8_t dac_write_index_ = 0;
  std::uint8_t dac_write_comp_ = 0;
  std::array<std::uint8_t, 2> dac_pending_rg_{};
  std::uint8_t dac_read_index_ = 0;
  std::uint8_t dac_read_comp_ = 0;
  std::uint8_t pel_mask_ = 0xFF;
  std::array<std::array<std::uint8_t, 3>, 256> dac_6bit_{};

  // Two-phase 0x3DA status poll state.
  std::uint32_t status_polls_ = 0;
  bool in_vretrace_ = false;
  bool vsync_edge_ = false;

  std::array<std::uint8_t, kWidth * kHeight> shadow_vram_{};
  std::array<std::uint8_t, kWidth * kHeight> latched_vram_{};
  std::array<std::uint8_t, kTileCount> live_dirty_tiles_{};
  std::array<std::uint8_t, kTileCount> upscale_dirty_tiles_{};
  std::array<std::uint8_t, kTileCount> live_glyph_tiles_{};
  std::array<std::uint8_t, kTileCount> latched_glyph_tiles_{};
  std::uint64_t live_vram_gen_ = 0;
  std::array<std::uint64_t, kTileCount> latched_vram_tile_gen_{};
  std::uint64_t live_glyph_gen_ = 0;
  std::uint64_t latched_glyph_gen_ = 0;
  std::array<std::uint64_t, kTileCount> live_glyph_tile_gen_{};
  std::array<std::uint64_t, kTileCount> latched_glyph_tile_gen_{};

  std::array<std::uint32_t, 16> live_palette_argb_{};
  std::array<std::uint32_t, 16> latched_palette_argb_{};
  std::uint32_t live_palette_gen_ = 1;
  std::uint32_t latched_palette_gen_ = 0;
  int latched_w_ = kWidth;
  int latched_h_ = kHeight;
  bool upscale_palette_changed_ = true;

  // Smooth 24-bit crossfade state.
  bool fade_active_ = false;
  double fade_elapsed_ = 0.0;
  double fade_duration_ = 0.0;
  std::uint8_t fade_start_idx_ = 0;
  std::uint8_t fade_count_ = 0;
  std::array<std::array<float, 3>, 16> fade_src_rgb_{};
  std::array<std::array<float, 3>, 16> fade_dst_rgb_{};
  std::array<std::uint8_t, 16> fade_dac_slots_{};
  std::array<std::array<std::uint8_t, 3>, 256> fade_dac_8bit_{};

  mutable std::array<std::uint64_t, kOccludeWords> hd_occlude_bits_{};
  mutable bool hd_occlude_any_ = false;

  std::uint64_t vram_activity_ = 0;
};

}  // namespace san4
