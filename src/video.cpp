#include "video.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "x86emu_clean.h"

namespace san4 {
namespace {

alignas(16) constexpr std::uint8_t kBuiltinFont8x16[4096] = {
#include "vga_font_8x16.inc"
};

constexpr std::uint8_t kDefaultAttrPalette[16] = {
    0, 1, 2, 3, 4, 5, 20, 7, 56, 57, 58, 59, 60, 61, 62, 63,
};

constexpr std::uint8_t rotate_right8(std::uint8_t v, unsigned count) {
  count &= 7u;
  if (count == 0u) {
    return v;
  }
  return static_cast<std::uint8_t>((v >> count) | (v << (8u - count)));
}

constexpr std::uint8_t apply_vga_alu(std::uint8_t src, std::uint8_t latch,
                                     unsigned op) {
  switch (op & 3u) {
    case 1u:
      return static_cast<std::uint8_t>(src & latch);
    case 2u:
      return static_cast<std::uint8_t>(src | latch);
    case 3u:
      return static_cast<std::uint8_t>(src ^ latch);
    default:
      return src;
  }
}

constexpr std::array<std::uint8_t, 256> make_bit_reverse8() {
  std::array<std::uint8_t, 256> tbl{};
  for (int i = 0; i < 256; ++i) {
    std::uint8_t v = static_cast<std::uint8_t>(i);
    std::uint8_t r = 0u;
    for (int b = 0; b < 8; ++b) {
      r = static_cast<std::uint8_t>((r << 1) | ((v >> b) & 1u));
    }
    tbl[static_cast<std::size_t>(i)] = r;
  }
  return tbl;
}

constexpr auto kBitReverse8 = make_bit_reverse8();

}  // namespace

VesaPlanarVideo::VesaPlanarVideo() {
  reset_mode(0x0102);
  latch_frame();
}

void VesaPlanarVideo::reset_mode(std::uint16_t mode) {
  current_mode_ = mode;
  for (auto& plane : planes_) {
    plane.fill(0);
  }
  latches_.fill(0);

  seq_index_ = 0;
  seq_regs_.fill(0);
  seq_regs_[2] = 0x0Fu;  // Map Mask: all 4 planes enabled

  gc_index_ = 0;
  gc_regs_.fill(0);
  gc_regs_[7] = 0x0Fu;  // Color Don't Care
  gc_regs_[8] = 0xFFu;  // Bit Mask

  attr_flipflop_data_ = false;
  attr_index_ = 0;
  attr_regs_.fill(0);
  for (int i = 0; i < 16; ++i) {
    attr_regs_[i] = kDefaultAttrPalette[i];
  }

  dac_write_index_ = 0u;
  dac_write_comp_ = 0u;
  dac_pending_rg_ = {0u, 0u};
  dac_read_index_ = 0u;
  dac_read_comp_ = 0u;
  pel_mask_ = 0xFFu;
  for (auto& rgb : dac_6bit_) {
    rgb = {0u, 0u, 0u};
  }

  // Standard 64-entry EGA/VGA DAC default cube in 0..63
  for (int i = 0; i < 64; ++i) {
    dac_6bit_[i][0] = static_cast<std::uint8_t>(
        (((i >> 2) & 1) * 42) + (((i >> 5) & 1) * 21));
    dac_6bit_[i][1] = static_cast<std::uint8_t>(
        (((i >> 1) & 1) * 42) + (((i >> 4) & 1) * 21));
    dac_6bit_[i][2] = static_cast<std::uint8_t>(
        (((i >> 0) & 1) * 42) + (((i >> 3) & 1) * 21));
  }

  shadow_vram_.fill(0);
  live_glyph_tiles_.fill(0);
  hd_occlude_bits_.fill(0u);
  hd_occlude_any_ = false;
  fade_active_ = false;
  rebuild_live_palette();
  ++live_palette_gen_;
  mark_all_dirty();
  ++vram_activity_;
}

std::uint8_t VesaPlanarVideo::vram_read8(std::uint32_t offset) {
  offset &= 0xFFFFu;
  for (int p = 0; p < 4; ++p) {
    latches_[p] = planes_[p][offset];
  }

  const unsigned read_mode = (gc_regs_[5] >> 3) & 1u;
  if (read_mode == 0u) {
    return latches_[gc_regs_[4] & 3u];
  }

  // Read Mode 1: Color Compare across planes enabled by Color Don't Care.
  std::uint8_t match = 0xFFu;
  const std::uint8_t color_cmp = gc_regs_[2];
  const std::uint8_t color_dont_care = gc_regs_[7];
  for (int p = 0; p < 4; ++p) {
    if ((color_dont_care >> p) & 1u) {
      const std::uint8_t target = ((color_cmp >> p) & 1u) ? 0xFFu : 0x00u;
      match &= static_cast<std::uint8_t>(~(latches_[p] ^ target));
    }
  }
  return match;
}

void VesaPlanarVideo::vram_write8(std::uint32_t offset, std::uint8_t val) {
  offset &= 0xFFFFu;
  const std::uint8_t map_mask = seq_regs_[2] & 0x0Fu;
  if (map_mask == 0u) {
    return;
  }

  const unsigned write_mode = gc_regs_[5] & 3u;
  const unsigned rot_count = gc_regs_[3] & 7u;
  const unsigned alu_op = (gc_regs_[3] >> 3) & 3u;
  const std::uint8_t set_reset = gc_regs_[0];
  const std::uint8_t enable_sr = gc_regs_[1];
  const std::uint8_t bit_mask = gc_regs_[8];

  std::uint8_t eff_pixel_mask = bit_mask;
  switch (write_mode) {
    case 0u: {
      const std::uint8_t rot = rotate_right8(val, rot_count);
      for (int p = 0; p < 4; ++p) {
        if (!((map_mask >> p) & 1u)) {
          continue;
        }
        const std::uint8_t src =
            ((enable_sr >> p) & 1u)
                ? (((set_reset >> p) & 1u) ? 0xFFu : 0x00u)
                : rot;
        const std::uint8_t alu = apply_vga_alu(src, latches_[p], alu_op);
        planes_[p][offset] = static_cast<std::uint8_t>(
            (alu & bit_mask) | (latches_[p] & static_cast<std::uint8_t>(~bit_mask)));
      }
      break;
    }
    case 1u: {
      eff_pixel_mask = 0xFFu;
      for (int p = 0; p < 4; ++p) {
        if ((map_mask >> p) & 1u) {
          planes_[p][offset] = latches_[p];
        }
      }
      break;
    }
    case 2u: {
      for (int p = 0; p < 4; ++p) {
        if (!((map_mask >> p) & 1u)) {
          continue;
        }
        const std::uint8_t src = ((val >> p) & 1u) ? 0xFFu : 0x00u;
        const std::uint8_t alu = apply_vga_alu(src, latches_[p], alu_op);
        planes_[p][offset] = static_cast<std::uint8_t>(
            (alu & bit_mask) | (latches_[p] & static_cast<std::uint8_t>(~bit_mask)));
      }
      break;
    }
    case 3u: {
      const std::uint8_t rot = rotate_right8(val, rot_count);
      const std::uint8_t eff_mask = static_cast<std::uint8_t>(rot & bit_mask);
      eff_pixel_mask = eff_mask;
      for (int p = 0; p < 4; ++p) {
        if (!((map_mask >> p) & 1u)) {
          continue;
        }
        const std::uint8_t sr = ((set_reset >> p) & 1u) ? 0xFFu : 0x00u;
        const std::uint8_t alu = apply_vga_alu(sr, latches_[p], alu_op);
        planes_[p][offset] = static_cast<std::uint8_t>(
            (alu & eff_mask) | (latches_[p] & static_cast<std::uint8_t>(~eff_mask)));
      }
      break;
    }
  }

  ++vram_activity_;
  if (offset < active_visible_bytes()) {
    update_shadow_byte(offset);
    if (eff_pixel_mask != 0u) {
      const int bpl = active_bytes_per_line();
      const int y = static_cast<int>(offset / static_cast<std::uint32_t>(bpl));
      const int x_byte = static_cast<int>(offset % static_cast<std::uint32_t>(bpl));
      if (y >= 0 && y < kHeight && x_byte * 8 + 8 <= kWidth) {
        const std::size_t px0 = static_cast<std::size_t>(y * kWidth + x_byte * 8);
        hd_occlude_bits_[px0 >> 6] |=
            static_cast<std::uint64_t>(kBitReverse8[eff_pixel_mask]) << (px0 & 56u);
        hd_occlude_any_ = true;
      }
    }
  }
}

void VesaPlanarVideo::update_shadow_byte(std::uint32_t offset) {
  const std::uint8_t p0 = planes_[0][offset];
  const std::uint8_t p1 = planes_[1][offset];
  const std::uint8_t p2 = planes_[2][offset];
  const std::uint8_t p3 = planes_[3][offset];

  const int bpl = active_bytes_per_line();
  const int y = static_cast<int>(offset / static_cast<std::uint32_t>(bpl));
  const int x_byte = static_cast<int>(offset % static_cast<std::uint32_t>(bpl));
  if (y < 0 || y >= kHeight || x_byte * 8 + 8 > kWidth) {
    return;
  }

  std::uint8_t* dst = shadow_vram_.data() + y * kWidth + x_byte * 8;
  for (int bit = 0; bit < 8; ++bit) {
    const unsigned shift = 7u - static_cast<unsigned>(bit);
    dst[bit] = static_cast<std::uint8_t>(
        (((p0 >> shift) & 1u) << 0) | (((p1 >> shift) & 1u) << 1) |
        (((p2 >> shift) & 1u) << 2) | (((p3 >> shift) & 1u) << 3));
  }

  const int x_tile = x_byte >> 1;  // 8px * 2 = 16px
  const int y_tile = y >> 4;
  live_dirty_tiles_[y_tile * kTilesX + x_tile] = 1u;
}

bool VesaPlanarVideo::handles_port(std::uint16_t port) const {
  return (port >= 0x03C0u && port <= 0x03DFu) || port == 0x03BAu;
}

std::uint8_t VesaPlanarVideo::port_in8(std::uint16_t port) {
  switch (port) {
    case 0x03C0u:
      return attr_index_;
    case 0x03C1u:
      return attr_regs_[attr_index_ & 0x1Fu];
    case 0x03C4u:
      return seq_index_;
    case 0x03C5u:
      return seq_regs_[seq_index_ & 7u];
    case 0x03C6u:
      return pel_mask_;
    case 0x03C8u:
      return dac_write_index_;
    case 0x03C9u: {
      const std::uint8_t v = dac_6bit_[dac_read_index_][dac_read_comp_];
      if (++dac_read_comp_ >= 3u) {
        dac_read_comp_ = 0u;
        ++dac_read_index_;
      }
      return v;
    }
    case 0x03CEu:
      return gc_index_;
    case 0x03CFu:
      return gc_regs_[gc_index_ & 0x0Fu];
    case 0x03BAu:
    case 0x03DAu: {
      // Reading Input Status Register 1 resets the 0x3C0 flip-flop and cycles
      // non-retrace -> retrace smoothly so guest wait_vsync loops progress.
      attr_flipflop_data_ = false;
      ++status_polls_;
      const bool prev_vret = in_vretrace_;
      in_vretrace_ = ((status_polls_ & 0x03u) == 0u);
      if (!prev_vret && in_vretrace_) {
        vsync_edge_ = true;
      }
      const std::uint8_t hret = (status_polls_ & 1u) ? 0x01u : 0x00u;
      const std::uint8_t vret = in_vretrace_ ? 0x08u : 0x00u;
      return static_cast<std::uint8_t>(hret | vret);
    }
    default:
      return 0x00u;
  }
}

void VesaPlanarVideo::port_out8(std::uint16_t port, std::uint8_t val) {
  switch (port) {
    case 0x03C0u:
      if (!attr_flipflop_data_) {
        attr_index_ = val & 0x1Fu;
        attr_flipflop_data_ = true;
      } else {
        const std::uint8_t idx = attr_index_ & 0x1Fu;
        if (idx < 16u && fade_active_) {
          finish_fade_immediately();
        }
        attr_regs_[idx] = val;
        attr_flipflop_data_ = false;
        if (idx < 16u) {
          rebuild_live_palette();
        }
      }
      break;
    case 0x03C4u:
      seq_index_ = val & 7u;
      break;
    case 0x03C5u:
      seq_regs_[seq_index_ & 7u] = val;
      break;
    case 0x03C6u:
      if (pel_mask_ != val) {
        if (fade_active_) {
          finish_fade_immediately();
        }
        pel_mask_ = val;
        rebuild_live_palette();
      }
      break;
    case 0x03C7u:
      dac_read_index_ = val;
      dac_read_comp_ = 0u;
      break;
    case 0x03C8u:
      dac_write_index_ = val;
      dac_write_comp_ = 0u;
      break;
    case 0x03C9u: {
      const std::uint8_t c6 = val & 0x3Fu;
      if (dac_write_comp_ < 2u) {
        dac_pending_rg_[dac_write_comp_] = c6;
        dac_6bit_[dac_write_index_][dac_write_comp_] = c6;
        ++dac_write_comp_;
      } else {
        const std::uint8_t reg = dac_write_index_;
        dac_write_comp_ = 0u;
        ++dac_write_index_;
        if (fade_active_) {
          finish_fade_immediately();
        }
        dac_6bit_[reg][0] = dac_pending_rg_[0];
        dac_6bit_[reg][1] = dac_pending_rg_[1];
        dac_6bit_[reg][2] = c6;
        rebuild_live_palette();
      }
      break;
    }
    case 0x03CEu:
      gc_index_ = val & 0x0Fu;
      break;
    case 0x03CFu:
      gc_regs_[gc_index_ & 0x0Fu] = val;
      break;
    default:
      break;
  }
}

void VesaPlanarVideo::handle_int10(std::uint16_t& ax, std::uint16_t& bx,
                                   std::uint16_t& cx, std::uint16_t& dx,
                                   std::uint16_t& es, std::uint16_t& bp,
                                   std::uint16_t di,
                                   std::span<std::uint8_t> guest_ram,
                                   std::uint16_t rom_font_seg,
                                   std::uint16_t rom_font_off,
                                   std::uint16_t rom_vbe_modes_off,
                                   std::uint16_t rom_vbe_oem_off,
                                   x86emu_t* emu) {
  auto write_guest_block = [&](std::size_t dst, const std::uint8_t* src,
                               std::size_t len) {
    if (len == 0u || dst >= guest_ram.size()) {
      return;
    }
    const std::size_t avail = std::min<std::size_t>(len, guest_ram.size() - dst);
    if (dst >= 0x00400u && (dst + avail) <= 0xA0000u) {
      std::memcpy(guest_ram.data() + dst, src, avail);
      return;
    }
    if (emu != nullptr) {
      for (std::size_t i = 0u; i < avail; ++i) {
        x86emu_write_byte(emu, static_cast<std::uint32_t>(dst + i), src[i]);
      }
      return;
    }
    for (std::size_t i = 0u; i < avail; ++i) {
      const std::size_t a = dst + i;
      if (a >= 0xF0000u && a < 0x100000u) {
        continue;
      }
      if (a >= 0xA0000u && a < 0xB0000u) {
        vram_write8(static_cast<std::uint32_t>(a - 0xA0000u), src[i]);
      } else {
        guest_ram[a] = src[i];
      }
    }
  };

  auto read_guest_block = [&](std::size_t src, std::uint8_t* dst,
                              std::size_t len) {
    if (len == 0u) {
      return;
    }
    if (src >= guest_ram.size()) {
      std::memset(dst, 0xFF, len);
      return;
    }
    const std::size_t avail = std::min<std::size_t>(len, guest_ram.size() - src);
    if (src >= 0x00400u && (src + avail) <= 0xA0000u) {
      std::memcpy(dst, guest_ram.data() + src, avail);
    } else if (emu != nullptr) {
      for (std::size_t i = 0u; i < avail; ++i) {
        dst[i] = x86emu_read_byte(emu, static_cast<std::uint32_t>(src + i));
      }
    } else {
      for (std::size_t i = 0u; i < avail; ++i) {
        const std::size_t a = src + i;
        if (a >= 0xA0000u && a < 0xB0000u) {
          dst[i] = vram_read8(static_cast<std::uint32_t>(a - 0xA0000u));
        } else {
          dst[i] = guest_ram[a];
        }
      }
    }
    if (avail < len) {
      std::memset(dst + avail, 0xFF, len - avail);
    }
  };

  auto put_le16 = [](std::uint8_t* buf, std::size_t off, std::uint16_t v) {
    buf[off + 0u] = static_cast<std::uint8_t>(v & 0xFFu);
    buf[off + 1u] = static_cast<std::uint8_t>(v >> 8);
  };

  if (ax == 0x4F00u) {
    // VESA VBE 1.2 Return Controller Information at ES:DI (256 bytes)
    const std::size_t dst = (static_cast<std::size_t>(es) << 4) + di;
    if (dst + 256u <= guest_ram.size()) {
      std::array<std::uint8_t, 256> blk{};
      blk[0x00u] = 'V';
      blk[0x01u] = 'E';
      blk[0x02u] = 'S';
      blk[0x03u] = 'A';
      put_le16(blk.data(), 0x04u, 0x0102u);  // VBE 1.2
      put_le16(blk.data(), 0x06u, rom_vbe_oem_off);
      put_le16(blk.data(), 0x08u, rom_font_seg);
      put_le16(blk.data(), 0x0Eu, rom_vbe_modes_off);
      put_le16(blk.data(), 0x10u, rom_font_seg);
      put_le16(blk.data(), 0x12u, 4u);  // 4 x 64KB = 256KB VRAM
      write_guest_block(dst, blk.data(), blk.size());
      ax = 0x004Fu;
    } else {
      ax = 0x014Fu;
    }
    return;
  }
  if (ax == 0x4F01u) {
    // VESA VBE 1.2 Return Mode Information for CX at ES:DI (256 bytes)
    const std::uint16_t mode = static_cast<std::uint16_t>(cx & 0x3FFFu);
    const std::size_t dst = (static_cast<std::size_t>(es) << 4) + di;
    if ((mode == 0x0102u || mode == 0x006Au) &&
        dst + 256u <= guest_ram.size()) {
      std::array<std::uint8_t, 256> blk{};
      put_le16(blk.data(), 0x00u, 0x001Bu);  // Supported + opt + color + gfx
      blk[0x02u] = 0x07u;                    // WinA supported/readable/writable
      put_le16(blk.data(), 0x04u, 64u);
      put_le16(blk.data(), 0x06u, 64u);
      put_le16(blk.data(), 0x08u, 0xA000u);
      put_le16(blk.data(), 0x10u, 100u);  // 800 / 8 = 100 bytes per scanline
      put_le16(blk.data(), 0x12u, 800u);
      put_le16(blk.data(), 0x14u, 600u);
      blk[0x16u] = 8u;
      blk[0x17u] = 16u;
      blk[0x18u] = 4u;  // 4 planes
      blk[0x19u] = 4u;  // 4 bpp
      blk[0x1Au] = 1u;
      blk[0x1Bu] = 0x03u;  // Planar memory model
      blk[0x1Eu] = 1u;
      write_guest_block(dst, blk.data(), blk.size());
      ax = 0x004Fu;
    } else {
      ax = 0x014Fu;
    }
    return;
  }
  if (ax == 0x4F02u) {
    reset_mode(static_cast<std::uint16_t>(bx & 0x7FFFu));
    ax = 0x004Fu;
    return;
  }
  if (ax == 0x4F03u) {
    bx = current_mode_;
    ax = 0x004Fu;
    return;
  }
  if (ax == 0x1130u) {
    es = rom_font_seg;
    bp = rom_font_off;
    cx = 16u;
    dx = static_cast<std::uint16_t>((dx & 0xFF00u) | 36u);
    return;
  }

  const std::uint8_t ah = static_cast<std::uint8_t>(ax >> 8);
  const std::uint8_t al = static_cast<std::uint8_t>(ax & 0xFFu);

  switch (ah) {
    case 0x00u:
      reset_mode(al);
      break;
    case 0x0Fu:
      ax = static_cast<std::uint16_t>((80u << 8) | (current_mode_ & 0xFFu));
      bx &= 0x00FFu;
      break;
    case 0x10u: {
      if (al == 0x00u) {
        if (fade_active_) {
          finish_fade_immediately();
        }
        const std::uint8_t idx = static_cast<std::uint8_t>(bx & 0x1Fu);
        const std::uint8_t val = static_cast<std::uint8_t>((bx >> 8) & 0x3Fu);
        attr_regs_[idx] = val;
        if (idx < 16u) {
          rebuild_live_palette();
        }
      } else if (al == 0x01u) {
        attr_regs_[0x11u] = static_cast<std::uint8_t>(bx >> 8);
      } else if (al == 0x02u) {
        const std::size_t lin = (static_cast<std::size_t>(es) << 4) + dx;
        if (lin + 17u <= guest_ram.size()) {
          if (fade_active_) {
            finish_fade_immediately();
          }
          std::array<std::uint8_t, 17> regs{};
          read_guest_block(lin, regs.data(), regs.size());
          for (int i = 0; i < 16; ++i) {
            attr_regs_[i] = regs[static_cast<std::size_t>(i)] & 0x3Fu;
          }
          attr_regs_[0x11u] = regs[16u];
          rebuild_live_palette();
        }
      } else if (al == 0x10u) {
        if (fade_active_) {
          finish_fade_immediately();
        }
        const std::uint8_t reg = static_cast<std::uint8_t>(bx & 0xFFu);
        dac_6bit_[reg][0] = static_cast<std::uint8_t>((dx >> 8) & 0x3Fu);
        dac_6bit_[reg][1] = static_cast<std::uint8_t>((cx >> 8) & 0x3Fu);
        dac_6bit_[reg][2] = static_cast<std::uint8_t>(cx & 0x3Fu);
        rebuild_live_palette();
      } else if (al == 0x12u) {
        if (fade_active_) {
          finish_fade_immediately();
        }
        const std::size_t lin = (static_cast<std::size_t>(es) << 4) + dx;
        for (std::uint16_t i = 0; i < cx; ++i) {
          const std::size_t p = lin + static_cast<std::size_t>(i) * 3u;
          if (p + 3u > guest_ram.size()) {
            break;
          }
          std::uint8_t rgb[3]{};
          read_guest_block(p, rgb, 3u);
          const std::uint8_t reg = static_cast<std::uint8_t>((bx + i) & 0xFFu);
          dac_6bit_[reg][0] = rgb[0] & 0x3Fu;
          dac_6bit_[reg][1] = rgb[1] & 0x3Fu;
          dac_6bit_[reg][2] = rgb[2] & 0x3Fu;
        }
        rebuild_live_palette();
      } else if (al == 0x07u) {
        const std::uint8_t idx = static_cast<std::uint8_t>(bx & 0x1Fu);
        bx = static_cast<std::uint16_t>(
            (bx & 0x00FFu) |
            (static_cast<std::uint16_t>(attr_regs_[idx]) << 8));
      } else if (al == 0x08u) {
        bx = static_cast<std::uint16_t>(
            (bx & 0x00FFu) |
            (static_cast<std::uint16_t>(attr_regs_[0x11u]) << 8));
      } else if (al == 0x09u) {
        const std::size_t lin = (static_cast<std::size_t>(es) << 4) + dx;
        if (lin + 17u <= guest_ram.size()) {
          std::array<std::uint8_t, 17> regs{};
          for (int i = 0; i < 16; ++i) {
            regs[static_cast<std::size_t>(i)] = attr_regs_[i];
          }
          regs[16u] = attr_regs_[0x11u];
          write_guest_block(lin, regs.data(), regs.size());
        }
      } else if (al == 0x15u) {
        const std::uint8_t reg = static_cast<std::uint8_t>(bx & 0xFFu);
        dx = static_cast<std::uint16_t>(
            (dx & 0x00FFu) |
            (static_cast<std::uint16_t>(dac_6bit_[reg][0]) << 8));
        cx = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(dac_6bit_[reg][1]) << 8) |
            dac_6bit_[reg][2]);
      } else if (al == 0x17u) {
        const std::size_t lin = (static_cast<std::size_t>(es) << 4) + dx;
        for (std::uint16_t i = 0; i < cx; ++i) {
          const std::size_t p = lin + static_cast<std::size_t>(i) * 3u;
          if (p + 3u > guest_ram.size()) {
            break;
          }
          const std::uint8_t reg = static_cast<std::uint8_t>((bx + i) & 0xFFu);
          write_guest_block(p, dac_6bit_[reg].data(), 3u);
        }
      }
      break;
    }
    default:
      break;
  }
}

void VesaPlanarVideo::rebuild_live_palette() {
  std::array<std::uint32_t, 16> next{};
  for (int i = 0; i < 16; ++i) {
    const std::uint8_t dac_idx = attr_regs_[i] & pel_mask_;
    const std::uint8_t r8 = expand_6bit(dac_6bit_[dac_idx][0]);
    const std::uint8_t g8 = expand_6bit(dac_6bit_[dac_idx][1]);
    const std::uint8_t b8 = expand_6bit(dac_6bit_[dac_idx][2]);
    next[i] = 0xFF000000u | (static_cast<std::uint32_t>(r8) << 16) |
              (static_cast<std::uint32_t>(g8) << 8) |
              static_cast<std::uint32_t>(b8);
  }
  if (next != live_palette_argb_) {
    live_palette_argb_ = next;
    ++live_palette_gen_;
  }
}

void VesaPlanarVideo::mark_glyph_rect(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) {
    return;
  }
  const std::int64_t x0 = std::max<std::int64_t>(0, x);
  const std::int64_t y0 = std::max<std::int64_t>(0, y);
  const std::int64_t x1 =
      std::min<std::int64_t>(kWidth, static_cast<std::int64_t>(x) + w);
  const std::int64_t y1 =
      std::min<std::int64_t>(kHeight, static_cast<std::int64_t>(y) + h);
  if (x0 >= x1 || y0 >= y1) {
    return;
  }
  const int tx0 = static_cast<int>(x0) >> 4;
  const int ty0 = static_cast<int>(y0) >> 4;
  const int tx1 = static_cast<int>(x1 - 1) >> 4;
  const int ty1 = static_cast<int>(y1 - 1) >> 4;
  for (int ty = ty0; ty <= ty1; ++ty) {
    for (int tx = tx0; tx <= tx1; ++tx) {
      live_glyph_tiles_[ty * kTilesX + tx] = 1u;
    }
  }
}

void VesaPlanarVideo::clear_glyph_rect(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) {
    return;
  }
  const std::int64_t x0 = std::max<std::int64_t>(0, x);
  const std::int64_t y0 = std::max<std::int64_t>(0, y);
  const std::int64_t x1 =
      std::min<std::int64_t>(kWidth, static_cast<std::int64_t>(x) + w);
  const std::int64_t y1 =
      std::min<std::int64_t>(kHeight, static_cast<std::int64_t>(y) + h);
  if (x0 >= x1 || y0 >= y1) {
    return;
  }
  const int tx0 = static_cast<int>(x0) >> 4;
  const int ty0 = static_cast<int>(y0) >> 4;
  const int tx1 = static_cast<int>(x1 - 1) >> 4;
  const int ty1 = static_cast<int>(y1 - 1) >> 4;
  for (int ty = ty0; ty <= ty1; ++ty) {
    for (int tx = tx0; tx <= tx1; ++tx) {
      live_glyph_tiles_[ty * kTilesX + tx] = 0u;
    }
  }
}

void VesaPlanarVideo::clear_all_glyphs() { live_glyph_tiles_.fill(0u); }

void VesaPlanarVideo::mark_all_dirty() { live_dirty_tiles_.fill(1u); }

void VesaPlanarVideo::begin_smooth_fade(std::uint8_t start_idx,
                                        std::uint8_t count,
                                        const std::uint8_t* src_rgb_4bit,
                                        const std::uint8_t* dst_rgb_4bit,
                                        const std::uint8_t* dac_slot_map,
                                        double duration_sec) {
  if (dst_rgb_4bit == nullptr || start_idx >= 16u || count == 0u) {
    return;
  }
  if (start_idx + count > 16u) {
    count = static_cast<std::uint8_t>(16u - start_idx);
  }
  if (fade_active_) {
    finish_fade_immediately();
  }

  // Ensure the pre-fade VRAM state is latched cleanly before holding mainline.
  latch_frame();

  fade_start_idx_ = start_idx;
  fade_count_ = count;
  fade_elapsed_ = 0.0;
  fade_duration_ = std::max(0.0, duration_sec);

  // When an explicit DAC slot map is supplied (as in GRP600C, which programs
  // identity attr_regs_[0..15] = 0..15 via AX=1002h at mode init), ensure any
  // targeted slot < 16 still holding power-on EGA defaults (20, 56..63) maps
  // cleanly to that DAC slot so advance_fade() and finish_fade_immediately()
  // agree 100% even if AX=1002h was omitted in a synthetic test.
  for (int i = 0; i < 16; ++i) {
    if (dac_slot_map != nullptr) {
      const std::uint8_t slot = dac_slot_map[i];
      fade_dac_slots_[i] = slot;
      if (i >= start_idx && i < static_cast<int>(start_idx) + count &&
          slot < 16u && attr_regs_[slot] == kDefaultAttrPalette[slot]) {
        attr_regs_[slot] = slot;
      }
    } else {
      fade_dac_slots_[i] = static_cast<std::uint8_t>(attr_regs_[i] & pel_mask_);
    }
  }

  for (int c = 0; c < 256; ++c) {
    fade_dac_8bit_[c][0] = expand_6bit(dac_6bit_[c][0]);
    fade_dac_8bit_[c][1] = expand_6bit(dac_6bit_[c][1]);
    fade_dac_8bit_[c][2] = expand_6bit(dac_6bit_[c][2]);
  }

  for (int i = 0; i < 16; ++i) {
    const std::uint8_t slot = fade_dac_slots_[i];
    fade_src_rgb_[i] = {
        static_cast<float>(fade_dac_8bit_[slot][0]),
        static_cast<float>(fade_dac_8bit_[slot][1]),
        static_cast<float>(fade_dac_8bit_[slot][2]),
    };
    fade_dst_rgb_[i] = fade_src_rgb_[i];
  }

  // Note: GRP600C stores 4-bit palette triples in NEC PC-98 [Blue, Red, Green]
  // byte order (CS:0x2B00/0x2AD5 writes byte[1]->R, byte[2]->G, byte[0]->B).
  for (std::uint8_t k = 0; k < count; ++k) {
    const std::uint8_t idx = static_cast<std::uint8_t>(start_idx + k);
    if (src_rgb_4bit != nullptr) {
      fade_src_rgb_[idx][0] =
          static_cast<float>(expand_4bit(src_rgb_4bit[idx * 3 + 1]));
      fade_src_rgb_[idx][1] =
          static_cast<float>(expand_4bit(src_rgb_4bit[idx * 3 + 2]));
      fade_src_rgb_[idx][2] =
          static_cast<float>(expand_4bit(src_rgb_4bit[idx * 3 + 0]));
    }
    fade_dst_rgb_[idx][0] =
        static_cast<float>(expand_4bit(dst_rgb_4bit[k * 3 + 1]));
    fade_dst_rgb_[idx][1] =
        static_cast<float>(expand_4bit(dst_rgb_4bit[k * 3 + 2]));
    fade_dst_rgb_[idx][2] =
        static_cast<float>(expand_4bit(dst_rgb_4bit[k * 3 + 0]));
  }

  fade_active_ = true;
  if (fade_duration_ <= 1e-6) {
    finish_fade_immediately();
  } else {
    advance_fade(0.0);
  }
}

bool VesaPlanarVideo::advance_fade(double dt_sec) {
  if (!fade_active_) {
    return false;
  }
  fade_elapsed_ += std::max(0.0, dt_sec);
  const float t = (fade_duration_ <= 1e-6)
                      ? 1.0f
                      : static_cast<float>(
                            std::clamp(fade_elapsed_ / fade_duration_, 0.0, 1.0));

  const auto lerp_u8 = [t](float a, float b) -> std::uint8_t {
    const float v = a + (b - a) * t;
    return static_cast<std::uint8_t>(
        std::clamp(static_cast<int>(std::lround(v)), 0, 255));
  };

  for (std::uint8_t k = 0; k < fade_count_; ++k) {
    const std::uint8_t idx = static_cast<std::uint8_t>(fade_start_idx_ + k);
    const std::uint8_t slot = fade_dac_slots_[idx];
    fade_dac_8bit_[slot][0] =
        lerp_u8(fade_src_rgb_[idx][0], fade_dst_rgb_[idx][0]);
    fade_dac_8bit_[slot][1] =
        lerp_u8(fade_src_rgb_[idx][1], fade_dst_rgb_[idx][1]);
    fade_dac_8bit_[slot][2] =
        lerp_u8(fade_src_rgb_[idx][2], fade_dst_rgb_[idx][2]);
  }

  for (int p = 0; p < 16; ++p) {
    const std::uint8_t dac_idx = attr_regs_[p] & pel_mask_;
    const std::uint32_t r = fade_dac_8bit_[dac_idx][0];
    const std::uint32_t g = fade_dac_8bit_[dac_idx][1];
    const std::uint32_t b = fade_dac_8bit_[dac_idx][2];
    live_palette_argb_[p] = 0xFF000000u | (r << 16) | (g << 8) | b;
  }
  ++live_palette_gen_;

  if (fade_elapsed_ >= fade_duration_) {
    finish_fade_immediately();
    return false;
  }
  return true;
}

void VesaPlanarVideo::finish_fade_immediately() {
  if (!fade_active_) {
    return;
  }
  for (std::uint8_t k = 0; k < fade_count_; ++k) {
    const std::uint8_t idx = static_cast<std::uint8_t>(fade_start_idx_ + k);
    const std::uint8_t slot = fade_dac_slots_[idx];
    dac_6bit_[slot][0] =
        static_cast<std::uint8_t>(static_cast<int>(fade_dst_rgb_[idx][0]) >> 2);
    dac_6bit_[slot][1] =
        static_cast<std::uint8_t>(static_cast<int>(fade_dst_rgb_[idx][1]) >> 2);
    dac_6bit_[slot][2] =
        static_cast<std::uint8_t>(static_cast<int>(fade_dst_rgb_[idx][2]) >> 2);
  }
  fade_active_ = false;
  rebuild_live_palette();
}

std::array<std::uint32_t, 16>
VesaPlanarVideo::reference_nonblack_palette_argb() const {
  if (!fade_active_) {
    return latched_palette_argb_;
  }

  auto build_endpoint_pal =
      [this](const std::array<std::array<float, 3>, 16>& ep,
             int& out_max_chan) -> std::array<std::uint32_t, 16> {
    std::array<std::array<std::uint8_t, 3>, 256> dac = fade_dac_8bit_;
    for (std::uint8_t k = 0; k < fade_count_; ++k) {
      const std::uint8_t idx = static_cast<std::uint8_t>(fade_start_idx_ + k);
      const std::uint8_t slot = fade_dac_slots_[idx];
      dac[slot][0] = static_cast<std::uint8_t>(
          std::clamp(static_cast<int>(std::lround(ep[idx][0])), 0, 255));
      dac[slot][1] = static_cast<std::uint8_t>(
          std::clamp(static_cast<int>(std::lround(ep[idx][1])), 0, 255));
      dac[slot][2] = static_cast<std::uint8_t>(
          std::clamp(static_cast<int>(std::lround(ep[idx][2])), 0, 255));
    }
    std::array<std::uint32_t, 16> pal{};
    int max_c = 0;
    for (int p = 0; p < 16; ++p) {
      const std::uint8_t dac_idx = attr_regs_[p] & pel_mask_;
      const int r = dac[dac_idx][0];
      const int g = dac[dac_idx][1];
      const int b = dac[dac_idx][2];
      max_c = std::max({max_c, r, g, b});
      pal[p] = 0xFF000000u | (static_cast<std::uint32_t>(r) << 16) |
               (static_cast<std::uint32_t>(g) << 8) |
               static_cast<std::uint32_t>(b);
    }
    out_max_chan = max_c;
    return pal;
  };

  int max_src = 0;
  int max_dst = 0;
  const auto src_pal = build_endpoint_pal(fade_src_rgb_, max_src);
  const auto dst_pal = build_endpoint_pal(fade_dst_rgb_, max_dst);
  if (max_src <= 16 && max_dst > 16) {
    return dst_pal;
  }
  return src_pal;
}

void VesaPlanarVideo::latch_frame() {
  // While holding mainline execution inside CS:0x2B35 smooth crossfade, keep
  // latched_vram_ frozen and update only the 24-bit interpolated palette.
  if (!fade_active_) {
    const int cur_w = active_width();
    const int cur_h = active_height();
    if (latched_w_ != cur_w || latched_h_ != cur_h) {
      latched_w_ = cur_w;
      latched_h_ = cur_h;
      upscale_palette_changed_ = true;
      upscale_dirty_tiles_.fill(1u);
    }
    for (int ty = 0; ty < kTilesY; ++ty) {
      for (int tx = 0; tx < kTilesX; ++tx) {
        const int tidx = ty * kTilesX + tx;
        if (!live_dirty_tiles_[tidx]) {
          continue;
        }
        live_dirty_tiles_[tidx] = 0u;
        upscale_dirty_tiles_[tidx] = 1u;

        const int x0 = tx * kTileSize;
        const int y0 = ty * kTileSize;
        const int rows = std::min(kTileSize, kHeight - y0);
        for (int r = 0; r < rows; ++r) {
          const int off = (y0 + r) * kWidth + x0;
          std::memcpy(latched_vram_.data() + off, shadow_vram_.data() + off,
                      kTileSize);
        }
      }
    }
    latched_glyph_tiles_ = live_glyph_tiles_;
  }

  if (latched_palette_gen_ != live_palette_gen_) {
    latched_palette_argb_ = live_palette_argb_;
    latched_palette_gen_ = live_palette_gen_;
    upscale_palette_changed_ = true;
    upscale_dirty_tiles_.fill(1u);
  }
}

bool VesaPlanarVideo::consume_dirty_tiles(
    std::array<std::uint8_t, kTileCount>& out_dirty, bool& out_palette_changed) {
  out_palette_changed = upscale_palette_changed_;
  upscale_palette_changed_ = false;

  bool any = out_palette_changed;
  for (int i = 0; i < kTileCount; ++i) {
    out_dirty[i] = upscale_dirty_tiles_[i];
    if (upscale_dirty_tiles_[i]) {
      any = true;
      upscale_dirty_tiles_[i] = 0u;
    }
  }
  return any;
}

void VesaPlanarVideo::mark_upscale_tiles_dirty(
    const std::array<std::uint8_t, kTileCount>& tiles) {
  for (int i = 0; i < kTileCount; ++i) {
    upscale_dirty_tiles_[i] |= tiles[i];
  }
}

std::span<const std::uint8_t> VesaPlanarVideo::builtin_font_8x16() {
  return kBuiltinFont8x16;
}

}  // namespace san4
