#include "hd_gen/san4_codecs.h"

#include <algorithm>
#include <cstring>

#include "grp600c_defs.h"

namespace san4::codecs {
namespace {

std::uint16_t read_u16_le(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(p[0]) |
         (static_cast<std::uint16_t>(p[1]) << 8);
}

std::uint32_t read_u32_be(const std::uint8_t* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) |
         (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) |
         static_cast<std::uint32_t>(p[3]);
}

std::uint32_t pack_argb_4bit(std::uint8_t r4, std::uint8_t g4, std::uint8_t b4) {
  const std::uint32_t r = static_cast<std::uint32_t>(r4 & 0x0Fu) * 0x11u;
  const std::uint32_t g = static_cast<std::uint32_t>(g4 & 0x0Fu) * 0x11u;
  const std::uint32_t b = static_cast<std::uint32_t>(b4 & 0x0Fu) * 0x11u;
  return 0xFF000000u | (r << 16) | (g << 8) | b;
}

class MsbBitReader {
 public:
  explicit MsbBitReader(std::span<const std::uint8_t> buf) : buf_(buf) {}

  bool read_bit(std::uint32_t& out_bit) {
    if (bit_pos_ >= buf_.size() * 8u) {
      return false;
    }
    const std::size_t byte_idx = bit_pos_ >> 3;
    const unsigned shift = 7u - static_cast<unsigned>(bit_pos_ & 7u);
    out_bit = (buf_[byte_idx] >> shift) & 1u;
    ++bit_pos_;
    return true;
  }

  bool read_varint(std::uint32_t& out_val) {
    unsigned k = 0;
    while (true) {
      std::uint32_t b = 0;
      if (!read_bit(b)) {
        return false;
      }
      if (b == 0u) {
        break;
      }
      if (++k > 24u) {
        return false;
      }
    }
    const unsigned cx = k + 1u;
    std::uint32_t di = 0;
    for (unsigned i = 0; i < cx; ++i) {
      std::uint32_t b = 0;
      if (!read_bit(b)) {
        return false;
      }
      di = (di << 1) | b;
    }
    out_val = ((1u << cx) - 2u) + di;
    return true;
  }

 private:
  std::span<const std::uint8_t> buf_;
  std::size_t bit_pos_ = 0;
};

bool decode_mondata_part(std::span<const std::uint8_t> raw, int w, int h,
                         MondataPart& out) {
  const int row_cols = (w + 7) / 8;
  const std::size_t needed =
      static_cast<std::size_t>(row_cols) * 3u * static_cast<std::size_t>(h);
  if (raw.size() < needed || w <= 0 || h <= 0) {
    return false;
  }
  out.w = w;
  out.h = h;
  const std::size_t total =
      static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  out.pix3.assign(total, 0u);
  out.vga_slots.assign(total, kGrpSlotMap[8]);
  out.mask.assign(total, 0u);

  for (int y = 0; y < h; ++y) {
    const std::uint8_t* row_ptr =
        raw.data() + static_cast<std::size_t>(y * row_cols * 3);
    for (int col = 0; col < row_cols; ++col) {
      const std::uint8_t p0 = row_ptr[col * 3 + 0];
      const std::uint8_t p1 = row_ptr[col * 3 + 1];
      const std::uint8_t p2 = row_ptr[col * 3 + 2];
      for (int bit = 0; bit < 8; ++bit) {
        const int x = col * 8 + bit;
        if (x >= w) {
          break;
        }
        const unsigned sh = static_cast<unsigned>(7 - bit);
        const std::uint8_t c3 =
            static_cast<std::uint8_t>(((p0 >> sh) & 1u) |
                                      (((p1 >> sh) & 1u) << 1) |
                                      (((p2 >> sh) & 1u) << 2));
        const std::size_t idx =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
            static_cast<std::size_t>(x);
        out.pix3[idx] = c3;
        out.vga_slots[idx] = kGrpSlotMap[static_cast<std::size_t>(8u + c3)];
        out.mask[idx] = (c3 != 0u) ? 1u : 0u;
      }
    }
  }
  return true;
}

}  // namespace

std::array<std::uint32_t, 16> decode_brg48_palette(
    std::span<const std::uint8_t> brg48) {
  std::array<std::uint32_t, 16> out{};
  out.fill(0xFF000000u);
  if (brg48.size() < 48u) {
    return out;
  }
  for (int gc = 0; gc < 16; ++gc) {
    const std::uint8_t b4 = brg48[static_cast<std::size_t>(gc) * 3u + 0u];
    const std::uint8_t r4 = brg48[static_cast<std::size_t>(gc) * 3u + 1u];
    const std::uint8_t g4 = brg48[static_cast<std::size_t>(gc) * 3u + 2u];
    out[kGrpSlotMap[static_cast<std::size_t>(gc)]] = pack_argb_4bit(r4, g4, b4);
  }
  return out;
}

std::array<std::uint32_t, 16> apply_brg_subpalette(
    const std::array<std::uint32_t, 16>& base_slot_pal,
    std::span<const std::uint8_t> brg_sub, int start_game_color, int count) {
  std::array<std::uint32_t, 16> out = base_slot_pal;
  if (start_game_color < 0 || count <= 0 || start_game_color + count > 16 ||
      brg_sub.size() < static_cast<std::size_t>(count) * 3u) {
    return out;
  }
  for (int i = 0; i < count; ++i) {
    const int gc = start_game_color + i;
    const std::uint8_t b4 = brg_sub[static_cast<std::size_t>(i) * 3u + 0u];
    const std::uint8_t r4 = brg_sub[static_cast<std::size_t>(i) * 3u + 1u];
    const std::uint8_t g4 = brg_sub[static_cast<std::size_t>(i) * 3u + 2u];
    out[kGrpSlotMap[static_cast<std::size_t>(gc)]] = pack_argb_4bit(r4, g4, b4);
  }
  return out;
}

std::array<std::uint32_t, 16> decode_npk_palette(
    std::span<const std::uint8_t> raw32) {
  std::array<std::uint32_t, 16> out{};
  out.fill(0xFF000000u);
  if (raw32.size() < 32u) {
    return out;
  }
  for (int gc = 0; gc < 16; ++gc) {
    const std::uint16_t grb =
        read_u16_le(raw32.data() + static_cast<std::size_t>(gc) * 2u);
    const auto b4 = static_cast<std::uint8_t>(grb & 0x0Fu);
    const auto r4 = static_cast<std::uint8_t>((grb >> 4) & 0x0Fu);
    const auto g4 = static_cast<std::uint8_t>((grb >> 8) & 0x0Fu);
    out[kGrpSlotMap[static_cast<std::size_t>(gc)]] = pack_argb_4bit(r4, g4, b4);
  }
  return out;
}

std::array<std::uint32_t, 16> default_san4_master_palette() {
  // Clean-room standard 16-color UI + portrait palette indexed by VGA slot.
  // Game colors: 0=black, 1=deep blue, 2=brick red, 3=ochre/purple, 4=forest,
  // 5=teal, 6=gold, 7=light gray, 8=dark ink, 9=slate blue, 10=crimson,
  // 11=copper/brown, 12=jade, 13=steel cyan, 14=warm skin/sand, 15=ivory white.
  static constexpr std::array<std::uint8_t, 48> kFallbackBrg = {
      0, 0,  0,   // 0
      7, 1,  2,   // 1
      2, 9,  2,   // 2
      6, 7,  4,   // 3
      2, 2,  7,   // 4
      7, 2,  7,   // 5
      3, 11, 8,   // 6
      9, 10, 10,  // 7
      2, 2,  2,   // 8
      8, 4,  5,   // 9
      3, 11, 3,   // 10
      4, 9,  6,   // 11
      5, 5,  8,   // 12
      9, 8,  9,   // 13
      8, 13, 10,  // 14
      13, 15, 14  // 15
  };
  return decode_brg48_palette(kFallbackBrg);
}

bool is_ls11_archive(std::span<const std::uint8_t> archive) {
  return archive.size() >= 0x0114u && archive[0] == 'L' && archive[1] == 'S' &&
         archive[2] == '1' && archive[3] == '1';
}

bool decompress_ls11_archive(
    std::span<const std::uint8_t> archive,
    std::vector<std::vector<std::uint8_t>>& out_records) {
  out_records.clear();
  if (!is_ls11_archive(archive)) {
    return false;
  }

  const std::uint8_t* dict = archive.data() + 0x0010u;
  std::size_t idx_off = 0x0110u;

  while (idx_off + 4u <= archive.size()) {
    const std::uint32_t comp_len = read_u32_be(archive.data() + idx_off);
    if (comp_len == 0u) {
      break;
    }
    if (idx_off + 12u > archive.size()) {
      return false;
    }
    const std::uint32_t uncomp_len = read_u32_be(archive.data() + idx_off + 4u);
    const std::uint32_t file_off = read_u32_be(archive.data() + idx_off + 8u);
    idx_off += 12u;

    if (uncomp_len > 16u * 1024u * 1024u ||
        static_cast<std::uint64_t>(file_off) + comp_len > archive.size()) {
      return false;
    }

    std::vector<std::uint8_t> out;
    out.reserve(uncomp_len);

    if (comp_len == uncomp_len) {
      out.assign(archive.data() + file_off,
                 archive.data() + file_off + uncomp_len);
    } else {
      MsbBitReader br(archive.subspan(file_off, comp_len));
      while (out.size() < uncomp_len) {
        std::uint32_t code = 0;
        if (!br.read_varint(code)) {
          break;
        }
        if (code < 256u) {
          out.push_back(dict[code]);
        } else {
          const std::size_t dist = static_cast<std::size_t>(code - 256u);
          std::uint32_t len_code = 0;
          if (dist == 0u || dist > out.size() || !br.read_varint(len_code)) {
            return false;
          }
          const std::size_t copy_len =
              std::min<std::size_t>(static_cast<std::size_t>(len_code) + 3u,
                                    uncomp_len - out.size());
          for (std::size_t i = 0; i < copy_len; ++i) {
            out.push_back(out[out.size() - dist]);
          }
        }
      }
      if (out.size() != uncomp_len) {
        return false;
      }
    }
    out_records.push_back(std::move(out));
  }

  return !out_records.empty();
}

bool decode_kaodata_archive(std::span<const std::uint8_t> archive,
                            std::vector<KaodataPortrait>& out_portraits) {
  out_portraits.clear();
  constexpr std::size_t kDataBase = 0x07F8u;
  if (archive.size() <= kDataBase) {
    return false;
  }

  const std::size_t entry_count = kDataBase / 6u;  // 340 portraits
  out_portraits.reserve(entry_count);

  for (std::size_t idx = 0; idx < entry_count; ++idx) {
    const std::uint8_t* ent = archive.data() + idx * 6u;
    const std::size_t rel_off = static_cast<std::size_t>(ent[0]) |
                                (static_cast<std::size_t>(ent[1]) << 8) |
                                (static_cast<std::size_t>(ent[2]) << 16);
    const std::size_t comp_len = read_u16_le(ent + 4u);
    if (comp_len < 4u) {
      continue;
    }
    const std::size_t abs_off = kDataBase + rel_off;
    if (abs_off + comp_len > archive.size()) {
      return false;
    }

    const int w = read_u16_le(archive.data() + abs_off);
    const int h = read_u16_le(archive.data() + abs_off + 2u);
    if (w <= 0 || w > 256 || h <= 0 || h > 256) {
      return false;
    }

    // Stage 1: 0xFE escape RLE expansion.
    std::vector<std::uint8_t> stage1;
    stage1.reserve(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    std::uint8_t last_byte = 0u;
    std::size_t p = abs_off + 4u;
    const std::size_t end = abs_off + comp_len;
    while (p < end) {
      const std::uint8_t b = archive[p++];
      if (b != 0xFEu) {
        stage1.push_back(b);
        last_byte = b;
      } else {
        if (p >= end) {
          break;
        }
        const std::uint8_t n = archive[p++];
        if (n == 0x01u) {
          stage1.push_back(0xFEu);
          last_byte = 0xFEu;
        } else {
          const std::size_t rep = (n == 0x00u) ? 256u : static_cast<std::size_t>(n);
          stage1.insert(stage1.end(), rep, last_byte);
        }
      }
    }

    // Stage 2: High-nibble-first 2D spatial predictor stream.
    std::size_t nib_pos = 0;
    auto read_nibble = [&]() -> std::uint8_t {
      const std::size_t byte_i = nib_pos >> 1;
      if (byte_i >= stage1.size()) {
        return 0u;
      }
      const std::uint8_t b = stage1[byte_i];
      const std::uint8_t nib =
          ((nib_pos & 1u) == 0u) ? static_cast<std::uint8_t>((b >> 4) & 0x0Fu)
                                 : static_cast<std::uint8_t>(b & 0x0Fu);
      ++nib_pos;
      return nib;
    };

    const std::size_t pix_total =
        static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<std::uint8_t> pix3(pix_total, 0u);
    for (int y = 0; y < h; ++y) {
      const std::uint8_t row_cfg = read_nibble();
      const bool copy_up = (row_cfg >= 8u);
      const int delta = copy_up ? (static_cast<int>(row_cfg) - 7)
                                : (static_cast<int>(row_cfg) + 1);
      int run_rem = 0;
      for (int x = 0; x < w; ++x) {
        const std::size_t cur =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
            static_cast<std::size_t>(x);
        if (run_rem == 0) {
          const std::uint8_t nib = read_nibble();
          if (nib < 8u) {
            pix3[cur] = static_cast<std::uint8_t>(nib & 7u);
            continue;
          } else if (nib < 12u) {
            run_rem = static_cast<int>(nib & 3u) + 2;
          } else {
            const std::uint8_t nib2 = read_nibble();
            run_rem = (static_cast<int>(nib & 3u) << 4) +
                      static_cast<int>(nib2) + 6;
          }
        }
        if (run_rem > 0) {
          std::uint8_t pred = 0u;
          if (copy_up) {
            if (y >= delta) {
              pred = pix3[static_cast<std::size_t>(y - delta) *
                              static_cast<std::size_t>(w) +
                          static_cast<std::size_t>(x)];
            }
          } else {
            if (x >= delta) {
              pred = pix3[static_cast<std::size_t>(y) *
                              static_cast<std::size_t>(w) +
                          static_cast<std::size_t>(x - delta)];
            }
          }
          pix3[cur] = pred;
          --run_rem;
        }
      }
    }

    KaodataPortrait k{};
    k.index = static_cast<int>(idx);
    k.w = w;
    k.h = h;
    k.vga_slots.resize(pix_total);
    for (std::size_t i = 0; i < pix_total; ++i) {
      k.vga_slots[i] = kGrpSlotMap[static_cast<std::size_t>(8u + (pix3[i] & 7u))];
    }
    out_portraits.push_back(std::move(k));
  }

  return !out_portraits.empty();
}

bool decode_mondata_archive(
    std::span<const std::uint8_t> archive,
    const std::array<int, 16>& eye_y_table,
    const std::array<int, 16>& mouth_y_table,
    std::vector<MondataTemplate>& out_templates) {
  out_templates.clear();
  constexpr std::size_t kTemplateBytes = 0x2E80u;  // 11,904 bytes
  if (archive.size() < 16u * kTemplateBytes) {
    return false;
  }

  out_templates.resize(16);
  for (int t = 0; t < 16; ++t) {
    const std::span<const std::uint8_t> blk =
        archive.subspan(static_cast<std::size_t>(t) * kTemplateBytes,
                        kTemplateBytes);
    MondataTemplate& mt = out_templates[static_cast<std::size_t>(t)];
    mt.template_id = t;
    mt.eye_y = std::clamp(eye_y_table[static_cast<std::size_t>(t)], 0, 64);
    mt.mouth_y = std::clamp(mouth_y_table[static_cast<std::size_t>(t)], 0, 60);

    // +0x0000: 4 tops (64x36 = 864B each)
    for (int i = 0; i < 4; ++i) {
      if (!decode_mondata_part(blk.subspan(0x0000u + static_cast<std::size_t>(i) * 864u, 864u),
                               64, 36, mt.tops[static_cast<std::size_t>(i)])) {
        return false;
      }
    }
    // +0x0D80: 4 bottoms (64x44 = 1056B each)
    for (int i = 0; i < 4; ++i) {
      if (!decode_mondata_part(blk.subspan(0x0D80u + static_cast<std::size_t>(i) * 1056u, 1056u),
                               64, 44, mt.bots[static_cast<std::size_t>(i)])) {
        return false;
      }
    }
    // +0x1E00: 8 eyes (64x16 = 384B each)
    for (int i = 0; i < 8; ++i) {
      if (!decode_mondata_part(blk.subspan(0x1E00u + static_cast<std::size_t>(i) * 384u, 384u),
                               64, 16, mt.eyes[static_cast<std::size_t>(i)])) {
        return false;
      }
    }
    // +0x2700: 4 mouths (64x20 = 480B each)
    for (int i = 0; i < 4; ++i) {
      if (!decode_mondata_part(blk.subspan(0x2700u + static_cast<std::size_t>(i) * 480u, 480u),
                               64, 20, mt.mouths[static_cast<std::size_t>(i)])) {
        return false;
      }
    }
  }

  return true;
}

void composite_mondata_4bit(const MondataTemplate& tmpl, int top_idx,
                            int bot_idx, int eye_idx, int mouth_idx,
                            std::vector<std::uint8_t>& out_vga_slots) {
  std::array<std::uint8_t, 64 * 80> pix3{};
  const auto& top = tmpl.tops[static_cast<std::size_t>(top_idx & 3)];
  const auto& bot = tmpl.bots[static_cast<std::size_t>(bot_idx & 3)];
  const auto& eye = tmpl.eyes[static_cast<std::size_t>(eye_idx & 7)];
  const auto& mouth = tmpl.mouths[static_cast<std::size_t>(mouth_idx & 3)];

  std::copy_n(top.pix3.data(), 64u * 36u, pix3.data());
  std::copy_n(bot.pix3.data(), 64u * 44u, pix3.data() + 64u * 36u);

  for (int y = 0; y < 16; ++y) {
    const int dy = tmpl.eye_y + y;
    if (dy < 0 || dy >= 80) continue;
    for (int x = 0; x < 64; ++x) {
      pix3[static_cast<std::size_t>(dy * 64 + x)] |=
          eye.pix3[static_cast<std::size_t>(y * 64 + x)];
    }
  }
  for (int y = 0; y < 20; ++y) {
    const int dy = tmpl.mouth_y + y;
    if (dy < 0 || dy >= 80) continue;
    for (int x = 0; x < 64; ++x) {
      pix3[static_cast<std::size_t>(dy * 64 + x)] |=
          mouth.pix3[static_cast<std::size_t>(y * 64 + x)];
    }
  }

  out_vga_slots.resize(64u * 80u);
  for (std::size_t i = 0; i < 64u * 80u; ++i) {
    out_vga_slots[i] = kGrpSlotMap[static_cast<std::size_t>(8u + (pix3[i] & 7u))];
  }
}

bool decompress_npk_bitstream(std::span<const std::uint8_t> stream, int w,
                              int h, std::vector<std::uint8_t>& out_vga_slots,
                              std::size_t* out_bytes_consumed) {
  if (w <= 0 || w > 800 || (w & 3) != 0 || h <= 0 || h > 600) {
    return false;
  }
  const int words_per_row = w / 4;
  constexpr int kRingStride = 200;
  constexpr int kRingSize = 1000;  // 5 rows x 200 words
  if (words_per_row > kRingStride) {
    return false;
  }

  std::array<std::uint16_t, kRingSize> ring{};
  out_vga_slots.assign(
      static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0u);

  std::size_t pos = 0;
  std::uint8_t flag_byte = 0u;
  int flag_bits_rem = 0;
  int row_base = 0;

  auto next_flag = [&](bool& cf) -> bool {
    if (flag_bits_rem == 0) {
      if (pos >= stream.size()) {
        return false;
      }
      flag_byte = stream[pos++];
      flag_bits_rem = 8;
    }
    cf = (flag_byte & 1u) != 0u;
    flag_byte >>= 1;
    --flag_bits_rem;
    return true;
  };

  for (int y = 0; y < h; ++y) {
    int col = 0;
    while (col < words_per_row) {
      bool cf = false;
      if (!next_flag(cf)) {
        return false;
      }
      if (!cf) {
        if (pos + 2u > stream.size()) {
          return false;
        }
        const std::uint16_t lit = read_u16_le(stream.data() + pos);
        pos += 2u;
        ring[static_cast<std::size_t>((row_base + col) % kRingSize)] = lit;
        ++col;
      } else {
        if (pos >= stream.size()) {
          return false;
        }
        const std::uint8_t m = stream[pos++];
        const int cx = static_cast<int>(m & 0x1Fu) + 1;
        const int step = static_cast<int>((m >> 5) & 0x03u) + 1;
        const int back = ((m & 0x80u) != 0u) ? (step * kRingStride) : step;
        for (int i = 0; i < cx && col < words_per_row; ++i) {
          int dst_idx = (row_base + col) % kRingSize;
          int src_idx = dst_idx - back;
          if (src_idx < 0) {
            src_idx += kRingSize;
          }
          ring[static_cast<std::size_t>(dst_idx)] =
              ring[static_cast<std::size_t>(src_idx)];
          ++col;
        }
      }
    }

    // Unpack 4 pixels per word (`b0 = lo`, `b1 = hi`)
    std::uint8_t* dst_row =
        out_vga_slots.data() +
        static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
    for (int c = 0; c < words_per_row; ++c) {
      const std::uint16_t word =
          ring[static_cast<std::size_t>((row_base + c) % kRingSize)];
      const std::uint8_t b0 = static_cast<std::uint8_t>(word & 0xFFu);
      const std::uint8_t b1 = static_cast<std::uint8_t>((word >> 8) & 0xFFu);
      for (int k = 0; k < 4; ++k) {
        const unsigned s = static_cast<unsigned>(3 - k);
        const std::uint8_t p0 = (b1 >> s) & 1u;
        const std::uint8_t p1 = (b1 >> (4u + s)) & 1u;
        const std::uint8_t p2 = (b0 >> s) & 1u;
        const std::uint8_t p3 = (b0 >> (4u + s)) & 1u;
        const std::uint8_t gc =
            static_cast<std::uint8_t>(p0 | (p1 << 1) | (p2 << 2) | (p3 << 3));
        dst_row[c * 4 + k] = kGrpSlotMap[gc];
      }
    }

    row_base = (row_base + kRingStride) % kRingSize;
  }

  if (out_bytes_consumed != nullptr) {
    *out_bytes_consumed = pos;
  }
  return true;
}

void scan_and_decode_npk_images(std::span<const std::uint8_t> file_bytes,
                                std::vector<NpkImage>& out_images) {
  out_images.clear();
  if (file_bytes.size() < 48u) {
    return;
  }

  std::size_t i = 0;
  while (i + 48u <= file_bytes.size()) {
    const std::uint8_t* p = file_bytes.data() + i;
    const bool magic_ok =
        (std::memcmp(p, "NPK016", 6) == 0) ||
        (std::memcmp(p, "NPK16\0", 6) == 0);
    if (!magic_ok) {
      ++i;
      continue;
    }

    const int planes = read_u16_le(p + 6u);
    const int scr_w = read_u16_le(p + 8u);
    const int scr_h = read_u16_le(p + 10u);
    const int img_w = read_u16_le(p + 12u);
    const int img_h = read_u16_le(p + 14u);
    if (planes != 4 || img_w <= 0 || img_w > 800 || (img_w & 3) != 0 ||
        img_h <= 0 || img_h > 600) {
      ++i;
      continue;
    }

    NpkImage img{};
    img.header_offset = i;
    img.bitstream_offset = i + 48u;
    img.scr_w = scr_w;
    img.scr_h = scr_h;
    img.w = img_w;
    img.h = img_h;
    static constexpr std::array<std::uint16_t, 16> kDefaultPc98Grb = {
        0x000u, 0x00Fu, 0x0F0u, 0x0FFu, 0xF00u, 0xF0Fu, 0xFF0u, 0xFFFu,
        0x777u, 0x00Au, 0x0A0u, 0x0AAu, 0xA00u, 0xA0Au, 0xAA0u, 0xAAAu,
    };
    bool is_def_pc98 = true;
    for (std::size_t c = 0; c < 16u; ++c) {
      if (read_u16_le(p + 16u + c * 2u) != kDefaultPc98Grb[c]) {
        is_def_pc98 = false;
        break;
      }
    }
    img.has_default_pc98_palette = is_def_pc98;
    img.slot_palette = decode_npk_palette(file_bytes.subspan(i + 16u, 32u));

    std::size_t consumed = 0;
    if (decompress_npk_bitstream(file_bytes.subspan(i + 48u), img_w, img_h,
                                 img.vga_slots, &consumed) &&
        consumed > 0u) {
      img.compressed_end = i + 48u + consumed;
      out_images.push_back(std::move(img));
      i += 48u + consumed;
    } else {
      ++i;
    }
  }
}

bool decode_plane_sequential_4p(std::span<const std::uint8_t> raw, int w, int h,
                                std::vector<std::uint8_t>& out_game_colors,
                                std::vector<std::uint8_t>& out_vga_slots) {
  if (w <= 0 || (w & 7) != 0 || h <= 0) {
    return false;
  }
  const std::size_t plane_bytes =
      static_cast<std::size_t>(w / 8) * static_cast<std::size_t>(h);
  if (raw.size() < plane_bytes * 4u) {
    return false;
  }

  const std::size_t total =
      static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  out_game_colors.resize(total);
  out_vga_slots.resize(total);

  const std::uint8_t* p0 = raw.data() + plane_bytes * 0u;
  const std::uint8_t* p1 = raw.data() + plane_bytes * 1u;
  const std::uint8_t* p2 = raw.data() + plane_bytes * 2u;
  const std::uint8_t* p3 = raw.data() + plane_bytes * 3u;

  for (std::size_t bi = 0; bi < plane_bytes; ++bi) {
    const std::uint8_t b0 = p0[bi];
    const std::uint8_t b1 = p1[bi];
    const std::uint8_t b2 = p2[bi];
    const std::uint8_t b3 = p3[bi];
    for (int bit = 0; bit < 8; ++bit) {
      const unsigned sh = static_cast<unsigned>(7 - bit);
      const std::uint8_t gc = static_cast<std::uint8_t>(
          ((b0 >> sh) & 1u) | (((b1 >> sh) & 1u) << 1) |
          (((b2 >> sh) & 1u) << 2) | (((b3 >> sh) & 1u) << 3));
      out_game_colors[bi * 8u + static_cast<std::size_t>(bit)] = gc;
      out_vga_slots[bi * 8u + static_cast<std::size_t>(bit)] = kGrpSlotMap[gc];
    }
  }
  return true;
}

bool decode_byte_interleaved(std::span<const std::uint8_t> raw, int w, int h,
                             int planes, std::uint8_t p3_fill_mode,
                             std::uint8_t p3_fill_byte,
                             std::vector<std::uint8_t>& out_game_colors,
                             std::vector<std::uint8_t>& out_vga_slots) {
  if (w <= 0 || h <= 0 || (planes != 3 && planes != 4)) {
    return false;
  }
  const int cols = (w + 7) / 8;
  const std::size_t row_bytes =
      static_cast<std::size_t>(cols) * static_cast<std::size_t>(planes);
  const std::size_t needed = row_bytes * static_cast<std::size_t>(h);
  if (raw.size() < needed) {
    return false;
  }

  const std::size_t total =
      static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  out_game_colors.resize(total);
  out_vga_slots.resize(total);

  const std::uint8_t p3_const = (p3_fill_mode == 1u) ? p3_fill_byte : 0x00u;

  for (int y = 0; y < h; ++y) {
    const std::uint8_t* rp = raw.data() + static_cast<std::size_t>(y) * row_bytes;
    for (int c = 0; c < cols; ++c) {
      const std::uint8_t b0 = rp[c * planes + 0];
      const std::uint8_t b1 = rp[c * planes + 1];
      const std::uint8_t b2 = rp[c * planes + 2];
      const std::uint8_t b3 = (planes == 4) ? rp[c * planes + 3] : p3_const;
      for (int bit = 0; bit < 8; ++bit) {
        const int x = c * 8 + bit;
        if (x >= w) {
          break;
        }
        const unsigned sh = static_cast<unsigned>(7 - bit);
        const std::uint8_t gc = static_cast<std::uint8_t>(
            ((b0 >> sh) & 1u) | (((b1 >> sh) & 1u) << 1) |
            (((b2 >> sh) & 1u) << 2) | (((b3 >> sh) & 1u) << 3));
        const std::size_t idx =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
            static_cast<std::size_t>(x);
        out_game_colors[idx] = gc;
        out_vga_slots[idx] = kGrpSlotMap[gc];
      }
    }
  }
  return true;
}

bool decode_graph_kanji_label(std::span<const std::uint8_t> raw128,
                              std::vector<std::uint8_t>& out_game_colors,
                              std::vector<std::uint8_t>& out_vga_slots) {
  if (raw128.size() < 128u) {
    return false;
  }
  // 32x16 = 4 cols/row * 16 rows * 2 bytes/col = 128 bytes.
  // MAIN.EXE (0x2C5FB) expands [b0, b1] -> 3-plane [b0, b1, b1] with P3 = 0xFF!
  out_game_colors.resize(32u * 16u);
  out_vga_slots.resize(32u * 16u);
  for (int y = 0; y < 16; ++y) {
    const std::uint8_t* rp = raw128.data() + static_cast<std::size_t>(y) * 8u;
    for (int c = 0; c < 4; ++c) {
      const std::uint8_t b0 = rp[c * 2 + 0];
      const std::uint8_t b1 = rp[c * 2 + 1];
      for (int bit = 0; bit < 8; ++bit) {
        const unsigned sh = static_cast<unsigned>(7 - bit);
        const std::uint8_t p0 = (b0 >> sh) & 1u;
        const std::uint8_t p1 = (b1 >> sh) & 1u;
        const std::uint8_t gc =
            static_cast<std::uint8_t>(8u | p0 | (p1 << 1) | (p1 << 2));
        const std::size_t idx =
            static_cast<std::size_t>(y * 32 + c * 8 + bit);
        out_game_colors[idx] = gc;
        out_vga_slots[idx] = kGrpSlotMap[gc];
      }
    }
  }
  return true;
}

bool decode_mono_1bit(std::span<const std::uint8_t> raw, int w, int h,
                      std::vector<std::uint8_t>& out_bits) {
  if (w <= 0 || h <= 0) {
    return false;
  }
  const int row_bytes = (w + 7) / 8;
  const std::size_t needed =
      static_cast<std::size_t>(row_bytes) * static_cast<std::size_t>(h);
  if (raw.size() < needed) {
    return false;
  }
  out_bits.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
  for (int y = 0; y < h; ++y) {
    const std::uint8_t* rp =
        raw.data() + static_cast<std::size_t>(y * row_bytes);
    for (int x = 0; x < w; ++x) {
      const std::uint8_t b = rp[x >> 3];
      out_bits[static_cast<std::size_t>(y * w + x)] =
          static_cast<std::uint8_t>((b >> (7 - (x & 7))) & 1u);
    }
  }
  return true;
}

std::uint64_t hash_masked_sprite_4bit(int w, int h,
                                      const std::uint8_t* vga_slots,
                                      const std::uint8_t* opaque_mask) {
  std::uint64_t hval = 14695981039346656037ull;
  auto mix = [&](std::uint8_t b) {
    hval ^= static_cast<std::uint64_t>(b);
    hval *= 1099511628211ull;
  };
  mix(0xA5u);  // Domain separator from opaque hash_sprite_4bit
  mix(static_cast<std::uint8_t>(w & 0xFF));
  mix(static_cast<std::uint8_t>((w >> 8) & 0xFF));
  mix(static_cast<std::uint8_t>(h & 0xFF));
  mix(static_cast<std::uint8_t>((h >> 8) & 0xFF));
  const std::size_t n =
      static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  for (std::size_t i = 0; i < n; ++i) {
    if (opaque_mask != nullptr && opaque_mask[i] == 0u) {
      mix(0xFFu);
    } else {
      mix(static_cast<std::uint8_t>(vga_slots[i] & 0x0Fu));
    }
  }
  return hval;
}

}  // namespace san4::codecs
