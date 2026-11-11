#include "hd_pack.h"

#include <png.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#include "grp600c_defs.h"
#include "hd_gen/hd_art_upscaler.h"
#include "util/simd_blend.h"
#include "util/string_util.h"
#include "util/thread_pool.h"

namespace san4 {
namespace {

struct PngImageGuard {
  png_image img{};

  PngImageGuard() { img.version = PNG_IMAGE_VERSION; }
  ~PngImageGuard() { png_image_free(&img); }

  PngImageGuard(const PngImageGuard&) = delete;
  PngImageGuard& operator=(const PngImageGuard&) = delete;
};

std::string format_sprite_filename(int w, int h, std::uint64_t hash) {
  std::ostringstream oss;
  oss << "img_" << w << "x" << h << "_" << std::hex << std::setw(16)
      << std::setfill('0') << hash << ".png";
  return oss.str();
}

inline std::uint32_t blend_straight_over(std::uint32_t dst,
                                         std::uint32_t src) noexcept {
  const std::uint32_t sa = (src >> 24) & 0xFFu;
  if (sa == 0u) {
    return dst;
  }
  if (sa == 255u) {
    return src;
  }
  const std::uint32_t da = (dst >> 24) & 0xFFu;
  if (da == 0u) {
    return src;
  }
  const std::uint32_t sr = (src >> 16) & 0xFFu;
  const std::uint32_t sg = (src >> 8) & 0xFFu;
  const std::uint32_t sb = src & 0xFFu;
  const std::uint32_t dr = (dst >> 16) & 0xFFu;
  const std::uint32_t dg = (dst >> 8) & 0xFFu;
  const std::uint32_t db = dst & 0xFFu;
  const std::uint32_t inv_sa = 255u - sa;

  if (da == 255u) {
    const std::uint32_t r = (sr * sa + dr * inv_sa + 127u) / 255u;
    const std::uint32_t g = (sg * sa + dg * inv_sa + 127u) / 255u;
    const std::uint32_t b = (sb * sa + db * inv_sa + 127u) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
  }

  const std::uint32_t out_a255 = sa * 255u + da * inv_sa;
  if (out_a255 == 0u) {
    return 0u;
  }
  const std::uint32_t out_a = (out_a255 + 127u) / 255u;
  const std::uint32_t w_s = sa * 255u;
  const std::uint32_t w_d = da * inv_sa;
  const std::uint32_t half = out_a255 / 2u;
  const std::uint32_t r = (sr * w_s + dr * w_d + half) / out_a255;
  const std::uint32_t g = (sg * w_s + dg * w_d + half) / out_a255;
  const std::uint32_t b = (sb * w_s + db * w_d + half) / out_a255;
  return (out_a << 24) | (r << 16) | (g << 8) | b;
}

inline std::uint32_t sample_lut_trilinear(
    const std::array<std::uint32_t, 17 * 17 * 17>& grid,
    std::uint32_t rgb) noexcept {
  const int r = static_cast<int>((rgb >> 16) & 0xFFu);
  const int g = static_cast<int>((rgb >> 8) & 0xFFu);
  const int b = static_cast<int>(rgb & 0xFFu);

  const int ri = (r * 16) / 255;
  const int gi = (g * 16) / 255;
  const int bi = (b * 16) / 255;
  const int r1 = std::min(ri + 1, 16);
  const int g1 = std::min(gi + 1, 16);
  const int b1 = std::min(bi + 1, 16);

  // Fractional weights in 0..255
  const int rf = r * 16 - ri * 255;
  const int gf = g * 16 - gi * 255;
  const int bf = b * 16 - bi * 255;

  auto idx = [](int ir, int ig, int ib) noexcept -> std::size_t {
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

  auto interp_ch = [&](int shift) noexcept -> std::uint32_t {
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

// simd::GradeFn adapter; `ctx` points at a LUT grid.
std::uint32_t grade_with_grid(const void* ctx, std::uint32_t rgb) {
  return sample_lut_trilinear(
      *static_cast<const std::array<std::uint32_t, 17 * 17 * 17>*>(ctx), rgb);
}

std::uint64_t hash_palette_array(
    const std::array<std::uint32_t, 16>& pal) noexcept {
  std::uint64_t h = 14695981039346656037ull;
  for (std::uint32_t c : pal) {
    const std::uint32_t rgb = c & 0x00FFFFFFu;
    h ^= (rgb & 0xFFu);
    h *= 1099511628211ull;
    h ^= ((rgb >> 8) & 0xFFu);
    h *= 1099511628211ull;
    h ^= ((rgb >> 16) & 0xFFu);
    h *= 1099511628211ull;
  }
  return h;
}

constexpr std::uint64_t kMondataPartKeyPrefix = 0x4D4F4E4400000000ull;
constexpr std::uint64_t kButtonPartKeyPrefix = 0x42544E5000000000ull;

constexpr std::uint16_t kAllSlotsMask = 0xFFFFu;

// VGA slots of the upper-bank game colors 8..15 (portraits and buttons).
constexpr std::uint16_t upper_bank_slot_mask() {
  std::uint16_t m = 0u;
  for (std::size_t gc = 8u; gc < 16u; ++gc) {
    m = static_cast<std::uint16_t>(m | (1u << (kGrpSlotMap[gc] & 0x0Fu)));
  }
  return m;
}
constexpr std::uint16_t kUpperBankSlotMask = upper_bank_slot_mask();

// Output rows per compositor band; bands always cover whole logical rows.
constexpr int kCompositeBandOutputRows = 16;

// Bit set of the VGA slots in `slots` (only where `opaque_mask` is non-zero,
// when given).
std::uint16_t slot_mask_of(std::span<const std::uint8_t> slots,
                           const std::uint8_t* opaque_mask = nullptr) {
  std::uint16_t m = 0u;
  for (std::size_t i = 0; i < slots.size(); ++i) {
    if (opaque_mask == nullptr || opaque_mask[i] != 0u) {
      m = static_cast<std::uint16_t>(m | (1u << (slots[i] & 0x0Fu)));
    }
  }
  return m;
}

}  // namespace

struct HdPackManager::AsyncCgQueue {
  struct Result {
    std::uint64_t dyn_key = 0;
    std::uint32_t region_id = 0;
    // False when the job threw; `tex` is then empty.
    bool ok = false;
    std::array<std::uint32_t, 16> target_pal{};
    HdTexture tex;
  };
  std::mutex mu;
  std::vector<Result> done;  // guarded by mu
  std::atomic<std::size_t> ready{0};
};

HdPackManager::HdPackManager()
    : hd_canvas_argb_(static_cast<std::size_t>(kCanvasW) * kCanvasH, 0u),
      hd_ref_pal_id_(
          static_cast<std::size_t>(VesaPlanarVideo::kWidth) *
              VesaPlanarVideo::kHeight,
          kDirectUngradedPalId),
      hd_bank_hint_(
          static_cast<std::size_t>(VesaPlanarVideo::kWidth) *
              VesaPlanarVideo::kHeight,
          0u),
      hd_snap_slot_(
          static_cast<std::size_t>(VesaPlanarVideo::kWidth) *
              VesaPlanarVideo::kHeight,
          0u),
      hd_region_id_(
          static_cast<std::size_t>(VesaPlanarVideo::kWidth) *
              VesaPlanarVideo::kHeight,
          0u) {
  async_queue_ = std::make_shared<AsyncCgQueue>();
  region_refcounts_.push_back(0u);  // slot 0 reserved for "no HD region"
  region_used_mask_.push_back(0u);
}

HdPackManager::~HdPackManager() = default;

std::size_t HdPackManager::async_results_ready() const noexcept {
  return async_queue_->ready.load(std::memory_order_acquire);
}

void HdPackManager::upscale_cg_4x(const std::uint8_t* vga_slots, int w, int h,
                                  const std::array<std::uint32_t, 16>& palette,
                                  std::vector<std::uint32_t>& out_argb) {
  hd_gen::upscale_art_4x_argb(vga_slots, nullptr, w, h, palette,
                              hd_gen::ArtAssetKind::CgIllustration, out_argb);
}

bool HdPackManager::parse_sprite_filename(std::string_view filename, int& out_w,
                                          int& out_h, std::uint64_t& out_hash) {
  if (filename.size() <= 8u || !filename.starts_with("img_") ||
      !filename.ends_with(".png")) {
    return false;
  }
  const std::string_view body = filename.substr(4u, filename.size() - 8u);
  const auto x_pos = body.find('x');
  const auto u_pos = body.find('_');
  if (x_pos == std::string_view::npos || u_pos == std::string_view::npos ||
      x_pos == 0u || u_pos <= x_pos + 1u ||
      body.find('_', u_pos + 1u) != std::string_view::npos) {
    return false;
  }

  int w = 0;
  int h = 0;
  if (!util::parse_int_strict(body.substr(0u, x_pos), 1, kMaxPngDim, w) ||
      !util::parse_int_strict(body.substr(x_pos + 1u, u_pos - x_pos - 1u), 1,
                              kMaxPngDim, h)) {
    return false;
  }

  const std::string_view hex_part = body.substr(u_pos + 1u);
  if (hex_part.empty() || hex_part.size() > 16u) {
    return false;
  }
  std::uint64_t hash = 0;
  if (!util::parse_u64_hex_strict(hex_part, hash)) {
    return false;
  }

  out_w = w;
  out_h = h;
  out_hash = hash;
  return true;
}

std::uint64_t HdPackManager::hash_sprite_4bit(int w, int h,
                                              const std::uint8_t* indices_4bit) {
  std::uint64_t hval = 14695981039346656037ull;
  auto mix = [&](std::uint8_t b) {
    hval ^= static_cast<std::uint64_t>(b);
    hval *= 1099511628211ull;
  };
  mix(static_cast<std::uint8_t>(w & 0xFF));
  mix(static_cast<std::uint8_t>((w >> 8) & 0xFF));
  mix(static_cast<std::uint8_t>(h & 0xFF));
  mix(static_cast<std::uint8_t>((h >> 8) & 0xFF));
  const std::size_t n =
      static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  for (std::size_t i = 0; i < n; ++i) {
    mix(static_cast<std::uint8_t>(indices_4bit[i] & 0x0Fu));
  }
  return hval;
}

bool HdPackManager::save_argb_png(const std::filesystem::path& path, int w,
                                  int h, const std::uint32_t* argb) {
  if (w < 1 || w > kMaxPngDim || h < 1 || h > kMaxPngDim || argb == nullptr) {
    return false;
  }
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
  }

  const auto row_stride =
      static_cast<png_int_32>(static_cast<std::int64_t>(w) *
                              static_cast<std::int64_t>(sizeof(std::uint32_t)));

  PngImageGuard guard;
  guard.img.width = static_cast<png_uint_32>(w);
  guard.img.height = static_cast<png_uint_32>(h);
  guard.img.format = PNG_FORMAT_BGRA;

  png_alloc_size_t png_bytes =
      static_cast<png_alloc_size_t>(PNG_IMAGE_PNG_SIZE_MAX(guard.img));
  if (png_bytes == 0u) {
    return false;
  }
  std::vector<std::uint8_t> encoded(static_cast<std::size_t>(png_bytes));
  int ok = png_image_write_to_memory(&guard.img, encoded.data(), &png_bytes, 0,
                                     argb, row_stride, nullptr);
  if (ok == 0 && png_bytes > encoded.size()) {
    encoded.resize(static_cast<std::size_t>(png_bytes));
    PngImageGuard retry_guard;
    retry_guard.img.width = static_cast<png_uint_32>(w);
    retry_guard.img.height = static_cast<png_uint_32>(h);
    retry_guard.img.format = PNG_FORMAT_BGRA;
    ok = png_image_write_to_memory(&retry_guard.img, encoded.data(),
                                   &png_bytes, 0, argb, row_stride, nullptr);
  }
  if (ok == 0 || png_bytes == 0u || png_bytes > encoded.size()) {
    return false;
  }

  std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
  if (!ofs) {
    return false;
  }
  ofs.write(reinterpret_cast<const char*>(encoded.data()),
            static_cast<std::streamsize>(png_bytes));
  return ofs.good();
}

bool HdPackManager::load_argb_png(const std::filesystem::path& path, int& out_w,
                                  int& out_h,
                                  std::vector<std::uint32_t>& out_argb) {
  std::error_code ec;
  const auto st = std::filesystem::symlink_status(path, ec);
  if (ec || std::filesystem::is_symlink(st) ||
      !std::filesystem::is_regular_file(st)) {
    return false;
  }

  std::ifstream ifs(path, std::ios::binary | std::ios::ate);
  if (!ifs) {
    return false;
  }
  const auto end_pos = ifs.tellg();
  constexpr std::streamoff kMaxPngFileBytes = 64ll * 1024ll * 1024ll;
  if (end_pos <= 0 || end_pos > kMaxPngFileBytes) {
    return false;
  }
  std::vector<std::uint8_t> raw(static_cast<std::size_t>(end_pos));
  ifs.seekg(0, std::ios::beg);
  if (!ifs.read(reinterpret_cast<char*>(raw.data()),
                static_cast<std::streamsize>(raw.size()))) {
    return false;
  }

  PngImageGuard guard;
  if (!png_image_begin_read_from_memory(&guard.img, raw.data(), raw.size())) {
    return false;
  }
  if (guard.img.width < 1u ||
      guard.img.width > static_cast<png_uint_32>(kMaxPngDim) ||
      guard.img.height < 1u ||
      guard.img.height > static_cast<png_uint_32>(kMaxPngDim)) {
    return false;
  }
  guard.img.format = PNG_FORMAT_BGRA;
  out_w = static_cast<int>(guard.img.width);
  out_h = static_cast<int>(guard.img.height);
  out_argb.resize(static_cast<std::size_t>(out_w) *
                  static_cast<std::size_t>(out_h));
  const auto row_stride =
      static_cast<png_int_32>(static_cast<std::int64_t>(out_w) *
                              static_cast<std::int64_t>(sizeof(std::uint32_t)));
  const int ok = png_image_finish_read(&guard.img, nullptr, out_argb.data(),
                                       row_stride, nullptr);
  return ok != 0;
}

bool HdPackManager::sanitize_pack_relpath(std::string_view rel_sv,
                                          std::filesystem::path& out_rel) {
  out_rel.clear();
  if (rel_sv.empty() || rel_sv.size() > 1024u || rel_sv.front() == '/' ||
      rel_sv.back() == '/') {
    return false;
  }
  for (char ch : rel_sv) {
    const auto uc = static_cast<unsigned char>(ch);
    if (uc <= 0x1Fu || uc == 0x7Fu || ch == '\\' || ch == ':' || ch == '*' ||
        ch == '?' || ch == '<' || ch == '>' || ch == '"' || ch == '|') {
      return false;
    }
  }

  std::size_t pos = 0u;
  while (pos <= rel_sv.size()) {
    const std::size_t slash = rel_sv.find('/', pos);
    const std::string_view seg =
        (slash == std::string_view::npos)
            ? rel_sv.substr(pos)
            : rel_sv.substr(pos, slash - pos);
    if (seg.empty() || seg == "." || seg == ".." || seg.back() == '.' ||
        seg.back() == ' ' || util::is_win32_reserved_device_stem(seg)) {
      return false;
    }
    if (slash == std::string_view::npos) {
      break;
    }
    pos = slash + 1u;
  }

  std::filesystem::path parsed = util::utf8_to_path(rel_sv);
  if (parsed.empty() || parsed.is_absolute() || parsed.has_root_name() ||
      parsed.has_root_directory()) {
    return false;
  }
  out_rel = std::move(parsed);
  return true;
}

bool HdPackManager::verify_safe_pack_file(const std::filesystem::path& base_dir,
                                          const std::filesystem::path& rel_path,
                                          std::filesystem::path& out_full,
                                          std::uintmax_t* out_size) {
  out_full.clear();
  if (out_size != nullptr) {
    *out_size = 0u;
  }
  if (base_dir.empty() || rel_path.empty() || rel_path.is_absolute() ||
      rel_path.has_root_name() || rel_path.has_root_directory()) {
    return false;
  }

  std::error_code ec;
  if (!std::filesystem::is_directory(base_dir, ec) || ec) {
    return false;
  }

  std::vector<std::filesystem::path> parts;
  for (const auto& comp : rel_path) {
    if (comp.empty() || comp == "." || comp == "..") {
      return false;
    }
    parts.push_back(comp);
  }
  if (parts.empty()) {
    return false;
  }

  std::filesystem::path cur = base_dir;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    cur /= parts[i];
    const auto st = std::filesystem::symlink_status(cur, ec);
    if (ec || std::filesystem::is_symlink(st)) {
      return false;
    }
    if (i + 1u == parts.size()) {
      if (!std::filesystem::is_regular_file(st)) {
        return false;
      }
    } else if (!std::filesystem::is_directory(st)) {
      return false;
    }
  }

  if (out_size != nullptr) {
    const auto fsz = std::filesystem::file_size(cur, ec);
    if (ec) {
      return false;
    }
    *out_size = fsz;
  }
  out_full = std::move(cur);
  return true;
}

void HdPackManager::set_pack_dir(const std::filesystem::path& dir) {
  pack_dir_ = dir;
  reload_pack();
}

void HdPackManager::set_dump_dir(const std::filesystem::path& dir) {
  dump_dir_ = dir;
  dumping_enabled_ = !dump_dir_.empty();
}

void HdPackManager::set_max_cache_bytes(std::size_t max_bytes) {
  max_cache_bytes_ = std::max<std::size_t>(64u * 1024u, max_bytes);
  evict_lru_if_needed();
}

void HdPackManager::set_dumping_enabled(bool enabled) {
  dumping_enabled_ = enabled && !dump_dir_.empty();
}

bool HdPackManager::toggle_dumping(const std::filesystem::path& default_dir) {
  if (dump_dir_.empty()) {
    dump_dir_ = default_dir;
  }
  dumping_enabled_ = !dumping_enabled_;
  return dumping_enabled_;
}

std::uint16_t HdPackManager::intern_palette(
    const std::array<std::uint32_t, 16>& pal) const {
  const std::uint64_t h = hash_palette_array(pal);
  auto it = ref_palette_hash_.find(h);
  if (it != ref_palette_hash_.end()) {
    const std::uint16_t id = it->second;
    if (id < ref_palettes_.size()) {
      bool exact = true;
      for (std::size_t i = 0; i < 16u; ++i) {
        if ((ref_palettes_[id][i] & 0x00FFFFFFu) != (pal[i] & 0x00FFFFFFu)) {
          exact = false;
          break;
        }
      }
      if (exact) {
        return id;
      }
    }
  }
  if (ref_palettes_.size() >= 0xFFFEu) {
    return kDirectUngradedPalId;
  }
  const auto id = static_cast<std::uint16_t>(ref_palettes_.size());
  std::array<std::uint32_t, 16> norm{};
  for (std::size_t i = 0; i < 16u; ++i) {
    norm[i] = 0xFF000000u | (pal[i] & 0x00FFFFFFu);
  }
  ref_palettes_.push_back(norm);
  ref_palette_hash_[h] = id;
  return id;
}

std::size_t HdPackManager::reload_pack() {
  const auto t0 = std::chrono::steady_clock::now();

  manifest_map_.clear();
  mondata_index_.clear();
  button_index_.clear();
  tile_dict_.clear();
  // Keep ref_palettes_ and ref_palette_hash_ append-only across reload_pack()
  // so existing hd_ref_pal_id_ entries on hd_canvas_argb_ remain valid on F8.
  lut_cache_.clear();
  lut_lru_.clear();
  tile_dict_index_.clear();
  lru_order_.clear();
  cache_.clear();
  active_dynamic_cgs_.clear();
  resident_bytes_ = 0;
  indexed_count_ = 0;

  const auto master_pal = codecs::default_san4_master_palette();
  mondata_ref_pal_id_ = intern_palette(master_pal);
  button_ref_pal_id_ = mondata_ref_pal_id_;

  if (pack_dir_.empty()) {
    last_reload_ms_ = 0.0;
    return 0;
  }
  std::error_code ec;
  if (!std::filesystem::is_directory(pack_dir_, ec)) {
    last_reload_ms_ = 0.0;
    return 0;
  }

  // 1. Parse `hd_manifest.tsv` lazily without decoding any PNG files (< 15 ms).
  // Format per line:
  //   <hash_hex16>\t<base_w>\t<base_h>\t<bank_hint>\t<rel_path>\t<p0_hex6>,...,<p15_hex6>
  std::filesystem::path manifest_path;
  std::uintmax_t manifest_sz = 0u;
  if (verify_safe_pack_file(pack_dir_, util::utf8_to_path("hd_manifest.tsv"),
                            manifest_path, &manifest_sz) &&
      manifest_sz <= kMaxManifestBytes) {
    std::ifstream ifs(manifest_path, std::ios::binary);
    if (ifs) {
      manifest_map_.reserve(4096u);
      std::size_t accepted_entries = 0u;
      std::string line;
      while (accepted_entries < kMaxManifestEntries && std::getline(ifs, line)) {
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
          continue;
        }
        const std::string_view sv(line);
        const std::size_t t1 = sv.find('\t');
        if (t1 == std::string_view::npos) continue;
        const std::size_t t2 = sv.find('\t', t1 + 1u);
        if (t2 == std::string_view::npos) continue;
        const std::size_t t3 = sv.find('\t', t2 + 1u);
        if (t3 == std::string_view::npos) continue;
        const std::size_t t4 = sv.find('\t', t3 + 1u);
        if (t4 == std::string_view::npos) continue;
        const std::size_t t5 = sv.find('\t', t4 + 1u);

        std::uint64_t hash = 0;
        int bw = 0;
        int bh = 0;
        int bank = 0;
        if (!util::parse_u64_hex_strict(sv.substr(0u, t1), hash) ||
            !util::parse_int_strict(sv.substr(t1 + 1u, t2 - t1 - 1u), 1,
                                    kMaxPngDim, bw) ||
            !util::parse_int_strict(sv.substr(t2 + 1u, t3 - t2 - 1u), 1,
                                    kMaxPngDim, bh) ||
            !util::parse_int_strict(sv.substr(t3 + 1u, t4 - t3 - 1u), 0, 3,
                                    bank)) {
          continue;
        }

        const std::string_view rel_sv =
            (t5 == std::string_view::npos)
                ? sv.substr(t4 + 1u)
                : sv.substr(t4 + 1u, t5 - t4 - 1u);
        std::filesystem::path clean_rel;
        if (!sanitize_pack_relpath(rel_sv, clean_rel)) {
          continue;
        }

        std::uint16_t pal_id = kDirectUngradedPalId;
        if (t5 != std::string_view::npos) {
          const std::string_view pal_sv = sv.substr(t5 + 1u);
          std::array<std::uint32_t, 16> pal{};
          bool ok_pal = true;
          std::size_t pos = 0;
          for (int pi = 0; pi < 16; ++pi) {
            const std::size_t comma =
                (pi < 15) ? pal_sv.find(',', pos) : pal_sv.size();
            if (comma == std::string_view::npos || comma <= pos) {
              ok_pal = false;
              break;
            }
            std::uint64_t rgb64 = 0;
            if (!util::parse_u64_hex_strict(pal_sv.substr(pos, comma - pos),
                                            rgb64)) {
              ok_pal = false;
              break;
            }
            pal[static_cast<std::size_t>(pi)] =
                0xFF000000u | static_cast<std::uint32_t>(rgb64 & 0x00FFFFFFu);
            pos = comma + 1u;
          }
          if (ok_pal) {
            pal_id = intern_palette(pal);
            if (rel_sv.starts_with("portraits/")) {
              mondata_ref_pal_id_ = pal_id;
            } else if (rel_sv.starts_with("sprites/ui/")) {
              button_ref_pal_id_ = pal_id;
            }
          }
        }

        manifest_map_[hash] = ManifestEntry{
            .base_w = bw,
            .base_h = bh,
            .ref_pal_id = pal_id,
            .bank_hint = static_cast<std::uint8_t>(bank),
            .is_loose_override = false,
            .rel_path = std::string(rel_sv),
        };
        ++accepted_entries;
      }
    }
  }

  // 2. Load optional `mondata/mondata_index.bin` (8,192 combinations, 128 KB).
  std::filesystem::path mondata_idx_path;
  std::uintmax_t mondata_sz = 0u;
  if (verify_safe_pack_file(
          pack_dir_, util::utf8_to_path("mondata/mondata_index.bin"),
          mondata_idx_path, &mondata_sz) &&
      mondata_sz >= sizeof(MondataIndexEntry) &&
      (mondata_sz % sizeof(MondataIndexEntry)) == 0u &&
      mondata_sz <= 65536u * sizeof(MondataIndexEntry)) {
    mondata_index_.resize(
        static_cast<std::size_t>(mondata_sz / sizeof(MondataIndexEntry)));
    std::ifstream mifs(mondata_idx_path, std::ios::binary);
    if (mifs.read(reinterpret_cast<char*>(mondata_index_.data()),
                  static_cast<std::streamsize>(mondata_sz))) {
      std::sort(mondata_index_.begin(), mondata_index_.end(),
                [](const MondataIndexEntry& a, const MondataIndexEntry& b) {
                  return a.hash64 < b.hash64;
                });
    } else {
      mondata_index_.clear();
    }
  }

  // 3. Load optional `sprites/buttons/button_index.bin` (1,407 buttons, 22 KB).
  std::filesystem::path btn_idx_path;
  std::uintmax_t btn_sz = 0u;
  if (verify_safe_pack_file(
          pack_dir_, util::utf8_to_path("sprites/buttons/button_index.bin"),
          btn_idx_path, &btn_sz) &&
      btn_sz >= sizeof(ButtonIndexEntry) &&
      (btn_sz % sizeof(ButtonIndexEntry)) == 0u &&
      btn_sz <= 16384u * sizeof(ButtonIndexEntry)) {
    button_index_.resize(
        static_cast<std::size_t>(btn_sz / sizeof(ButtonIndexEntry)));
    std::ifstream bifs(btn_idx_path, std::ios::binary);
    if (bifs.read(reinterpret_cast<char*>(button_index_.data()),
                  static_cast<std::streamsize>(btn_sz))) {
      std::sort(button_index_.begin(), button_index_.end(),
                [](const ButtonIndexEntry& a, const ButtonIndexEntry& b) {
                  return a.hash64 < b.hash64;
                });
    } else {
      button_index_.clear();
    }
  }

  // 4. Load optional `tiles/tile_dict.bin` for dynamic 16x16 cell compositing.
  std::filesystem::path tile_dict_path;
  std::uintmax_t tile_dict_sz = 0u;
  if (verify_safe_pack_file(pack_dir_,
                            util::utf8_to_path("tiles/tile_dict.bin"),
                            tile_dict_path, &tile_dict_sz) &&
      tile_dict_sz >= sizeof(codecs::PackedTileDictEntry) &&
      (tile_dict_sz % sizeof(codecs::PackedTileDictEntry)) == 0u &&
      tile_dict_sz <= 8192u * sizeof(codecs::PackedTileDictEntry)) {
    const std::size_t n =
        static_cast<std::size_t>(tile_dict_sz / sizeof(codecs::PackedTileDictEntry));
    std::vector<codecs::PackedTileDictEntry> raw_entries(n);
    std::ifstream tifs(tile_dict_path, std::ios::binary);
    if (tifs.read(reinterpret_cast<char*>(raw_entries.data()),
                  static_cast<std::streamsize>(tile_dict_sz))) {
      tile_dict_.reserve(n);
      for (const auto& pe : raw_entries) {
        UnpackedTileDictEntry ue{};
        ue.hd_hash = pe.hd_hash;
        ue.archive_group = pe.archive_group;
        ue.is_overlay = (pe.is_overlay != 0u);
        ue.non_zero_count =
            (pe.full_zero_256 != 0u) ? 256 : static_cast<int>(pe.non_zero_count);
        auto mit = manifest_map_.find(pe.hd_hash);
        if (mit != manifest_map_.end()) {
          ue.ref_pal_id = mit->second.ref_pal_id;
        }
        for (int i = 0; i < 128; ++i) {
          ue.slots[static_cast<std::size_t>(i * 2)] =
              static_cast<std::uint8_t>(pe.packed_slots[i] & 0x0Fu);
          ue.slots[static_cast<std::size_t>(i * 2 + 1)] =
              static_cast<std::uint8_t>((pe.packed_slots[i] >> 4) & 0x0Fu);
        }
        for (int i = 0; i < 256; ++i) {
          ue.mask[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(
              (pe.zero_mask[i >> 3] >> (i & 7)) & 1u);
        }
        if (!ue.is_overlay) {
          tile_dict_index_.emplace(hash_sprite_4bit(16, 16, ue.slots.data()),
                                   static_cast<std::uint32_t>(tile_dict_.size()));
        }
        tile_dict_.push_back(ue);
      }
    }
  }

  // 5. Scan loose `img_<w>x<h>_<hash>.png` files in top-level `pack_dir_`
  // (custom user drop-ins override manifest entries and render ungraded RGB).
  for (const auto& entry : std::filesystem::directory_iterator(pack_dir_, ec)) {
    const auto st = entry.symlink_status(ec);
    if (ec || std::filesystem::is_symlink(st) ||
        !std::filesystem::is_regular_file(st)) {
      continue;
    }
    int base_w = 0;
    int base_h = 0;
    std::uint64_t hash = 0;
    const std::string fn = util::path_to_utf8(entry.path().filename());
    std::filesystem::path clean_fn;
    if (!parse_sprite_filename(fn, base_w, base_h, hash) ||
        !sanitize_pack_relpath(fn, clean_fn)) {
      continue;
    }
    if (!manifest_map_.contains(hash) &&
        manifest_map_.size() >= kMaxManifestEntries) {
      continue;
    }
    manifest_map_[hash] = ManifestEntry{
        .base_w = base_w,
        .base_h = base_h,
        .ref_pal_id = kDirectUngradedPalId,
        .bank_hint = 0u,
        .is_loose_override = true,
        .rel_path = fn,
    };
  }

  indexed_count_ =
      manifest_map_.size() + mondata_index_.size() + button_index_.size();
  const auto t1 = std::chrono::steady_clock::now();
  last_reload_ms_ =
      std::chrono::duration<double, std::milli>(t1 - t0).count();
  return indexed_count_;
}

std::uint32_t HdPackManager::alloc_region_id() const {
  if (!free_region_ids_.empty()) {
    const std::uint32_t id = free_region_ids_.back();
    free_region_ids_.pop_back();
    region_used_mask_[id] = 0u;
    return id;
  }
  const auto id = static_cast<std::uint32_t>(region_refcounts_.size());
  region_refcounts_.push_back(0u);
  region_used_mask_.push_back(0u);
  return id;
}

void HdPackManager::mark_hd_tiles_dirty(int x0, int y0, int x1, int y1) const {
  constexpr int kTile = VesaPlanarVideo::kTileSize;
  const int tx0 = std::max(x0, 0) / kTile;
  const int ty0 = std::max(y0, 0) / kTile;
  const int tx1 = (std::min(x1, VesaPlanarVideo::kWidth) + kTile - 1) / kTile;
  const int ty1 = (std::min(y1, VesaPlanarVideo::kHeight) + kTile - 1) / kTile;
  for (int ty = ty0; ty < ty1; ++ty) {
    for (int tx = tx0; tx < tx1; ++tx) {
      hd_dirty_tiles_[static_cast<std::size_t>(ty * VesaPlanarVideo::kTilesX + tx)] = 1u;
      hd_dirty_any_ = true;
    }
  }
}

void HdPackManager::flush_dirty_tiles(VesaPlanarVideo& video) const {
  if (!hd_dirty_any_) {
    return;
  }
  video.mark_upscale_tiles_dirty(hd_dirty_tiles_);
  hd_dirty_tiles_.fill(0u);
  hd_dirty_any_ = false;
}

void HdPackManager::update_hd_state(const VesaPlanarVideo& video) const {
  sync_vram_occlusions(video);
  hd_dirty_rects_.clear();
  drain_async_results();
  refresh_dynamic_cgs(video);
}

void HdPackManager::prepare_frame(VesaPlanarVideo& video) const {
  update_hd_state(video);
  frame_prepared_ = true;
  flush_dirty_tiles(video);
}

void HdPackManager::clear_logical_pixel(std::size_t lidx) const {
  const std::uint32_t old_reg = hd_region_id_[lidx];
  if (old_reg != 0u) {
    hd_region_id_[lidx] = 0u;
    if (old_reg < region_refcounts_.size() && region_refcounts_[old_reg] > 0u) {
      if (--region_refcounts_[old_reg] == 0u) {
        free_region_ids_.push_back(old_reg);
        if (active_region_count_ > 0u) {
          --active_region_count_;
        }
      }
    }
  }
  hd_ref_pal_id_[lidx] = kDirectUngradedPalId;
  hd_bank_hint_[lidx] = 0u;

  const int lx = static_cast<int>(lidx % VesaPlanarVideo::kWidth);
  const int ly = static_cast<int>(lidx / VesaPlanarVideo::kWidth);
  mark_hd_tiles_dirty(lx, ly, lx + 1, ly + 1);
  const int hx0 = lx * kHdScale;
  const int hy0 = ly * kHdScale;
  for (int dy = 0; dy < kHdScale; ++dy) {
    std::uint32_t* row =
        hd_canvas_argb_.data() +
        static_cast<std::size_t>(hy0 + dy) * static_cast<std::size_t>(kCanvasW) +
        static_cast<std::size_t>(hx0);
    row[0] = 0u;
    row[1] = 0u;
    row[2] = 0u;
    row[3] = 0u;
  }
}

void HdPackManager::clear_canvas_clipped_rect(int x0, int y0, int x1_excl,
                                              int y1_excl) const {
  if (active_region_count_ == 0u) {
    return;
  }
  const int cx0 = std::max(0, x0);
  const int cy0 = std::max(0, y0);
  const int cx1 = std::min(VesaPlanarVideo::kWidth, x1_excl);
  const int cy1 = std::min(VesaPlanarVideo::kHeight, y1_excl);
  if (cx0 >= cx1 || cy0 >= cy1) {
    return;
  }
  for (int ly = cy0; ly < cy1; ++ly) {
    std::size_t lidx =
        static_cast<std::size_t>(ly) * VesaPlanarVideo::kWidth +
        static_cast<std::size_t>(cx0);
    for (int lx = cx0; lx < cx1; ++lx, ++lidx) {
      if (hd_region_id_[lidx] != 0u) {
        clear_logical_pixel(lidx);
      }
    }
  }
}

void HdPackManager::sync_vram_occlusions(const VesaPlanarVideo& video) const {
  if (!video.has_hd_occlude_bits()) {
    return;
  }
  if (active_region_count_ == 0u) {
    video.clear_hd_occlude_bits();
    return;
  }
  const auto& bits = video.hd_occlude_bits();
  for (std::size_t wi = 0; wi < bits.size(); ++wi) {
    std::uint64_t word = bits[wi];
    while (word != 0u) {
      const int bit = __builtin_ctzll(word);
      const std::size_t lidx = (wi << 6u) + static_cast<std::size_t>(bit);
      if (hd_region_id_[lidx] != 0u) {
        clear_logical_pixel(lidx);
      }
      word &= (word - 1u);
    }
  }
  video.clear_hd_occlude_bits();
}

void HdPackManager::evict_lru_to_fit(std::size_t limit_bytes) const {
  while (resident_bytes_ > limit_bytes && !lru_order_.empty()) {
    const std::uint64_t victim_key = lru_order_.back();
    lru_order_.pop_back();
    auto it = cache_.find(victim_key);
    if (it != cache_.end()) {
      if (resident_bytes_ >= it->second.byte_size) {
        resident_bytes_ -= it->second.byte_size;
      } else {
        resident_bytes_ = 0u;
      }
      cache_.erase(it);
      ++cache_evictions_;
    }
  }
}

void HdPackManager::evict_lru_if_needed() const {
  evict_lru_to_fit(max_cache_bytes_);
}

void HdPackManager::publish_or_hold_texture(std::uint64_t key, HdTexture&& tex,
                                            std::uint16_t ref_pal_id,
                                            std::uint8_t bank_hint,
                                            ResolvedTexture& out) const {
  auto existing = cache_.find(key);
  if (existing != cache_.end()) {
    if (resident_bytes_ >= existing->second.byte_size) {
      resident_bytes_ -= existing->second.byte_size;
    } else {
      resident_bytes_ = 0u;
    }
    lru_order_.erase(existing->second.lru_it);
    cache_.erase(existing);
  }

  out.ref_pal_id = ref_pal_id;
  out.bank_hint = bank_hint;
  const std::size_t bytes = tex.argb.size() * sizeof(std::uint32_t);
  if (bytes > max_cache_bytes_) {
    out.owned_tex = std::move(tex);
    out.tex = &out.owned_tex;
    return;
  }

  evict_lru_to_fit(max_cache_bytes_ - bytes);
  lru_order_.push_front(key);
  CacheEntry ce{};
  ce.tex = std::move(tex);
  ce.ref_pal_id = ref_pal_id;
  ce.bank_hint = bank_hint;
  ce.byte_size = bytes;
  ce.lru_it = lru_order_.begin();
  auto [ins_it, _] = cache_.emplace(key, std::move(ce));
  resident_bytes_ += bytes;
  out.owned_tex = HdTexture{};
  out.tex = &ins_it->second.tex;
}

bool HdPackManager::resolve_or_load_texture(std::uint64_t hash,
                                            ResolvedTexture& out) {
  auto cit = cache_.find(hash);
  if (cit != cache_.end()) {
    lru_order_.splice(lru_order_.begin(), lru_order_, cit->second.lru_it);
    ++cache_hits_;
    out.owned_tex = HdTexture{};
    out.tex = &cit->second.tex;
    out.ref_pal_id = cit->second.ref_pal_id;
    out.bank_hint = cit->second.bank_hint;
    return true;
  }

  auto mit = manifest_map_.find(hash);
  if (mit != manifest_map_.end()) {
    ++cache_misses_;
    std::filesystem::path rel_path;
    std::filesystem::path full_path;
    if (!sanitize_pack_relpath(mit->second.rel_path, rel_path) ||
        !verify_safe_pack_file(pack_dir_, rel_path, full_path)) {
      return false;
    }
    HdTexture tex{};
    tex.base_w = mit->second.base_w;
    tex.base_h = mit->second.base_h;
    if (!load_argb_png(full_path, tex.hd_w, tex.hd_h, tex.argb)) {
      return false;
    }
    if (tex.base_w > 0 && tex.base_h > 0 &&
        (static_cast<std::int64_t>(tex.hd_w) >
             static_cast<std::int64_t>(tex.base_w) * 8 ||
         static_cast<std::int64_t>(tex.hd_h) >
             static_cast<std::int64_t>(tex.base_h) * 8)) {
      return false;
    }
    publish_or_hold_texture(hash, std::move(tex), mit->second.ref_pal_id,
                            mit->second.bank_hint, out);
    return true;
  }

  if (try_composite_mondata(hash, out)) {
    return true;
  }
  if (try_composite_button(hash, out)) {
    return true;
  }
  return false;
}

bool HdPackManager::try_composite_mondata(std::uint64_t hash,
                                          ResolvedTexture& out) {
  if (mondata_index_.empty()) {
    return false;
  }
  auto it = std::lower_bound(
      mondata_index_.begin(), mondata_index_.end(), hash,
      [](const MondataIndexEntry& e, std::uint64_t h) { return e.hash64 < h; });
  if (it == mondata_index_.end() || it->hash64 != hash) {
    return false;
  }
  ++cache_misses_;

  auto consume_part = [&](int kind, int tmpl, int idx,
                          auto&& consume_fn) -> bool {
    static constexpr int kPartW = 256;
    static constexpr int kPartH[4] = {144, 176, 64, 80};
    static constexpr const char* kKindNames[4] = {"top", "bot", "eye", "mouth"};
    const int exp_h = kPartH[kind];
    const std::size_t exp_px =
        static_cast<std::size_t>(kPartW) * static_cast<std::size_t>(exp_h);
    const std::uint64_t key =
        kMondataPartKeyPrefix | (static_cast<std::uint64_t>(tmpl & 0xFF) << 16) |
        (static_cast<std::uint64_t>(kind & 0xFF) << 8) |
        static_cast<std::uint64_t>(idx & 0xFF);
    auto pcit = cache_.find(key);
    if (pcit != cache_.end()) {
      if (pcit->second.tex.hd_w != kPartW || pcit->second.tex.hd_h != exp_h ||
          pcit->second.tex.argb.size() != exp_px) {
        return false;
      }
      lru_order_.splice(lru_order_.begin(), lru_order_, pcit->second.lru_it);
      consume_fn(pcit->second.tex);
      return true;
    }

    std::ostringstream fn;
    fn << "mondata/t" << std::setw(2) << std::setfill('0') << tmpl << "_"
       << kKindNames[kind] << "_" << idx << ".png";
    std::filesystem::path rel_path;
    std::filesystem::path full_path;
    if (!sanitize_pack_relpath(fn.str(), rel_path) ||
        !verify_safe_pack_file(pack_dir_, rel_path, full_path)) {
      return false;
    }

    HdTexture ptex{};
    if (!load_argb_png(full_path, ptex.hd_w, ptex.hd_h, ptex.argb)) {
      return false;
    }
    if (ptex.hd_w != kPartW || ptex.hd_h != exp_h ||
        ptex.argb.size() != exp_px) {
      return false;
    }
    consume_fn(ptex);
    ResolvedTexture part_slot{};
    publish_or_hold_texture(key, std::move(ptex), mondata_ref_pal_id_, 1u,
                            part_slot);
    return true;
  };

  HdTexture comp{};
  comp.base_w = 64;
  comp.base_h = 80;
  comp.hd_w = 256;
  comp.hd_h = 320;
  comp.argb.resize(256u * 320u);

  if (!consume_part(0, it->tmpl, it->top, [&](const HdTexture& top) {
        std::memcpy(comp.argb.data(), top.argb.data(),
                    256u * 144u * sizeof(std::uint32_t));
      })) {
    return false;
  }
  if (!consume_part(1, it->tmpl, it->bot, [&](const HdTexture& bot) {
        std::memcpy(comp.argb.data() + 256u * 144u, bot.argb.data(),
                    256u * 176u * sizeof(std::uint32_t));
      })) {
    return false;
  }

  auto blend_layer = [&](const HdTexture& layer, int base_y) {
    const int hy0 = base_y * kHdScale;
    for (int ly = 0; ly < layer.hd_h; ++ly) {
      const int dy = hy0 + ly;
      if (dy < 0 || dy >= 320) continue;
      std::uint32_t* drow = comp.argb.data() + static_cast<std::size_t>(dy) * 256u;
      const std::uint32_t* srow =
          layer.argb.data() + static_cast<std::size_t>(ly) * 256u;
      for (int lx = 0; lx < 256; ++lx) {
        drow[lx] = blend_straight_over(drow[lx], srow[lx]);
      }
    }
  };
  if (!consume_part(2, it->tmpl, it->eye, [&](const HdTexture& eye) {
        blend_layer(eye, static_cast<int>(it->eye_y));
      })) {
    return false;
  }
  if (!consume_part(3, it->tmpl, it->mouth, [&](const HdTexture& mouth) {
        blend_layer(mouth, static_cast<int>(it->mouth_y));
      })) {
    return false;
  }

  publish_or_hold_texture(hash, std::move(comp), mondata_ref_pal_id_, 1u, out);
  return true;
}

bool HdPackManager::try_composite_button(std::uint64_t hash,
                                         ResolvedTexture& out) {
  if (button_index_.empty()) {
    return false;
  }
  auto it = std::lower_bound(
      button_index_.begin(), button_index_.end(), hash,
      [](const ButtonIndexEntry& e, std::uint64_t h) { return e.hash64 < h; });
  if (it == button_index_.end() || it->hash64 != hash) {
    return false;
  }
  ++cache_misses_;

  auto consume_btn_part = [&](bool is_label, int idx, int exp_w, int exp_h,
                              auto&& consume_fn) -> bool {
    const std::size_t exp_px =
        static_cast<std::size_t>(exp_w) * static_cast<std::size_t>(exp_h);
    const std::uint64_t key =
        kButtonPartKeyPrefix | (is_label ? 0x10000ull : 0ull) |
        static_cast<std::uint64_t>(idx & 0xFF);
    auto pcit = cache_.find(key);
    if (pcit != cache_.end()) {
      if (pcit->second.tex.hd_w != exp_w || pcit->second.tex.hd_h != exp_h ||
          pcit->second.tex.argb.size() != exp_px) {
        return false;
      }
      lru_order_.splice(lru_order_.begin(), lru_order_, pcit->second.lru_it);
      consume_fn(pcit->second.tex);
      return true;
    }

    std::ostringstream fn;
    fn << "sprites/buttons/" << (is_label ? "label_" : "base_") << std::setw(2)
       << std::setfill('0') << idx << ".png";
    std::filesystem::path rel_path;
    std::filesystem::path full_path;
    if (!sanitize_pack_relpath(fn.str(), rel_path) ||
        !verify_safe_pack_file(pack_dir_, rel_path, full_path)) {
      return false;
    }

    HdTexture ptex{};
    if (!load_argb_png(full_path, ptex.hd_w, ptex.hd_h, ptex.argb)) {
      return false;
    }
    if (ptex.hd_w != exp_w || ptex.hd_h != exp_h ||
        ptex.argb.size() != exp_px) {
      return false;
    }
    consume_fn(ptex);
    ResolvedTexture part_slot{};
    publish_or_hold_texture(key, std::move(ptex), button_ref_pal_id_, 1u,
                            part_slot);
    return true;
  };

  // Verified from MAIN.EXE 0x2C5EA..0x2C643: the 32x16 Kanji label replaces
  // rows y=16..31 (bottom 64 HD rows 64..127) of the 32x32 button!
  HdTexture comp{};
  comp.base_w = 32;
  comp.base_h = 32;
  comp.hd_w = 128;
  comp.hd_h = 128;
  if (!consume_btn_part(false, it->base_id, 128, 128,
                        [&](const HdTexture& base) { comp.argb = base.argb; })) {
    return false;
  }
  if (!consume_btn_part(true, it->label_id, 128, 64,
                        [&](const HdTexture& label) {
                          std::memcpy(comp.argb.data() + 128u * 64u,
                                      label.argb.data(),
                                      128u * 64u * sizeof(std::uint32_t));
                        })) {
    return false;
  }

  publish_or_hold_texture(hash, std::move(comp), button_ref_pal_id_, 1u, out);
  return true;
}

bool HdPackManager::try_composite_cell_16x16(
    std::uint64_t hash, const std::uint8_t* target_slots_256,
    ResolvedTexture& out) {
  if (tile_dict_.empty()) {
    return false;
  }

  // Step 1: Find best-matching opaque 16x16 base tile: the first entry with
  // the most matching slots. Exact matches come from the hash index; the
  // linear scan only runs when no entry matches exactly.
  int best_base_idx = -1;
  int best_base_matches = -1;
  const auto [cand_begin, cand_end] = tile_dict_index_.equal_range(hash);
  for (auto it = cand_begin; it != cand_end; ++it) {
    const auto& td = tile_dict_[it->second];
    bool exact = true;
    for (int p = 0; p < 256 && exact; ++p) {
      exact = td.slots[static_cast<std::size_t>(p)] ==
              (target_slots_256[p] & 0x0Fu);
    }
    if (exact && (best_base_idx < 0 ||
                  static_cast<int>(it->second) < best_base_idx)) {
      best_base_idx = static_cast<int>(it->second);
      best_base_matches = 256;
    }
  }
  for (std::size_t i = 0; i < tile_dict_.size() && best_base_matches < 256;
       ++i) {
    const auto& td = tile_dict_[i];
    if (td.is_overlay) continue;
    int matches = 0;
    for (int p = 0; p < 256; ++p) {
      if (td.slots[static_cast<std::size_t>(p)] ==
          (target_slots_256[p] & 0x0Fu)) {
        ++matches;
      }
    }
    if (matches > best_base_matches) {
      best_base_matches = matches;
      best_base_idx = static_cast<int>(i);
      if (matches == 256) break;
    }
  }

  if (best_base_idx < 0 || best_base_matches < 96) {
    return false;
  }
  const auto& base_td = tile_dict_[static_cast<std::size_t>(best_base_idx)];
  ResolvedTexture base_res{};
  if (!resolve_or_load_texture(base_td.hd_hash, base_res) ||
      base_res.tex == nullptr || base_res.tex->hd_w != 64 ||
      base_res.tex->hd_h != 64 || base_res.tex->argb.size() != 64u * 64u) {
    return false;
  }

  // Step 2: Check if a known 16x16 masked overlay (city, mountain, gate, unit,
  // tree) explains the majority of differences over `base_td`.
  int best_ov_idx = -1;
  int best_ov_explained = 0;
  if (best_base_matches < 256) {
    for (std::size_t i = 0; i < tile_dict_.size(); ++i) {
      const auto& ov = tile_dict_[i];
      if (!ov.is_overlay || ov.non_zero_count < 6) continue;
      if (ov.archive_group != base_td.archive_group && ov.archive_group != 4u) {
        continue;
      }
      int ov_match = 0;
      int ov_explained_diff = 0;
      for (int p = 0; p < 256; ++p) {
        if (ov.mask[static_cast<std::size_t>(p)] != 0u) {
          const std::uint8_t ts =
              static_cast<std::uint8_t>(target_slots_256[p] & 0x0Fu);
          if (ov.slots[static_cast<std::size_t>(p)] == ts) {
            ++ov_match;
            if (base_td.slots[static_cast<std::size_t>(p)] != ts) {
              ++ov_explained_diff;
            }
          }
        }
      }
      if (ov_match * 10 >= ov.non_zero_count * 9 &&
          ov_explained_diff > best_ov_explained) {
        best_ov_explained = ov_explained_diff;
        best_ov_idx = static_cast<int>(i);
      }
    }
  }

  HdTexture comp{};
  comp.base_w = 16;
  comp.base_h = 16;
  comp.hd_w = 64;
  comp.hd_h = 64;
  comp.argb = base_res.tex->argb;
  base_res = ResolvedTexture{};

  std::array<std::uint8_t, 256> model_slots = base_td.slots;
  if (best_ov_idx >= 0) {
    const auto& ov_td = tile_dict_[static_cast<std::size_t>(best_ov_idx)];
    ResolvedTexture ov_res{};
    if (resolve_or_load_texture(ov_td.hd_hash, ov_res) &&
        ov_res.tex != nullptr && ov_res.tex->hd_w == 64 &&
        ov_res.tex->hd_h == 64 && ov_res.tex->argb.size() == 64u * 64u) {
      for (std::size_t k = 0; k < 64u * 64u; ++k) {
        comp.argb[k] = blend_straight_over(comp.argb[k], ov_res.tex->argb[k]);
      }
      for (int p = 0; p < 256; ++p) {
        if (ov_td.mask[static_cast<std::size_t>(p)] != 0u) {
          model_slots[static_cast<std::size_t>(p)] =
              ov_td.slots[static_cast<std::size_t>(p)];
        }
      }
    }
  }

  // Step 3: Render any remaining residual pixels (software-drawn road lines,
  // borders, or city-name Kanji strokes) cleanly at 4x over the HD composite!
  std::array<std::uint8_t, 256> res_mask{};
  std::array<std::uint8_t, 256> res_slots{};
  int res_count = 0;
  for (int p = 0; p < 256; ++p) {
    const auto ts = static_cast<std::uint8_t>(target_slots_256[p] & 0x0Fu);
    res_slots[static_cast<std::size_t>(p)] = ts;
    if (model_slots[static_cast<std::size_t>(p)] != ts) {
      res_mask[static_cast<std::size_t>(p)] = 1u;
      ++res_count;
    }
  }
  if (res_count > 160 && best_ov_idx < 0) {
    // Too dissimilar from any known terrain tile (e.g. arbitrary bitmap).
    return false;
  }
  if (res_count > 0) {
    const std::array<std::uint32_t, 16> pal =
        (base_td.ref_pal_id < ref_palettes_.size())
            ? ref_palettes_[base_td.ref_pal_id]
            : codecs::default_san4_master_palette();
    std::vector<std::uint32_t> res_4x;
    hd_gen::upscale_art_4x_argb(res_slots.data(), res_mask.data(), 16, 16, pal,
                                hd_gen::ArtAssetKind::MaskedSprite, res_4x);
    for (std::size_t k = 0; k < 64u * 64u; ++k) {
      comp.argb[k] = blend_straight_over(comp.argb[k], res_4x[k]);
    }
  }

  publish_or_hold_texture(hash, std::move(comp), base_td.ref_pal_id, 0u, out);
  return true;
}

std::uint32_t HdPackManager::paint_hd_texture_clipped(
    int dst_x, int dst_y, int w, int h, GrpClipRect clip, const HdTexture& tex,
    std::uint16_t ref_pal_id, std::uint8_t bank_hint, std::uint16_t used_mask,
    bool opaque_replace, const VesaPlanarVideo& video) {
  if (w <= 0 || h <= 0 || tex.hd_w <= 0 || tex.hd_h <= 0) {
    return 0u;
  }

  const std::int64_t x0_64 =
      std::max<std::int64_t>({0, clip.x0, dst_x});
  const std::int64_t y0_64 =
      std::max<std::int64_t>({0, clip.y0, dst_y});
  const std::int64_t x1_64 = std::min<std::int64_t>(
      {VesaPlanarVideo::kWidth, static_cast<std::int64_t>(clip.x1) + 1,
       static_cast<std::int64_t>(dst_x) + w});
  const std::int64_t y1_64 = std::min<std::int64_t>(
      {VesaPlanarVideo::kHeight, static_cast<std::int64_t>(clip.y1) + 1,
       static_cast<std::int64_t>(dst_y) + h});
  if (x0_64 >= x1_64 || y0_64 >= y1_64) {
    return 0u;
  }

  const int x0 = static_cast<int>(x0_64);
  const int y0 = static_cast<int>(y0_64);
  const int x1 = static_cast<int>(x1_64);
  const int y1 = static_cast<int>(y1_64);

  const auto& svram = video.shadow_vram();
  const std::uint32_t new_reg = alloc_region_id();
  std::uint32_t assigned_pixels = 0u;
  mark_hd_tiles_dirty(x0, y0, x1, y1);

  const bool exact_4x = (tex.hd_w == w * kHdScale && tex.hd_h == h * kHdScale);
  const std::int64_t total_hw = static_cast<std::int64_t>(w) * kHdScale;
  const std::int64_t total_hh = static_cast<std::int64_t>(h) * kHdScale;

  for (int ly = y0; ly < y1; ++ly) {
    const int local_ly = ly - dst_y;
    for (int lx = x0; lx < x1; ++lx) {
      const int local_lx = lx - dst_x;
      const std::size_t lidx =
          static_cast<std::size_t>(ly) * VesaPlanarVideo::kWidth +
          static_cast<std::size_t>(lx);

      bool any_alpha = false;
      bool dominant_src = opaque_replace;
      std::uint32_t alpha_sum = 0u;

      const int hx_base = lx * kHdScale;
      const int hy_base = ly * kHdScale;
      const int src_hx0 = local_lx * kHdScale;
      const int src_hy0 = local_ly * kHdScale;

      for (int sy = 0; sy < kHdScale; ++sy) {
        const int ty =
            exact_4x ? (src_hy0 + sy)
                     : static_cast<int>(
                           (static_cast<std::int64_t>(src_hy0 + sy) * tex.hd_h) /
                           total_hh);
        const std::uint32_t* trow =
            tex.argb.data() +
            static_cast<std::size_t>(ty) * static_cast<std::size_t>(tex.hd_w);
        std::uint32_t* crow =
            hd_canvas_argb_.data() +
            static_cast<std::size_t>(hy_base + sy) *
                static_cast<std::size_t>(kCanvasW) +
            static_cast<std::size_t>(hx_base);

        for (int sx = 0; sx < kHdScale; ++sx) {
          const int tx =
              exact_4x
                  ? (src_hx0 + sx)
                  : static_cast<int>(
                        (static_cast<std::int64_t>(src_hx0 + sx) * tex.hd_w) /
                        total_hw);
          const std::uint32_t spx = trow[static_cast<std::size_t>(tx)];
          const std::uint32_t sa = (spx >> 24) & 0xFFu;
          if (opaque_replace) {
            crow[sx] = (sa == 0u) ? 0u : spx;
            if (sa != 0u) {
              any_alpha = true;
              alpha_sum += sa;
            }
          } else if (sa != 0u) {
            crow[sx] = blend_straight_over(crow[sx], spx);
            any_alpha = true;
            alpha_sum += sa;
          }
        }
      }

      if (opaque_replace && !any_alpha) {
        if (hd_region_id_[lidx] != 0u) {
          clear_logical_pixel(lidx);
        }
        continue;
      }

      if (any_alpha) {
        if (!opaque_replace && alpha_sum >= 2040u) {
          dominant_src = true;
        }
        if (opaque_replace || hd_region_id_[lidx] == 0u || dominant_src) {
          const std::uint32_t old_reg = hd_region_id_[lidx];
          if (old_reg != 0u && old_reg != new_reg) {
            if (--region_refcounts_[old_reg] == 0u) {
              free_region_ids_.push_back(old_reg);
              if (active_region_count_ > 0u) {
                --active_region_count_;
              }
            }
          }
          if (old_reg != new_reg) {
            hd_region_id_[lidx] = new_reg;
            ++assigned_pixels;
          }
          hd_ref_pal_id_[lidx] = ref_pal_id;
          hd_bank_hint_[lidx] = bank_hint;
        } else {
          // Blended into a surviving region: it now also shows these colors.
          region_used_mask_[hd_region_id_[lidx]] |= used_mask;
        }
        hd_snap_slot_[lidx] = svram[lidx];
      }
    }
  }

  if (assigned_pixels > 0u) {
    region_refcounts_[new_reg] += assigned_pixels;
    region_used_mask_[new_reg] = used_mask;
    ++active_region_count_;
    return new_reg;
  }
  free_region_ids_.push_back(new_reg);
  return 0u;
}

void HdPackManager::register_dynamic_cg(
    std::uint32_t region_id, int dst_x, int dst_y, int w, int h,
    GrpClipRect clip, std::uint64_t base_hash, std::uint16_t cur_pal_id,
    std::span<const std::uint8_t> vga_slots) {
  if (region_id == 0u || w <= 0 || h <= 0 ||
      vga_slots.size() <
          static_cast<std::size_t>(w) * static_cast<std::size_t>(h)) {
    return;
  }
  std::erase_if(active_dynamic_cgs_, [this](const ActiveDynamicCg& c) {
    return c.region_id == 0u || c.region_id >= region_refcounts_.size() ||
           region_refcounts_[c.region_id] == 0u;
  });
  ActiveDynamicCg item{};
  item.region_id = region_id;
  item.dst_x = dst_x;
  item.dst_y = dst_y;
  item.w = w;
  item.h = h;
  item.clip = clip;
  item.base_hash = base_hash;
  item.cur_pal_id = cur_pal_id;
  item.vga_slots.assign(
      vga_slots.begin(),
      vga_slots.begin() +
          static_cast<std::ptrdiff_t>(static_cast<std::size_t>(w) *
                                      static_cast<std::size_t>(h)));
  active_dynamic_cgs_.push_back(std::move(item));
  while (active_dynamic_cgs_.size() > kMaxActiveDynamicCgs) {
    active_dynamic_cgs_.pop_front();
  }
}

void HdPackManager::refresh_dynamic_cgs(const VesaPlanarVideo& video) const {
  if (active_dynamic_cgs_.empty()) {
    return;
  }
  std::erase_if(active_dynamic_cgs_, [this](const ActiveDynamicCg& c) {
    return c.region_id == 0u || c.region_id >= region_refcounts_.size() ||
           region_refcounts_[c.region_id] == 0u;
  });
  if (active_dynamic_cgs_.empty()) {
    return;
  }

  const auto target_pal = video.reference_nonblack_palette_argb();
  int max_target_chan = 0;
  for (std::uint32_t c : target_pal) {
    max_target_chan = std::max<int>({max_target_chan,
                                     static_cast<int>((c >> 16) & 0xFFu),
                                     static_cast<int>((c >> 8) & 0xFFu),
                                     static_cast<int>(c & 0xFFu)});
  }
  if (max_target_chan <= 16) {
    return;
  }

  for (auto& cg : active_dynamic_cgs_) {
    int max_diff = 255;
    if (cg.cur_pal_id < ref_palettes_.size()) {
      max_diff = 0;
      const auto& cur_pal = ref_palettes_[cg.cur_pal_id];
      for (std::size_t i = 0; i < 16u; ++i) {
        const int cr = static_cast<int>((cur_pal[i] >> 16) & 0xFFu);
        const int cg_ch = static_cast<int>((cur_pal[i] >> 8) & 0xFFu);
        const int cb = static_cast<int>(cur_pal[i] & 0xFFu);
        const int tr = static_cast<int>((target_pal[i] >> 16) & 0xFFu);
        const int tg = static_cast<int>((target_pal[i] >> 8) & 0xFFu);
        const int tb = static_cast<int>(target_pal[i] & 0xFFu);
        max_diff = std::max(
            {max_diff, std::abs(tr - cr), std::abs(tg - cg_ch), std::abs(tb - cb)});
      }
    }
    if (max_diff <= 13) {
      // Current pixels match: a pending job's result must not repaint.
      cg.target_key = 0;
      continue;
    }

    const std::uint16_t new_pal_id = intern_palette(target_pal);
    const std::uint64_t dyn_key =
        cg.base_hash ^
        (hash_palette_array(target_pal) * 0x9E3779B97F4A7C15ull) ^
        0xD1AC600000000003ull;
    cg.target_key = dyn_key;
    cg.target_pal = target_pal;

    ResolvedTexture dyn_res{};
    auto cit = cache_.find(dyn_key);
    if (cit != cache_.end()) {
      lru_order_.splice(lru_order_.begin(), lru_order_, cit->second.lru_it);
      ++cache_hits_;
      dyn_res.tex = &cit->second.tex;
    } else if (async_pool_ != nullptr) {
      // A job already in flight for this CG picks up the new target when
      // its result is drained.
      if (!async_inflight_.contains(cg.region_id)) {
        ++cache_misses_;
        enqueue_dynamic_cg_job(cg);
      }
      continue;
    } else {
      ++cache_misses_;
      HdTexture dyn_tex{};
      dyn_tex.base_w = cg.w;
      dyn_tex.base_h = cg.h;
      dyn_tex.hd_w = cg.w * kHdScale;
      dyn_tex.hd_h = cg.h * kHdScale;
      cg_upscale_fn_(cg.vga_slots.data(), cg.w, cg.h, target_pal, dyn_tex.argb);
      publish_or_hold_texture(dyn_key, std::move(dyn_tex), new_pal_id, 3u,
                              dyn_res);
    }
    if (dyn_res.tex != nullptr) {
      paint_dynamic_cg(cg, *dyn_res.tex, new_pal_id);
    }
  }
}

void HdPackManager::enqueue_dynamic_cg_job(const ActiveDynamicCg& cg) const {
  async_inflight_.insert(cg.region_id);
  {
    // Every in-flight job delivers exactly one result, so with this much
    // room a job's push_back never allocates, not even after a failure.
    std::lock_guard<std::mutex> lk(async_queue_->mu);
    async_queue_->done.reserve(async_inflight_.size());
  }
  // The job runs inside the pool, where the pool-aware upscaler overload would
  // run inline anyway, so it uses the serial one (hundreds of ms for a
  // full-screen CG, off the frame path). It touches only its own copies and
  // the shared result queue.
  async_pool_->enqueue([queue = async_queue_, upscale = cg_upscale_fn_,
                        slots = cg.vga_slots, w = cg.w, h = cg.h,
                        target_pal = cg.target_pal, dyn_key = cg.target_key,
                        region_id = cg.region_id]() {
    AsyncCgQueue::Result result;
    result.dyn_key = dyn_key;
    result.region_id = region_id;
    result.target_pal = target_pal;
    try {
      result.tex.base_w = w;
      result.tex.base_h = h;
      result.tex.hd_w = w * kHdScale;
      result.tex.hd_h = h * kHdScale;
      upscale(slots.data(), w, h, target_pal, result.tex.argb);
      result.ok = true;
    } catch (...) {
      // Reported as a failed result so the drain clears the in-flight entry
      // and a later frame can retry.
      result.tex = HdTexture{};
    }
    std::lock_guard<std::mutex> lk(queue->mu);
    queue->done.push_back(std::move(result));
    queue->ready.store(queue->done.size(), std::memory_order_release);
  });
}

void HdPackManager::drain_async_results() const {
  if (async_queue_->ready.load(std::memory_order_acquire) == 0u) {
    return;
  }
  std::vector<AsyncCgQueue::Result> done;
  {
    std::lock_guard<std::mutex> lk(async_queue_->mu);
    done.swap(async_queue_->done);
    async_queue_->ready.store(0u, std::memory_order_relaxed);
  }
  for (auto& r : done) {
    async_inflight_.erase(r.region_id);
    ResolvedTexture dyn_res{};
    std::uint16_t pal_id = kDirectUngradedPalId;
    if (r.ok) {
      // Interned here rather than in the job: palette ids are owned by this
      // thread.
      pal_id = intern_palette(r.target_pal);
      publish_or_hold_texture(r.dyn_key, std::move(r.tex), pal_id, 3u, dyn_res);
    }
    if (r.region_id >= region_refcounts_.size() ||
        region_refcounts_[r.region_id] == 0u) {
      continue;
    }
    for (auto& cg : active_dynamic_cgs_) {
      if (cg.region_id != r.region_id) {
        continue;
      }
      if (cg.target_key == r.dyn_key) {
        if (dyn_res.tex != nullptr) {
          paint_dynamic_cg(cg, *dyn_res.tex, pal_id);
        }
      } else if (cg.target_key != 0u && !cache_.contains(cg.target_key)) {
        enqueue_dynamic_cg_job(cg);
      }
      break;
    }
  }
}

void HdPackManager::paint_dynamic_cg(ActiveDynamicCg& cg, const HdTexture& tex,
                                     std::uint16_t new_pal_id) const {
  if (tex.hd_w != cg.w * kHdScale || tex.hd_h != cg.h * kHdScale) {
    return;
  }
  const int x0 =
      static_cast<int>(std::max<std::int64_t>({0, cg.clip.x0, cg.dst_x}));
  const int y0 =
      static_cast<int>(std::max<std::int64_t>({0, cg.clip.y0, cg.dst_y}));
  const int x1 = static_cast<int>(std::min<std::int64_t>(
      {VesaPlanarVideo::kWidth, static_cast<std::int64_t>(cg.clip.x1) + 1,
       static_cast<std::int64_t>(cg.dst_x) + cg.w}));
  const int y1 = static_cast<int>(std::min<std::int64_t>(
      {VesaPlanarVideo::kHeight, static_cast<std::int64_t>(cg.clip.y1) + 1,
       static_cast<std::int64_t>(cg.dst_y) + cg.h}));

  const int tex_stride = tex.hd_w;
  for (int ly = y0; ly < y1; ++ly) {
    const int local_ly = ly - cg.dst_y;
    for (int lx = x0; lx < x1; ++lx) {
      const std::size_t lidx =
          static_cast<std::size_t>(ly) * VesaPlanarVideo::kWidth +
          static_cast<std::size_t>(lx);
      if (hd_region_id_[lidx] != cg.region_id) {
        continue;
      }
      const int local_lx = lx - cg.dst_x;
      hd_ref_pal_id_[lidx] = new_pal_id;
      for (int sy = 0; sy < kHdScale; ++sy) {
        const std::uint32_t* srow =
            tex.argb.data() +
            static_cast<std::size_t>(local_ly * kHdScale + sy) *
                static_cast<std::size_t>(tex_stride) +
            static_cast<std::size_t>(local_lx * kHdScale);
        std::uint32_t* drow =
            hd_canvas_argb_.data() +
            static_cast<std::size_t>(ly * kHdScale + sy) *
                static_cast<std::size_t>(kCanvasW) +
            static_cast<std::size_t>(lx * kHdScale);
        std::memcpy(drow, srow,
                    static_cast<std::size_t>(kHdScale) * sizeof(std::uint32_t));
      }
    }
  }
  cg.cur_pal_id = new_pal_id;

  if (x0 < x1 && y0 < y1) {
    mark_hd_tiles_dirty(x0, y0, x1, y1);
    hd_dirty_rects_.push_back(UpscaleDirtyRect{
        .has_changes = true, .x = x0, .y = y0, .w = x1 - x0, .h = y1 - y0});
  }
}

HdPackManager::SavedHdPatch HdPackManager::capture_patch_snapshot(
    int x, int y, int w, int h, GrpClipRect clip,
    const VesaPlanarVideo& video) const {
  SavedHdPatch patch{};
  if (w <= 0 || h <= 0 || active_region_count_ == 0u) {
    return patch;
  }
  if (x < 0 || y < 0 || x < clip.x0 || y < clip.y0 ||
      static_cast<std::int64_t>(x) + w > std::min<std::int64_t>(
                                             VesaPlanarVideo::kWidth,
                                             static_cast<std::int64_t>(clip.x1) + 1) ||
      static_cast<std::int64_t>(y) + h > std::min<std::int64_t>(
                                             VesaPlanarVideo::kHeight,
                                             static_cast<std::int64_t>(clip.y1) + 1)) {
    return patch;
  }

  const std::size_t n =
      static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  bool any_hd = false;
  for (int ry = 0; ry < h && !any_hd; ++ry) {
    const std::size_t row_off =
        static_cast<std::size_t>(y + ry) * VesaPlanarVideo::kWidth +
        static_cast<std::size_t>(x);
    for (int rx = 0; rx < w; ++rx) {
      if (hd_region_id_[row_off + static_cast<std::size_t>(rx)] != 0u) {
        any_hd = true;
        break;
      }
    }
  }
  if (!any_hd) {
    return patch;
  }

  patch.w = w;
  patch.h = h;
  patch.has_any_hd = true;
  patch.ref_pal_ids.resize(n);
  patch.bank_hints.resize(n);
  patch.snap_slots.resize(n);
  patch.active_mask.resize(n);
  patch.hd_pixels_4x.resize(n * static_cast<std::size_t>(kHdScale * kHdScale));

  const auto& svram = video.shadow_vram();
  for (int ry = 0; ry < h; ++ry) {
    for (int rx = 0; rx < w; ++rx) {
      const std::size_t pidx =
          static_cast<std::size_t>(ry) * static_cast<std::size_t>(w) +
          static_cast<std::size_t>(rx);
      const std::size_t lidx =
          static_cast<std::size_t>(y + ry) * VesaPlanarVideo::kWidth +
          static_cast<std::size_t>(x + rx);
      patch.snap_slots[pidx] = svram[lidx];
      patch.ref_pal_ids[pidx] = hd_ref_pal_id_[lidx];
      patch.bank_hints[pidx] = hd_bank_hint_[lidx];
      const std::uint32_t reg = hd_region_id_[lidx];
      patch.active_mask[pidx] = (reg != 0u) ? 1u : 0u;
      if (reg != 0u) {
        patch.used_mask |= region_used_mask_[reg];
      }
    }
  }

  const int hw = w * kHdScale;
  const int hh = h * kHdScale;
  for (int hy = 0; hy < hh; ++hy) {
    std::memcpy(
        patch.hd_pixels_4x.data() +
            static_cast<std::size_t>(hy) * static_cast<std::size_t>(hw),
        hd_canvas_argb_.data() +
            static_cast<std::size_t>(y * kHdScale + hy) *
                static_cast<std::size_t>(kCanvasW) +
            static_cast<std::size_t>(x * kHdScale),
        static_cast<std::size_t>(hw) * sizeof(std::uint32_t));
  }

  patch.vram_hash = hash_sprite_4bit(w, h, patch.snap_slots.data());
  return patch;
}

void HdPackManager::push_saved_patch(SavedHdPatch&& patch) {
  if (!patch.has_any_hd || patch.w <= 0 || patch.h <= 0) {
    return;
  }
  for (auto it = saved_patches_.begin(); it != saved_patches_.end(); ++it) {
    if (it->vram_hash == patch.vram_hash && it->w == patch.w &&
        it->h == patch.h) {
      saved_patches_.erase(it);
      break;
    }
  }
  saved_patches_.push_front(std::move(patch));
  while (saved_patches_.size() > kMaxSavedPatches) {
    saved_patches_.pop_back();
  }
}

bool HdPackManager::try_restore_saved_patch(int dst_x, int dst_y, int w, int h,
                                            GrpClipRect clip,
                                            std::uint64_t vram_hash,
                                            const VesaPlanarVideo& video) {
  const SavedHdPatch* match = nullptr;
  for (const auto& sp : saved_patches_) {
    if (sp.vram_hash == vram_hash && sp.w == w && sp.h == h) {
      match = &sp;
      break;
    }
  }
  if (!match) {
    return false;
  }

  const std::int64_t x0_64 = std::max<std::int64_t>({0, clip.x0, dst_x});
  const std::int64_t y0_64 = std::max<std::int64_t>({0, clip.y0, dst_y});
  const std::int64_t x1_64 = std::min<std::int64_t>(
      {VesaPlanarVideo::kWidth, static_cast<std::int64_t>(clip.x1) + 1,
       static_cast<std::int64_t>(dst_x) + w});
  const std::int64_t y1_64 = std::min<std::int64_t>(
      {VesaPlanarVideo::kHeight, static_cast<std::int64_t>(clip.y1) + 1,
       static_cast<std::int64_t>(dst_y) + h});
  if (x0_64 >= x1_64 || y0_64 >= y1_64) {
    return true;
  }

  const auto& svram = video.shadow_vram();
  const std::uint32_t new_reg = alloc_region_id();
  std::uint32_t assigned = 0u;
  mark_hd_tiles_dirty(static_cast<int>(x0_64), static_cast<int>(y0_64),
                      static_cast<int>(x1_64), static_cast<int>(y1_64));
  const int hw = w * kHdScale;

  for (int ly = static_cast<int>(y0_64); ly < static_cast<int>(y1_64); ++ly) {
    const int py = ly - dst_y;
    for (int lx = static_cast<int>(x0_64); lx < static_cast<int>(x1_64); ++lx) {
      const int px = lx - dst_x;
      const std::size_t pidx =
          static_cast<std::size_t>(py) * static_cast<std::size_t>(w) +
          static_cast<std::size_t>(px);
      const std::size_t lidx =
          static_cast<std::size_t>(ly) * VesaPlanarVideo::kWidth +
          static_cast<std::size_t>(lx);

      if (match->active_mask[pidx] == 0u) {
        if (hd_region_id_[lidx] != 0u) {
          clear_logical_pixel(lidx);
        }
        continue;
      }

      const std::uint32_t old_reg = hd_region_id_[lidx];
      if (old_reg != 0u && old_reg != new_reg) {
        if (--region_refcounts_[old_reg] == 0u) {
          free_region_ids_.push_back(old_reg);
          if (active_region_count_ > 0u) {
            --active_region_count_;
          }
        }
      }
      if (old_reg != new_reg) {
        hd_region_id_[lidx] = new_reg;
        ++assigned;
      }
      hd_ref_pal_id_[lidx] = match->ref_pal_ids[pidx];
      hd_bank_hint_[lidx] = match->bank_hints[pidx];
      hd_snap_slot_[lidx] = svram[lidx];

      for (int sy = 0; sy < kHdScale; ++sy) {
        std::memcpy(
            hd_canvas_argb_.data() +
                static_cast<std::size_t>(ly * kHdScale + sy) *
                    static_cast<std::size_t>(kCanvasW) +
                static_cast<std::size_t>(lx * kHdScale),
            match->hd_pixels_4x.data() +
                static_cast<std::size_t>(py * kHdScale + sy) *
                    static_cast<std::size_t>(hw) +
                static_cast<std::size_t>(px * kHdScale),
            static_cast<std::size_t>(kHdScale) * sizeof(std::uint32_t));
      }
    }
  }

  if (assigned > 0u) {
    region_refcounts_[new_reg] += assigned;
    region_used_mask_[new_reg] = match->used_mask;
    ++active_region_count_;
  } else {
    free_region_ids_.push_back(new_reg);
  }
  return true;
}

void HdPackManager::on_vram_read_rect(int src_x, int src_y, int w, int h,
                                      GrpClipRect clip,
                                      const VesaPlanarVideo& video) {
  sync_vram_occlusions(video);
  SavedHdPatch snap = capture_patch_snapshot(src_x, src_y, w, h, clip, video);
  push_saved_patch(std::move(snap));
}

void HdPackManager::on_vram_swap_complete(int dst_x, int dst_y, int w, int h,
                                          GrpClipRect clip,
                                          SavedHdPatch&& pre_swap_patch,
                                          const VesaPlanarVideo& video) {
  if (w <= 0 || h <= 0) {
    return;
  }

  // Inspect post-swap VRAM at (dst_x, dst_y, w, h): restore if it matches an
  // earlier SavedHdPatch or HD sprite; otherwise clear covered HD pixels.
  bool handled = false;
  if (dst_x >= 0 && dst_y >= 0 &&
      static_cast<std::int64_t>(dst_x) + w <= VesaPlanarVideo::kWidth &&
      static_cast<std::int64_t>(dst_y) + h <= VesaPlanarVideo::kHeight) {
    const std::size_t n =
        static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<std::uint8_t> post_slots(n);
    const auto& svram = video.shadow_vram();
    for (int ry = 0; ry < h; ++ry) {
      std::memcpy(post_slots.data() +
                      static_cast<std::size_t>(ry) * static_cast<std::size_t>(w),
                  svram.data() +
                      static_cast<std::size_t>(dst_y + ry) *
                          VesaPlanarVideo::kWidth +
                      static_cast<std::size_t>(dst_x),
                  static_cast<std::size_t>(w));
    }
    const std::uint64_t post_hash =
        hash_sprite_4bit(w, h, post_slots.data());
    if (try_restore_saved_patch(dst_x, dst_y, w, h, clip, post_hash, video)) {
      handled = true;
    } else {
      ResolvedTexture res{};
      if (resolve_or_load_texture(post_hash, res) && res.tex != nullptr) {
        const std::uint32_t reg_id = paint_hd_texture_clipped(
            dst_x, dst_y, w, h, clip, *res.tex, res.ref_pal_id, res.bank_hint,
            slot_mask_of(post_slots), true, video);
        if (res.bank_hint == 3u && reg_id != 0u) {
          register_dynamic_cg(reg_id, dst_x, dst_y, w, h, clip, post_hash,
                              res.ref_pal_id, post_slots);
        }
        handled = true;
      }
    }
  }

  if (!handled) {
    const int cx0 = std::max(clip.x0, dst_x);
    const int cy0 = std::max(clip.y0, dst_y);
    const int cx1 = std::min<int>(
        clip.x1 + 1,
        static_cast<int>(std::min<std::int64_t>(
            VesaPlanarVideo::kWidth, static_cast<std::int64_t>(dst_x) + w)));
    const int cy1 = std::min<int>(
        clip.y1 + 1,
        static_cast<int>(std::min<std::int64_t>(
            VesaPlanarVideo::kHeight, static_cast<std::int64_t>(dst_y) + h)));
    clear_canvas_clipped_rect(cx0, cy0, cx1, cy1);
  }

  push_saved_patch(std::move(pre_swap_patch));
}

void HdPackManager::maybe_dump_sprite(int w, int h, std::uint64_t hash,
                                      const std::uint8_t* slots,
                                      const VesaPlanarVideo& video) {
  if (!dumping_enabled_ || dump_dir_.empty() || dumped_hashes_.contains(hash)) {
    return;
  }
  dumped_hashes_.insert(hash);
  std::array<std::uint32_t, 16> pal = video.live_palette_argb();
  bool any_nonblack = false;
  for (std::uint32_t c : pal) {
    if ((c & 0x00FFFFFFu) != 0u) {
      any_nonblack = true;
      break;
    }
  }
  if (!any_nonblack) {
    pal = codecs::default_san4_master_palette();
  }
  const std::size_t pixel_count =
      static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  std::vector<std::uint32_t> rgba(pixel_count);
  for (std::size_t i = 0; i < pixel_count; ++i) {
    rgba[i] = pal[slots[i] & 0x0Fu];
  }
  save_argb_png(dump_dir_ / format_sprite_filename(w, h, hash), w, h,
                rgba.data());
}

void HdPackManager::on_planar_blit_source(
    int dst_x, int dst_y, int w, int h, int active_planes,
    std::uint8_t p3_fill_mode, std::uint8_t p3_fill_byte,
    std::uint16_t planar_rop, std::uint8_t plane_disable, GrpClipRect clip,
    std::span<const std::uint8_t> src_planar, const VesaPlanarVideo& video) {
  if (w <= 0 || h <= 0) {
    return;
  }

  const int cx0 = std::max({0, clip.x0, dst_x});
  const int cy0 = std::max({0, clip.y0, dst_y});
  const int cx1 = static_cast<int>(std::min<std::int64_t>(
      {VesaPlanarVideo::kWidth, static_cast<std::int64_t>(clip.x1) + 1,
       static_cast<std::int64_t>(dst_x) + w}));
  const int cy1 = static_cast<int>(std::min<std::int64_t>(
      {VesaPlanarVideo::kHeight, static_cast<std::int64_t>(clip.y1) + 1,
       static_cast<std::int64_t>(dst_y) + h}));
  if (cx0 >= cx1 || cy0 >= cy1) {
    return;
  }

  // Individual-plane bitmask writes or XOR/AND/OR/NOT planar blits clear
  // modified HD pixels so hardware VRAM result shows through cleanly.
  if ((plane_disable & 0x0Fu) != 0u ||
      (planar_rop != 0u && planar_rop != 4u)) {
    clear_canvas_clipped_rect(cx0, cy0, cx1, cy1);
    return;
  }

  std::vector<std::uint8_t> game_colors;
  std::vector<std::uint8_t> vga_slots;
  const bool decoded = codecs::decode_byte_interleaved(
      src_planar, w, h, active_planes, p3_fill_mode, p3_fill_byte, game_colors,
      vga_slots);

  if (planar_rop == 4u) {
    // Color-0 transparent masked sprite (`HUNIT`, `HIGRP`, `HKGRP`, `HEXGRP`,
    // `MAINCG`/`TOSHICG` overlays, `GRAPH` rings/cursors, `HWGRP`, `HEGRP`).
    if (!decoded) {
      clear_canvas_clipped_rect(cx0, cy0, cx1, cy1);
      return;
    }
    const std::size_t n = game_colors.size();
    std::vector<std::uint8_t> opaque_mask(n, 0u);
    for (std::size_t i = 0; i < n; ++i) {
      opaque_mask[i] = (game_colors[i] != 0u) ? 1u : 0u;
    }
    const std::uint64_t mhash =
        codecs::hash_masked_sprite_4bit(w, h, vga_slots.data(), opaque_mask.data());
    maybe_dump_sprite(w, h, mhash, vga_slots.data(), video);

    ResolvedTexture mres{};
    if (resolve_or_load_texture(mhash, mres) && mres.tex != nullptr) {
      paint_hd_texture_clipped(dst_x, dst_y, w, h, clip, *mres.tex,
                               mres.ref_pal_id, mres.bank_hint,
                               slot_mask_of(vga_slots, opaque_mask.data()),
                               false, video);
      return;
    }

    // Masked sprite without HD replacement: clear only non-transparent pixels!
    for (int ly = cy0; ly < cy1; ++ly) {
      const int sy = ly - dst_y;
      for (int lx = cx0; lx < cx1; ++lx) {
        const int sx = lx - dst_x;
        if (opaque_mask[static_cast<std::size_t>(sy) *
                            static_cast<std::size_t>(w) +
                        static_cast<std::size_t>(sx)] != 0u) {
          const std::size_t lidx =
              static_cast<std::size_t>(ly) * VesaPlanarVideo::kWidth +
              static_cast<std::size_t>(lx);
          if (hd_region_id_[lidx] != 0u) {
            clear_logical_pixel(lidx);
          }
        }
      }
    }
    return;
  }

  // `planar_rop == 0` (Opaque PSET):
  // If `active_planes == 3` and `p3_fill_mode == 0`, Plane 3 in VRAM was left
  // untouched by GRP600C, so prefer post-blit VRAM when fully on-screen!
  const bool unclipped_onscreen =
      (dst_x == cx0 && dst_y == cy0 && dst_x + w == cx1 && dst_y + h == cy1);
  if (decoded && (active_planes == 4 || p3_fill_mode == 1u || !unclipped_onscreen)) {
    const std::uint64_t shash = hash_sprite_4bit(w, h, vga_slots.data());
    maybe_dump_sprite(w, h, shash, vga_slots.data(), video);

    const std::uint16_t used_mask = slot_mask_of(vga_slots);
    ResolvedTexture sres{};
    if (resolve_or_load_texture(shash, sres) && sres.tex != nullptr) {
      const std::uint32_t reg_id = paint_hd_texture_clipped(
          dst_x, dst_y, w, h, clip, *sres.tex, sres.ref_pal_id, sres.bank_hint,
          used_mask, true, video);
      if (sres.bank_hint == 3u && reg_id != 0u) {
        register_dynamic_cg(reg_id, dst_x, dst_y, w, h, clip, shash,
                            sres.ref_pal_id, vga_slots);
      }
      return;
    }
    if (try_restore_saved_patch(dst_x, dst_y, w, h, clip, shash, video)) {
      return;
    }
    if (w == 16 && h == 16) {
      ResolvedTexture cell_res{};
      if (try_composite_cell_16x16(shash, vga_slots.data(), cell_res) &&
          cell_res.tex != nullptr) {
        paint_hd_texture_clipped(dst_x, dst_y, w, h, clip, *cell_res.tex,
                                 cell_res.ref_pal_id, cell_res.bank_hint,
                                 used_mask, true, video);
        return;
      }
    }
  }

  // Fallback to post-blit `shadow_vram` check (handles `active_planes == 3,
  // p3_fill_mode == 0` and custom F9 VRAM-dumped user PNGs!).
  if (unclipped_onscreen) {
    on_planar_blit_complete(dst_x, dst_y, w, h, video);
    return;
  }

  clear_canvas_clipped_rect(cx0, cy0, cx1, cy1);
}

void HdPackManager::on_planar_blit_complete(int dst_x, int dst_y, int w, int h,
                                            const VesaPlanarVideo& video) {
  sync_vram_occlusions(video);
  if (w <= 0 || h <= 0) {
    return;
  }

  const std::int64_t cx0 = std::max<std::int64_t>(0, dst_x);
  const std::int64_t cy0 = std::max<std::int64_t>(0, dst_y);
  const std::int64_t cx1 = std::min<std::int64_t>(
      VesaPlanarVideo::kWidth, static_cast<std::int64_t>(dst_x) + w);
  const std::int64_t cy1 = std::min<std::int64_t>(
      VesaPlanarVideo::kHeight, static_cast<std::int64_t>(dst_y) + h);

  if (dst_x < 0 || dst_y < 0 ||
      static_cast<std::int64_t>(dst_x) + w > VesaPlanarVideo::kWidth ||
      static_cast<std::int64_t>(dst_y) + h > VesaPlanarVideo::kHeight) {
    if (cx0 < cx1 && cy0 < cy1) {
      clear_canvas_clipped_rect(static_cast<int>(cx0), static_cast<int>(cy0),
                                static_cast<int>(cx1), static_cast<int>(cy1));
    }
    return;
  }

  const std::size_t pixel_count =
      static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  std::vector<std::uint8_t> snap(pixel_count);
  const auto& svram = video.shadow_vram();
  for (int row = 0; row < h; ++row) {
    std::memcpy(
        snap.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(w),
        svram.data() +
            static_cast<std::size_t>(dst_y + row) * VesaPlanarVideo::kWidth +
            static_cast<std::size_t>(dst_x),
        static_cast<std::size_t>(w));
  }

  const std::uint64_t hash = hash_sprite_4bit(w, h, snap.data());
  maybe_dump_sprite(w, h, hash, snap.data(), video);

  const GrpClipRect full_clip{};
  const std::uint16_t used_mask = slot_mask_of(snap);
  ResolvedTexture res{};
  if (resolve_or_load_texture(hash, res) && res.tex != nullptr) {
    const std::uint32_t reg_id = paint_hd_texture_clipped(
        dst_x, dst_y, w, h, full_clip, *res.tex, res.ref_pal_id, res.bank_hint,
        used_mask, true, video);
    if (res.bank_hint == 3u && reg_id != 0u) {
      register_dynamic_cg(reg_id, dst_x, dst_y, w, h, full_clip, hash,
                          res.ref_pal_id, snap);
    }
    return;
  }
  if (try_restore_saved_patch(dst_x, dst_y, w, h, full_clip, hash, video)) {
    return;
  }
  if (w == 16 && h == 16) {
    ResolvedTexture cell_res{};
    if (try_composite_cell_16x16(hash, snap.data(), cell_res) &&
        cell_res.tex != nullptr) {
      paint_hd_texture_clipped(dst_x, dst_y, w, h, full_clip, *cell_res.tex,
                               cell_res.ref_pal_id, cell_res.bank_hint,
                               used_mask, true, video);
      return;
    }
  }

  clear_canvas_clipped_rect(dst_x, dst_y, dst_x + w, dst_y + h);
}

void HdPackManager::on_npk_blit_complete(
    int dst_x, int dst_y, int w, int h, GrpClipRect clip,
    std::span<const std::uint8_t> npk_bitstream, const VesaPlanarVideo& video) {
  if (w <= 0 || h <= 0) {
    return;
  }
  const int cx0 = std::max({0, clip.x0, dst_x});
  const int cy0 = std::max({0, clip.y0, dst_y});
  const int cx1 = static_cast<int>(std::min<std::int64_t>(
      {VesaPlanarVideo::kWidth, static_cast<std::int64_t>(clip.x1) + 1,
       static_cast<std::int64_t>(dst_x) + w}));
  const int cy1 = static_cast<int>(std::min<std::int64_t>(
      {VesaPlanarVideo::kHeight, static_cast<std::int64_t>(clip.y1) + 1,
       static_cast<std::int64_t>(dst_y) + h}));
  if (cx0 >= cx1 || cy0 >= cy1) {
    return;
  }

  if (dst_x == cx0 && dst_y == cy0 && dst_x + w == cx1 && dst_y + h == cy1) {
    on_planar_blit_complete(dst_x, dst_y, w, h, video);
    return;
  }

  std::vector<std::uint8_t> vga_slots;
  if (codecs::decompress_npk_bitstream(npk_bitstream, w, h, vga_slots)) {
    const std::uint64_t hash = hash_sprite_4bit(w, h, vga_slots.data());
    maybe_dump_sprite(w, h, hash, vga_slots.data(), video);
    ResolvedTexture nres{};
    if (resolve_or_load_texture(hash, nres) && nres.tex != nullptr) {
      const std::uint32_t reg_id = paint_hd_texture_clipped(
          dst_x, dst_y, w, h, clip, *nres.tex, nres.ref_pal_id, nres.bank_hint,
          slot_mask_of(vga_slots), true, video);
      if (nres.bank_hint == 3u && reg_id != 0u) {
        register_dynamic_cg(reg_id, dst_x, dst_y, w, h, clip, hash,
                            nres.ref_pal_id, vga_slots);
      }
      return;
    }
  }

  clear_canvas_clipped_rect(cx0, cy0, cx1, cy1);
}

void HdPackManager::on_mono_blit_1bit(
    int dst_x, int dst_y, int w, int h, std::uint16_t mono_rop,
    std::uint8_t fg_slot, std::uint8_t bg_slot, std::uint8_t plane_disable,
    GrpClipRect clip, std::span<const std::uint8_t> src_1bit,
    const VesaPlanarVideo& video) {
  if (w <= 0 || h <= 0) {
    return;
  }
  const int cx0 = std::max({0, clip.x0, dst_x});
  const int cy0 = std::max({0, clip.y0, dst_y});
  const int cx1 = static_cast<int>(std::min<std::int64_t>(
      {VesaPlanarVideo::kWidth, static_cast<std::int64_t>(clip.x1) + 1,
       static_cast<std::int64_t>(dst_x) + w}));
  const int cy1 = static_cast<int>(std::min<std::int64_t>(
      {VesaPlanarVideo::kHeight, static_cast<std::int64_t>(clip.y1) + 1,
       static_cast<std::int64_t>(dst_y) + h}));
  if (cx0 >= cx1 || cy0 >= cy1) {
    return;
  }

  // When no HD pack is active and no HD pixels are on screen, leave classic
  // 1-bit rendering untouched so `--no-hd-pack` is 100% original.
  if (indexed_count_ == 0u && active_region_count_ == 0u) {
    return;
  }

  if ((plane_disable & 0x0Fu) != 0u ||
      (mono_rop != 0u && mono_rop != 4u && mono_rop != 5u)) {
    clear_canvas_clipped_rect(cx0, cy0, cx1, cy1);
    return;
  }

  std::vector<std::uint8_t> bits;
  if (!codecs::decode_mono_1bit(src_1bit, w, h, bits)) {
    clear_canvas_clipped_rect(cx0, cy0, cx1, cy1);
    return;
  }

  const auto& live_pal = video.live_palette_argb();
  std::uint32_t fg_argb = live_pal[fg_slot & 0x0Fu];
  std::uint32_t bg_argb = live_pal[bg_slot & 0x0Fu];
  if (mono_rop == 5u) {
    std::swap(fg_argb, bg_argb);
  }
  const bool transparent_zero = (mono_rop == 4u);

  HdTexture glyph_tex{};
  glyph_tex.base_w = w;
  glyph_tex.base_h = h;
  glyph_tex.hd_w = w * kHdScale;
  glyph_tex.hd_h = h * kHdScale;
  hd_gen::rasterize_mono_1bit_4x(bits.data(), w, h, fg_argb, bg_argb,
                                 transparent_zero, glyph_tex.argb);

  // Intern current live palette as `ref_pal_id` (when non-black) so crisp 4x
  // text also fades out smoothly to black during subsequent AH=0x16 fades!
  bool any_nonblack = false;
  for (std::uint32_t c : live_pal) {
    if ((c & 0x00FFFFFFu) != 0u) {
      any_nonblack = true;
      break;
    }
  }
  const std::uint16_t pal_id =
      any_nonblack ? intern_palette(live_pal) : kDirectUngradedPalId;

  // Transparent glyphs draw only the foreground color.
  const auto used_mask = static_cast<std::uint16_t>(
      (1u << (fg_slot & 0x0Fu)) |
      (transparent_zero ? 0u : (1u << (bg_slot & 0x0Fu))));
  paint_hd_texture_clipped(dst_x, dst_y, w, h, clip, glyph_tex, pal_id, 0u,
                           used_mask, !transparent_zero, video);
}

void HdPackManager::on_vram_copy_rect(int src_x, int src_y, int w, int h,
                                      int dst_x, int dst_y) {
  if (w <= 0 || h <= 0 || (src_x == dst_x && src_y == dst_y) ||
      active_region_count_ == 0u) {
    return;
  }

  // Clamp copy span in 64-bit math so extreme int inputs never overflow.
  std::int64_t adj_sx64 = src_x;
  std::int64_t adj_sy64 = src_y;
  std::int64_t adj_dx64 = dst_x;
  std::int64_t adj_dy64 = dst_y;
  std::int64_t adj_w64 = w;
  std::int64_t adj_h64 = h;

  if (adj_sx64 < 0) {
    adj_w64 += adj_sx64;
    adj_dx64 -= adj_sx64;
    adj_sx64 = 0;
  }
  if (adj_sy64 < 0) {
    adj_h64 += adj_sy64;
    adj_dy64 -= adj_sy64;
    adj_sy64 = 0;
  }
  if (adj_dx64 < 0) {
    adj_w64 += adj_dx64;
    adj_sx64 -= adj_dx64;
    adj_dx64 = 0;
  }
  if (adj_dy64 < 0) {
    adj_h64 += adj_dy64;
    adj_sy64 -= adj_dy64;
    adj_dy64 = 0;
  }
  if (adj_w64 <= 0 || adj_h64 <= 0 || adj_sx64 >= VesaPlanarVideo::kWidth ||
      adj_sy64 >= VesaPlanarVideo::kHeight ||
      adj_dx64 >= VesaPlanarVideo::kWidth ||
      adj_dy64 >= VesaPlanarVideo::kHeight) {
    return;
  }
  if (adj_sx64 + adj_w64 > VesaPlanarVideo::kWidth) {
    adj_w64 = VesaPlanarVideo::kWidth - adj_sx64;
  }
  if (adj_sy64 + adj_h64 > VesaPlanarVideo::kHeight) {
    adj_h64 = VesaPlanarVideo::kHeight - adj_sy64;
  }
  if (adj_dx64 + adj_w64 > VesaPlanarVideo::kWidth) {
    adj_w64 = VesaPlanarVideo::kWidth - adj_dx64;
  }
  if (adj_dy64 + adj_h64 > VesaPlanarVideo::kHeight) {
    adj_h64 = VesaPlanarVideo::kHeight - adj_dy64;
  }
  if (adj_w64 <= 0 || adj_h64 <= 0) {
    return;
  }

  const int adj_sx = static_cast<int>(adj_sx64);
  const int adj_sy = static_cast<int>(adj_sy64);
  const int adj_dx = static_cast<int>(adj_dx64);
  const int adj_dy = static_cast<int>(adj_dy64);
  const int adj_w = static_cast<int>(adj_w64);
  const int adj_h = static_cast<int>(adj_h64);

  // Remap distinct source region IDs inside the copied rectangle to fresh
  // destination region IDs so `active_overlay_count()` accurately reflects
  // copied sprites/tiles while remaining O(w * h).
  std::unordered_map<std::uint32_t, std::uint32_t> remap;
  remap.reserve(64u);
  mark_hd_tiles_dirty(adj_dx, adj_dy, adj_dx + adj_w, adj_dy + adj_h);

  const int y_start = (adj_dy > adj_sy) ? (adj_h - 1) : 0;
  const int y_end = (adj_dy > adj_sy) ? -1 : adj_h;
  const int y_step = (adj_dy > adj_sy) ? -1 : 1;

  const int x_start = (adj_dx > adj_sx) ? (adj_w - 1) : 0;
  const int x_end = (adj_dx > adj_sx) ? -1 : adj_w;
  const int x_step = (adj_dx > adj_sx) ? -1 : 1;

  for (int ry = y_start; ry != y_end; ry += y_step) {
    const int sy = adj_sy + ry;
    const int dy = adj_dy + ry;
    for (int rx = x_start; rx != x_end; rx += x_step) {
      const int sx = adj_sx + rx;
      const int dx = adj_dx + rx;
      const std::size_t s_lidx =
          static_cast<std::size_t>(sy) * VesaPlanarVideo::kWidth +
          static_cast<std::size_t>(sx);
      const std::size_t d_lidx =
          static_cast<std::size_t>(dy) * VesaPlanarVideo::kWidth +
          static_cast<std::size_t>(dx);

      const std::uint32_t s_reg = hd_region_id_[s_lidx];
      const std::uint16_t s_pal = hd_ref_pal_id_[s_lidx];
      const std::uint8_t s_bank = hd_bank_hint_[s_lidx];
      const std::uint8_t s_snap = hd_snap_slot_[s_lidx];

      std::uint32_t sub[16];
      for (int ky = 0; ky < kHdScale; ++ky) {
        const std::uint32_t* srow =
            hd_canvas_argb_.data() +
            static_cast<std::size_t>(sy * kHdScale + ky) *
                static_cast<std::size_t>(kCanvasW) +
            static_cast<std::size_t>(sx * kHdScale);
        std::memcpy(sub + ky * 4, srow, 4u * sizeof(std::uint32_t));
      }

      const std::uint32_t old_d_reg = hd_region_id_[d_lidx];
      if (old_d_reg != 0u) {
        hd_region_id_[d_lidx] = 0u;
        if (--region_refcounts_[old_d_reg] == 0u) {
          free_region_ids_.push_back(old_d_reg);
          if (active_region_count_ > 0u) {
            --active_region_count_;
          }
        }
      }

      if (s_reg != 0u) {
        std::uint32_t mapped = 0u;
        auto rit = remap.find(s_reg);
        if (rit != remap.end()) {
          mapped = rit->second;
        } else {
          mapped = alloc_region_id();
          region_used_mask_[mapped] = region_used_mask_[s_reg];
          remap[s_reg] = mapped;
          ++active_region_count_;
        }
        hd_region_id_[d_lidx] = mapped;
        ++region_refcounts_[mapped];
      }

      hd_ref_pal_id_[d_lidx] = s_pal;
      hd_bank_hint_[d_lidx] = s_bank;
      hd_snap_slot_[d_lidx] = s_snap;

      for (int ky = 0; ky < kHdScale; ++ky) {
        std::uint32_t* drow =
            hd_canvas_argb_.data() +
            static_cast<std::size_t>(dy * kHdScale + ky) *
                static_cast<std::size_t>(kCanvasW) +
            static_cast<std::size_t>(dx * kHdScale);
        std::memcpy(drow, sub + ky * 4, 4u * sizeof(std::uint32_t));
      }
    }
  }
}

void HdPackManager::clear_overlays() {
  active_dynamic_cgs_.clear();
  if (active_region_count_ == 0u) {
    saved_patches_.clear();
    return;
  }
  clear_pending_ = true;
  mark_hd_tiles_dirty(0, 0, VesaPlanarVideo::kWidth, VesaPlanarVideo::kHeight);
  std::fill(hd_canvas_argb_.begin(), hd_canvas_argb_.end(), 0u);
  std::fill(hd_ref_pal_id_.begin(), hd_ref_pal_id_.end(), kDirectUngradedPalId);
  std::fill(hd_bank_hint_.begin(), hd_bank_hint_.end(), 0u);
  std::fill(hd_snap_slot_.begin(), hd_snap_slot_.end(), 0u);
  std::fill(hd_region_id_.begin(), hd_region_id_.end(), 0u);
  region_refcounts_.assign(1u, 0u);
  region_used_mask_.assign(1u, 0u);
  free_region_ids_.clear();
  active_region_count_ = 0u;
  saved_patches_.clear();
}

std::uint16_t HdPackManager::effective_grading_mask(std::uint8_t bank_hint,
                                                    std::uint16_t used_mask) {
  const std::uint16_t bank_mask =
      (bank_hint == 1u) ? kUpperBankSlotMask : kAllSlotsMask;
  const auto m = static_cast<std::uint16_t>(used_mask & bank_mask);
  return (m != 0u) ? m : bank_mask;
}

std::uint64_t HdPackManager::grading_lut_key(std::uint16_t ref_pal_id,
                                             std::uint8_t bank_hint,
                                             std::uint16_t effective_mask) {
  return (static_cast<std::uint64_t>(ref_pal_id) << 24u) |
         (static_cast<std::uint64_t>(bank_hint) << 16u) | effective_mask;
}

void HdPackManager::build_grading_lut(std::uint16_t ref_pal_id,
                                      std::uint16_t effective_mask,
                                      const VesaPlanarVideo& video,
                                      PaletteGradingLut& lut) const {
  lut.palette_gen = video.palette_generation();
  lut.is_fading = video.is_fading();
  const bool fading = lut.is_fading;

  const auto& ref_pal = ref_palettes_[ref_pal_id];
  const auto& live_pal = video.latched_palette_argb();

  struct Anchor {
    float pr, pg, pb;
    float tr, tg, tb;
    int count;
  };
  std::array<Anchor, 16> anchors{};
  int num_anchors = 0;

  int max_diff = 0;
  for (int gc = 0; gc < 16; ++gc) {
    const std::uint8_t slot = kGrpSlotMap[static_cast<std::size_t>(gc)];
    if (((effective_mask >> (slot & 0x0Fu)) & 1u) == 0u) {
      continue;
    }
    const std::uint32_t p = ref_pal[slot];
    const std::uint32_t t = live_pal[slot];
    const int pr = static_cast<int>((p >> 16) & 0xFFu);
    const int pg = static_cast<int>((p >> 8) & 0xFFu);
    const int pb = static_cast<int>(p & 0xFFu);
    const int tr = static_cast<int>((t >> 16) & 0xFFu);
    const int tg = static_cast<int>((t >> 8) & 0xFFu);
    const int tb = static_cast<int>(t & 0xFFu);
    max_diff = std::max({max_diff, std::abs(tr - pr), std::abs(tg - pg),
                         std::abs(tb - pb)});

    bool merged = false;
    for (int ai = 0; ai < num_anchors; ++ai) {
      if (std::abs(anchors[ai].pr - static_cast<float>(pr)) < 1.0f &&
          std::abs(anchors[ai].pg - static_cast<float>(pg)) < 1.0f &&
          std::abs(anchors[ai].pb - static_cast<float>(pb)) < 1.0f) {
        anchors[ai].tr += static_cast<float>(tr);
        anchors[ai].tg += static_cast<float>(tg);
        anchors[ai].tb += static_cast<float>(tb);
        ++anchors[ai].count;
        merged = true;
        break;
      }
    }
    if (!merged) {
      anchors[num_anchors++] = Anchor{
          static_cast<float>(pr), static_cast<float>(pg),
          static_cast<float>(pb), static_cast<float>(tr),
          static_cast<float>(tg), static_cast<float>(tb),
          1,
      };
    }
  }

  // GRP600C AH=0x15 shifts 4-bit palette values by <<2 (max 243 vs 255 = diff 12).
  // When not actively fading and all slots match within 6-bit DAC rounding (<= 13),
  // preserve exact ungraded 24-bit artistic RGB!
  if (!fading && max_diff <= 13) {
    lut.is_identity = true;
    return;
  }

  lut.is_identity = false;
  float mean_pr = 0.0f, mean_pg = 0.0f, mean_pb = 0.0f;
  float mean_tr = 0.0f, mean_tg = 0.0f, mean_tb = 0.0f;
  for (int ai = 0; ai < num_anchors; ++ai) {
    const float inv = 1.0f / static_cast<float>(anchors[ai].count);
    anchors[ai].tr *= inv;
    anchors[ai].tg *= inv;
    anchors[ai].tb *= inv;
    mean_pr += anchors[ai].pr;
    mean_pg += anchors[ai].pg;
    mean_pb += anchors[ai].pb;
    mean_tr += anchors[ai].tr;
    mean_tg += anchors[ai].tg;
    mean_tb += anchors[ai].tb;
  }
  if (num_anchors > 0) {
    const float inv_n = 1.0f / static_cast<float>(num_anchors);
    mean_pr *= inv_n;
    mean_pg *= inv_n;
    mean_pb *= inv_n;
    mean_tr *= inv_n;
    mean_tg *= inv_n;
    mean_tb *= inv_n;
  }

  float var_p = 0.0f;
  float var_t = 0.0f;
  for (int ai = 0; ai < num_anchors; ++ai) {
    const float dpr = anchors[ai].pr - mean_pr;
    const float dpg = anchors[ai].pg - mean_pg;
    const float dpb = anchors[ai].pb - mean_pb;
    const float dtr = anchors[ai].tr - mean_tr;
    const float dtg = anchors[ai].tg - mean_tg;
    const float dtb = anchors[ai].tb - mean_tb;
    var_p += dpr * dpr + dpg * dpg + dpb * dpb;
    var_t += dtr * dtr + dtg * dtg + dtb * dtb;
  }
  const float detail_scale = std::clamp(
      std::sqrt(var_t) / std::max(std::sqrt(var_p), 16.0f), 0.0f, 1.25f);

  // Populate 17x17x17 smooth 3D color-grading lattice.
  for (int ir = 0; ir <= 16; ++ir) {
    const float qr = static_cast<float>(ir * 255) / 16.0f;
    for (int ig = 0; ig <= 16; ++ig) {
      const float qg = static_cast<float>(ig * 255) / 16.0f;
      for (int ib = 0; ib <= 16; ++ib) {
        const float qb = static_cast<float>(ib * 255) / 16.0f;

        float w_sum = 0.0f;
        float ph_r = 0.0f, ph_g = 0.0f, ph_b = 0.0f;
        float th_r = 0.0f, th_g = 0.0f, th_b = 0.0f;
        for (int ai = 0; ai < num_anchors; ++ai) {
          const float dr = qr - anchors[ai].pr;
          const float dg = qg - anchors[ai].pg;
          const float db = qb - anchors[ai].pb;
          const float d2 = dr * dr + dg * dg + db * db;
          const float w = 1.0f / (d2 * d2 + 64.0f);
          w_sum += w;
          ph_r += w * anchors[ai].pr;
          ph_g += w * anchors[ai].pg;
          ph_b += w * anchors[ai].pb;
          th_r += w * anchors[ai].tr;
          th_g += w * anchors[ai].tg;
          th_b += w * anchors[ai].tb;
        }

        int out_r = static_cast<int>(std::lround(qr));
        int out_g = static_cast<int>(std::lround(qg));
        int out_b = static_cast<int>(std::lround(qb));
        if (w_sum > 0.0f) {
          const float inv_w = 1.0f / w_sum;
          ph_r *= inv_w;
          ph_g *= inv_w;
          ph_b *= inv_w;
          th_r *= inv_w;
          th_g *= inv_w;
          th_b *= inv_w;

          out_r = static_cast<int>(
              std::lround(th_r + detail_scale * (qr - ph_r)));
          out_g = static_cast<int>(
              std::lround(th_g + detail_scale * (qg - ph_g)));
          out_b = static_cast<int>(
              std::lround(th_b + detail_scale * (qb - ph_b)));
        }
        const auto cr = static_cast<std::uint32_t>(std::clamp(out_r, 0, 255));
        const auto cg = static_cast<std::uint32_t>(std::clamp(out_g, 0, 255));
        const auto cb = static_cast<std::uint32_t>(std::clamp(out_b, 0, 255));
        lut.grid[static_cast<std::size_t>((ir * 17 + ig) * 17 + ib)] =
            (cr << 16) | (cg << 8) | cb;
      }
    }
  }
}

void HdPackManager::run_parallel(int begin, int end,
                                 FunctionRef<void(int, int)> fn) const {
  if (pool_ != nullptr) {
    pool_->parallel_for(begin, end, 1, fn);
  } else if (begin < end) {
    fn(begin, end);
  }
}

void HdPackManager::resolve_band_luts(int band_begin, int band_end,
                                      const VesaPlanarVideo& video) const {
  const std::uint32_t gen = video.palette_generation();
  const bool fading = video.is_fading();
  frame_luts_.clear();
  lut_build_keys_.clear();
  for (int b = band_begin; b < band_end; ++b) {
    for (const std::uint64_t key : bands_[static_cast<std::size_t>(b)].keys) {
      const auto [fit, inserted] = frame_luts_.try_emplace(key, nullptr);
      if (!inserted ||
          static_cast<std::uint16_t>(key >> 24u) >= ref_palettes_.size()) {
        continue;
      }
      const auto cit = lut_cache_.find(key);
      if (cit != lut_cache_.end() && cit->second.lut.palette_gen == gen &&
          cit->second.lut.is_fading == fading) {
        lut_lru_.splice(lut_lru_.begin(), lut_lru_, cit->second.lru_it);
        fit->second = cit->second.lut.is_identity ? nullptr : &cit->second.lut;
      } else {
        lut_build_keys_.push_back(key);
      }
    }
  }

  // Missing or stale LUTs are independent: build them in parallel into
  // scratch storage, then publish them to the cache serially.
  const auto n_build = static_cast<int>(lut_build_keys_.size());
  if (n_build > 0) {
    if (lut_build_out_.size() < lut_build_keys_.size()) {
      lut_build_out_.resize(lut_build_keys_.size());
    }
    run_parallel(0, n_build, [&](int i0, int i1) {
      for (int i = i0; i < i1; ++i) {
        const std::uint64_t key = lut_build_keys_[static_cast<std::size_t>(i)];
        build_grading_lut(static_cast<std::uint16_t>(key >> 24u),
                          static_cast<std::uint16_t>(key & 0xFFFFu), video,
                          lut_build_out_[static_cast<std::size_t>(i)]);
      }
    });
    for (std::size_t i = 0; i < lut_build_keys_.size(); ++i) {
      const std::uint64_t key = lut_build_keys_[i];
      const auto [cit, inserted] = lut_cache_.try_emplace(key);
      if (inserted) {
        lut_lru_.push_front(key);
        cit->second.lru_it = lut_lru_.begin();
      } else {
        lut_lru_.splice(lut_lru_.begin(), lut_lru_, cit->second.lru_it);
      }
      cit->second.lut = lut_build_out_[i];
      frame_luts_[key] = cit->second.lut.is_identity ? nullptr : &cit->second.lut;
    }
  }

  for (int b = band_begin; b < band_end; ++b) {
    CompositeBand& band = bands_[static_cast<std::size_t>(b)];
    band.luts.resize(band.keys.size());
    for (std::size_t i = 0; i < band.keys.size(); ++i) {
      band.luts[i] = frame_luts_.find(band.keys[i])->second;
    }
  }
}

void HdPackManager::trim_lut_cache() const {
  while (lut_cache_.size() > kMaxLutCacheEntries) {
    lut_cache_.erase(lut_lru_.back());
    lut_lru_.pop_back();
  }
}

void HdPackManager::composite_rects(std::vector<std::uint32_t>& canvas,
                                    int out_w, int out_h, int scale,
                                    const VesaPlanarVideo& video,
                                    const UpscaleDirtyRect& bounds,
                                    std::span<const UpscaleDirtyRect> rects) const {
  constexpr int kW = VesaPlanarVideo::kWidth;
  constexpr int kH = VesaPlanarVideo::kHeight;
  const int s = scale;
  const int ds = kHdScale / s;  // 1 for 4x, 2 for 2x, 4 for 1x
  const int max_ox = std::min(out_w, kW * s);
  const int max_oy = std::min(out_h, kH * s);
  const int band_rows = kCompositeBandOutputRows / s;  // logical rows
  const int ly_begin = bounds.y / s;
  const int ly_end =
      std::min(kH, (std::min(bounds.y + bounds.h, max_oy) + s - 1) / s);
  if (ly_begin >= ly_end || bounds.x >= max_ox) {
    return;
  }
  const int band_begin = ly_begin / band_rows;
  const int band_end = (ly_end + band_rows - 1) / band_rows;
  if (bands_.size() < static_cast<std::size_t>(band_end)) {
    bands_.resize(static_cast<std::size_t>(band_end));
  }
  if (band_lut_index_.empty()) {
    band_lut_index_.assign(static_cast<std::size_t>(kW) * kH, 0u);
  }

  // Calls fn(x0, y0, x1, y1) with the part of each rect inside band `b`, in
  // output pixels clipped to the logical screen. Bands cover whole logical
  // rows, so different bands never share a canvas row or a logical pixel.
  const auto for_band_rects = [&](int b, auto&& fn) {
    const int oy_b0 = b * band_rows * s;
    const int oy_b1 = std::min((b + 1) * band_rows * s, max_oy);
    for (const UpscaleDirtyRect& r : rects) {
      const int y0 = std::max(r.y, oy_b0);
      const int y1 = std::min(r.y + r.h, oy_b1);
      const int x1 = std::min(r.x + r.w, max_ox);
      if (y0 < y1 && r.x < x1) {
        fn(r.x, y0, x1, y1);
      }
    }
  };

  // Phase 1: classify every logical pixel under the rects as not drawn (0)
  // or by the 1-based index of its LUT key in the band's key list.
  const auto& lvram = video.latched_vram();
  run_parallel(band_begin, band_end, [&](int b0, int b1) {
    for (int b = b0; b < b1; ++b) {
      CompositeBand& band = bands_[static_cast<std::size_t>(b)];
      band.keys.clear();
      std::uint32_t prev_reg = 0u;
      std::uint16_t prev_pal = 0u;
      std::uint8_t prev_bank = 0u;
      std::uint16_t prev_index = 0u;
      for_band_rects(b, [&](int x0, int y0, int x1, int y1) {
        const int lx0 = x0 / s;
        const int lx1 = (x1 - 1) / s + 1;
        for (int ly = y0 / s; ly <= (y1 - 1) / s; ++ly) {
          std::size_t lidx = static_cast<std::size_t>(ly) * kW +
                             static_cast<std::size_t>(lx0);
          for (int lx = lx0; lx < lx1; ++lx, ++lidx) {
            std::uint16_t index = 0u;
            const std::uint32_t reg = hd_region_id_[lidx];
            // Secondary shadow-VRAM guard (in addition to exact vram_write8
            // bitset).
            if (reg != 0u && lvram[lidx] == hd_snap_slot_[lidx]) {
              const std::uint16_t pal = hd_ref_pal_id_[lidx];
              const std::uint8_t bank = hd_bank_hint_[lidx];
              if (prev_index != 0u && reg == prev_reg && pal == prev_pal &&
                  bank == prev_bank) {
                index = prev_index;
              } else {
                const std::uint64_t key = grading_lut_key(
                    pal, bank,
                    effective_grading_mask(bank, region_used_mask_[reg]));
                std::size_t k = 0;
                while (k < band.keys.size() && band.keys[k] != key) {
                  ++k;
                }
                if (k == band.keys.size()) {
                  band.keys.push_back(key);
                }
                index = static_cast<std::uint16_t>(k + 1u);
                prev_reg = reg;
                prev_pal = pal;
                prev_bank = bank;
                prev_index = index;
              }
            }
            band_lut_index_[lidx] = index;
          }
        }
      });
    }
  });

  // Phase 2 (serial): resolve every key to an immutable LUT.
  resolve_band_luts(band_begin, band_end, video);

  // Phase 3: blend runs of equally graded pixels with the SIMD row kernels.
  // Workers write disjoint canvas rows and only read shared state.
  std::uint32_t* const out = canvas.data();
  const std::uint32_t* const hd = hd_canvas_argb_.data();
  run_parallel(band_begin, band_end, [&](int b0, int b1) {
    for (int b = b0; b < b1; ++b) {
      const CompositeBand& band = bands_[static_cast<std::size_t>(b)];
      if (band.keys.empty()) {
        continue;
      }
      for_band_rects(b, [&](int x0, int y0, int x1, int y1) {
        const int lx0 = x0 / s;
        const int lx1 = (x1 - 1) / s + 1;
        for (int oy = y0; oy < y1; ++oy) {
          const int ly = oy / s;
          const int hy = ly * kHdScale + (oy - ly * s) * ds;
          std::uint32_t* const dst_row =
              out + static_cast<std::size_t>(oy) * static_cast<std::size_t>(out_w);
          const std::uint32_t* const hd_row =
              hd + static_cast<std::size_t>(hy) * static_cast<std::size_t>(kCanvasW);
          const std::uint16_t* const index_row =
              band_lut_index_.data() + static_cast<std::size_t>(ly) * kW;
          int lx = lx0;
          while (lx < lx1) {
            const std::uint16_t index = index_row[lx];
            if (index == 0u) {
              ++lx;
              continue;
            }
            int run_end = lx + 1;
            while (run_end < lx1 && index_row[run_end] == index) {
              ++run_end;
            }
            const int ox0 = std::max(lx * s, x0);
            const int n = std::min(run_end * s, x1) - ox0;
            const PaletteGradingLut* lut = band.luts[index - 1u];
            const simd::GradeFn grade{&grade_with_grid,
                                      lut != nullptr ? &lut->grid : nullptr};
            const simd::GradeFn* grade_ptr = lut != nullptr ? &grade : nullptr;
            if (ds == 1) {
              simd::blend_row_4x(dst_row + ox0, hd_row + ox0, n, grade_ptr);
            } else {
              simd::blend_row_box(dst_row + ox0, hd_row + ox0 * ds, kCanvasW,
                                  ds, n, grade_ptr);
            }
            lx = run_end;
          }
        }
      });
    }
  });
}

UpscaleDirtyRect HdPackManager::composite_overlays(
    std::vector<std::uint32_t>& canvas, int out_w, int out_h, int scale,
    const VesaPlanarVideo& video, std::span<const UpscaleDirtyRect> dirty_rects,
    std::vector<UpscaleDirtyRect>& out_rects) const {
  if (!std::exchange(frame_prepared_, false)) {
    update_hd_state(video);
  }
  if (clear_pending_) {
    clear_pending_ = false;
    hd_dirty_rects_.push_back(UpscaleDirtyRect{.has_changes = true,
                                               .x = 0,
                                               .y = 0,
                                               .w = VesaPlanarVideo::kWidth,
                                               .h = VesaPlanarVideo::kHeight});
  }
  const bool can_composite =
      active_region_count_ > 0u && (scale == 1 || scale == 2 || scale == 4) &&
      out_w > 0 && out_h > 0 &&
      canvas.size() >=
          static_cast<std::size_t>(out_w) * static_cast<std::size_t>(out_h);

  // Copied first so `dirty_rects` may alias `out_rects`.
  rect_scratch_.assign(dirty_rects.begin(), dirty_rects.end());
  if (scale > 0) {
    for (const UpscaleDirtyRect& r : hd_dirty_rects_) {
      rect_scratch_.push_back(UpscaleDirtyRect{.has_changes = true,
                                               .x = r.x * scale,
                                               .y = r.y * scale,
                                               .w = r.w * scale,
                                               .h = r.h * scale});
    }
  }
  const UpscaleDirtyRect bounds =
      normalize_dirty_rects(rect_scratch_, out_w, out_h, out_rects);
  if (can_composite && bounds.has_changes) {
    composite_rects(canvas, out_w, out_h, scale, video, bounds, out_rects);
    trim_lut_cache();
  }
  return bounds;
}

void HdPackManager::composite_overlays(std::vector<std::uint32_t>& canvas,
                                       int out_w, int out_h, int scale,
                                       const VesaPlanarVideo& video,
                                       UpscaleDirtyRect& inout_dirty_rect) const {
  const std::span<const UpscaleDirtyRect> rects =
      inout_dirty_rect.has_changes
          ? std::span<const UpscaleDirtyRect>(&inout_dirty_rect, 1u)
          : std::span<const UpscaleDirtyRect>();
  const UpscaleDirtyRect changed = composite_overlays(
      canvas, out_w, out_h, scale, video, rects, legacy_rects_);
  if (!changed.has_changes) {
    return;
  }
  if (!inout_dirty_rect.has_changes) {
    inout_dirty_rect.has_changes = true;
    inout_dirty_rect.x = changed.x;
    inout_dirty_rect.y = changed.y;
    inout_dirty_rect.w = changed.w;
    inout_dirty_rect.h = changed.h;
    return;
  }
  const int ux0 = std::min(inout_dirty_rect.x, changed.x);
  const int uy0 = std::min(inout_dirty_rect.y, changed.y);
  const int ux1 =
      std::max(inout_dirty_rect.x + inout_dirty_rect.w, changed.x + changed.w);
  const int uy1 =
      std::max(inout_dirty_rect.y + inout_dirty_rect.h, changed.y + changed.h);
  inout_dirty_rect.x = ux0;
  inout_dirty_rect.y = uy0;
  inout_dirty_rect.w = ux1 - ux0;
  inout_dirty_rect.h = uy1 - uy0;
}

}  // namespace san4
