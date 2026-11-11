#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "game_manifest.h"
#include "grp600c_defs.h"
#include "hd_gen/hd_art_upscaler.h"
#include "hd_gen/san4_codecs.h"
#include "hd_pack.h"
#include "util/string_util.h"
#include "util/win32_platform.h"

namespace {

using san4::ButtonIndexEntry;
using san4::HdPackManager;
using san4::MondataIndexEntry;
using san4::codecs::PackedTileDictEntry;
using san4::hd_gen::ArtAssetKind;

struct UpscaleJob {
  std::uint64_t hash = 0;  // 0 if part-only PNG (not direct manifest hash)
  bool add_to_manifest = true;
  int w = 0;
  int h = 0;
  std::uint8_t bank_hint = 0;
  ArtAssetKind kind = ArtAssetKind::CgIllustration;
  std::array<std::uint32_t, 16> slot_palette{};
  std::vector<std::uint8_t> vga_slots;
  std::vector<std::uint8_t> opaque_mask;  // empty = fully opaque
  std::string rel_path;
  std::string category;
};

bool read_binary_file(const std::filesystem::path& p,
                      std::vector<std::uint8_t>& out) {
  std::ifstream ifs(p, std::ios::binary | std::ios::ate);
  if (!ifs) {
    return false;
  }
  const auto end_pos = ifs.tellg();
  if (end_pos < 0) {
    return false;
  }
  out.resize(static_cast<std::size_t>(end_pos));
  ifs.seekg(0, std::ios::beg);
  return out.empty() ||
         static_cast<bool>(ifs.read(reinterpret_cast<char*>(out.data()),
                                    static_cast<std::streamsize>(out.size())));
}

std::filesystem::path find_case_insensitive_file(
    const std::filesystem::path& dir, std::string_view target_upper) {
  std::error_code ec;
  const auto direct = dir / target_upper;
  if (std::filesystem::is_regular_file(direct, ec)) {
    return direct;
  }
  for (const auto& ent : std::filesystem::directory_iterator(dir, ec)) {
    if (!ent.is_regular_file(ec)) continue;
    std::string fn = san4::util::path_to_utf8(ent.path().filename());
    for (char& c : fn) {
      if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    if (fn == target_upper) {
      return ent.path();
    }
  }
  return {};
}

std::string hex16(std::uint64_t v) {
  std::ostringstream oss;
  oss << std::hex << std::setw(16) << std::setfill('0') << v;
  return oss.str();
}

std::string format_palette_csv(const std::array<std::uint32_t, 16>& pal) {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < 16u; ++i) {
    if (i > 0u) oss << ',';
    oss << std::setw(6) << (pal[i] & 0x00FFFFFFu);
  }
  return oss.str();
}

void print_usage(const char* prog) {
  std::cout
      << "Usage: " << prog
      << " --game-dir <DIR> --out-dir <DIR> [--jobs <N>]\n\n"
      << "Clean-room Bring-Your-Own-Game 4x HD asset pack generator for SAN4.\n"
      << "Extracts and artistically upscales 100% of original DOS graphic archives\n"
      << "(NPK016 CGs, KAODATA & MONDATA portraits, LS11 strategic/city/battle/siege\n"
      << "tiles, masked units/duelists, and UI chrome) into 32-bit RGBA PNGs.\n\n"
      << "Options:\n"
      << "  --game-dir <DIR>   Path to user-supplied original SAN4 DOS directory\n"
      << "  --out-dir <DIR>    Destination directory for generated HD pack\n"
      << "  --jobs <1..128>    Worker thread count (default: hardware concurrency)\n"
      << "  -h, --help         Show this help message\n";
}

}  // namespace

int main(int argc, char** argv) {
  san4::win32_platform::init_process();
  const std::vector<std::string> utf8_args =
      san4::win32_platform::get_utf8_args(argc, argv);
  const char* prog_name =
      !utf8_args.empty()
          ? utf8_args[0].c_str()
          : ((argc > 0 && argv != nullptr && argv[0] != nullptr)
                 ? argv[0]
                 : "san4-hdpack-gen");

  std::string cli_game_dir;
  std::filesystem::path out_dir;
  int num_jobs =
      static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));

  for (std::size_t i = 1; i < utf8_args.size(); ++i) {
    const std::string_view arg(utf8_args[i]);
    std::string opt_val;
    auto match_val = [&](std::string_view flag) -> int {
      if (arg == flag) {
        if (i + 1u >= utf8_args.size()) return -1;
        opt_val = utf8_args[++i];
        return 1;
      }
      if (arg.size() > flag.size() && arg.starts_with(flag) &&
          arg[flag.size()] == '=') {
        opt_val = std::string(arg.substr(flag.size() + 1u));
        return opt_val.empty() ? -1 : 1;
      }
      return 0;
    };

    int m = 0;
    if (arg == "-h" || arg == "--help") {
      print_usage(prog_name);
      return 0;
    } else if ((m = match_val("--game-dir")) != 0) {
      if (m < 0) {
        std::cerr << "[hdpack-gen] Missing value for --game-dir\n";
        return 2;
      }
      cli_game_dir = opt_val;
    } else if ((m = match_val("--out-dir")) != 0) {
      if (m < 0) {
        std::cerr << "[hdpack-gen] Missing value for --out-dir\n";
        return 2;
      }
      out_dir = san4::util::utf8_to_path(opt_val);
    } else if ((m = match_val("--jobs")) != 0) {
      if (m < 0 || !san4::util::parse_int_strict(opt_val, 1, 128, num_jobs)) {
        std::cerr << "[hdpack-gen] Invalid --jobs (expected 1..128)\n";
        return 2;
      }
    } else {
      std::cerr << "[hdpack-gen] Unknown option: " << arg << "\n";
      return 2;
    }
  }

  if (out_dir.empty()) {
    std::cerr << "[hdpack-gen] --out-dir <DIR> is required (try --help)\n";
    return 2;
  }

  const auto start_tp = std::chrono::steady_clock::now();
  std::filesystem::path exe_path =
      san4::win32_platform::get_executable_path(prog_name);
  if (exe_path.empty()) {
    exe_path = std::filesystem::current_path() / "san4-hdpack-gen";
  }
  const san4::ManifestCheckResult check =
      san4::resolve_and_verify_game_dir(cli_game_dir, exe_path);
  if (!check.ok) {
    std::cerr << "[hdpack-gen] Game directory verification failed:\n";
    for (const auto& e : check.errors) {
      std::cerr << "  - " << e << "\n";
    }
    return 1;
  }
  const std::filesystem::path game_dir = check.resolved_dir;

  // 1. Extract reference palettes & MONDATA Y-tables from MAIN.EXE (read-only).
  std::vector<std::uint8_t> main_exe;
  if (!read_binary_file(find_case_insensitive_file(game_dir, "MAIN.EXE"),
                        main_exe) ||
      main_exe.size() < 0x4A160u) {
    std::cerr << "[hdpack-gen] Failed to read MAIN.EXE\n";
    return 1;
  }

  const auto pal_master = san4::codecs::decode_brg48_palette(
      std::span<const std::uint8_t>(main_exe.data() + 0x47732u, 48u));
  const auto pal_spring_map = san4::codecs::apply_brg_subpalette(
      pal_master, std::span<const std::uint8_t>(main_exe.data() + 0x478C2u, 24u),
      0, 8);
  const auto pal_spring_city = san4::codecs::apply_brg_subpalette(
      pal_master, std::span<const std::uint8_t>(main_exe.data() + 0x47922u, 24u),
      0, 8);
  const auto pal_battle0 = san4::codecs::apply_brg_subpalette(
      pal_master, std::span<const std::uint8_t>(main_exe.data() + 0x39C24u, 24u),
      0, 8);

  std::array<int, 16> mondata_eye_y = san4::codecs::kDefaultMondataEyeY;
  std::array<int, 16> mondata_mouth_y = san4::codecs::kDefaultMondataMouthY;
  for (std::size_t i = 0; i < 16u; ++i) {
    mondata_eye_y[i] = static_cast<int>(main_exe[0x4A13Au + i]);
    mondata_mouth_y[i] = static_cast<int>(main_exe[0x4A14Au + i]);
  }

  std::vector<UpscaleJob> jobs;
  jobs.reserve(4500u);
  std::unordered_map<std::uint64_t, std::size_t> seen_hashes;
  seen_hashes.reserve(4500u);
  std::unordered_map<std::string, std::size_t> archive_counts;

  std::vector<MondataIndexEntry> mondata_index;
  std::vector<ButtonIndexEntry> button_index;
  std::vector<PackedTileDictEntry> tile_dict;
  std::unordered_map<std::uint64_t, bool> seen_tile_dict_hashes;

  auto enqueue_manifest_job = [&](UpscaleJob&& job) -> std::uint64_t {
    ++archive_counts[job.category];
    if (seen_hashes.contains(job.hash)) {
      return job.hash;
    }
    const std::uint64_t h = job.hash;
    seen_hashes[h] = jobs.size();
    jobs.push_back(std::move(job));
    return h;
  };

  auto add_tile_dict_entry = [&](std::uint64_t hd_hash, std::uint8_t group,
                                 bool is_overlay,
                                 const std::vector<std::uint8_t>& game_colors,
                                 const std::vector<std::uint8_t>& vga_slots) {
    if (game_colors.size() != 256u || vga_slots.size() != 256u) return;
    const std::uint64_t dedup_key =
        hd_hash ^ (is_overlay ? 0x9E3779B97F4A7C15ull : 0ull);
    if (seen_tile_dict_hashes.contains(dedup_key)) return;
    seen_tile_dict_hashes[dedup_key] = true;

    PackedTileDictEntry pe{};
    pe.hd_hash = hd_hash;
    pe.archive_group = group;
    pe.is_overlay = is_overlay ? 1u : 0u;
    int nz = 0;
    for (int i = 0; i < 128; ++i) {
      pe.packed_slots[i] = static_cast<std::uint8_t>(
          (vga_slots[static_cast<std::size_t>(i * 2)] & 0x0Fu) |
          ((vga_slots[static_cast<std::size_t>(i * 2 + 1)] & 0x0Fu) << 4u));
    }
    for (int i = 0; i < 256; ++i) {
      if (game_colors[static_cast<std::size_t>(i)] != 0u) {
        pe.zero_mask[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        ++nz;
      }
    }
    if (nz >= 256) {
      pe.non_zero_count = 255u;
      pe.full_zero_256 = 1u;
    } else {
      pe.non_zero_count = static_cast<std::uint8_t>(nz);
      pe.full_zero_256 = 0u;
    }
    tile_dict.push_back(pe);
  };

  auto emit_16x16_tile_variants =
      [&](std::string_view category, std::string_view subdir, int idx,
          const std::vector<std::uint8_t>& gc,
          const std::vector<std::uint8_t>& slots,
          const std::array<std::uint32_t, 16>& pal, std::uint8_t dict_group,
          bool prefer_base_dict, bool also_emit_masked) {
        const std::uint64_t op_hash =
            HdPackManager::hash_sprite_4bit(16, 16, slots.data());
        UpscaleJob op_job{};
        op_job.hash = op_hash;
        op_job.w = 16;
        op_job.h = 16;
        op_job.bank_hint = 0u;
        op_job.kind = prefer_base_dict ? ArtAssetKind::TerrainTile
                                       : ArtAssetKind::MaskedSprite;
        op_job.slot_palette = pal;
        op_job.vga_slots = slots;
        op_job.rel_path =
            std::string(subdir) + "/tile_" + std::to_string(idx) + "_" +
            hex16(op_hash) + ".png";
        op_job.category = std::string(category);
        enqueue_manifest_job(std::move(op_job));

        int nz = 0;
        std::vector<std::uint8_t> mask(256u, 0u);
        for (std::size_t i = 0; i < 256u; ++i) {
          if (gc[i] != 0u) {
            mask[i] = 1u;
            ++nz;
          }
        }

        if (prefer_base_dict) {
          add_tile_dict_entry(op_hash, dict_group, false, gc, slots);
        }

        if (also_emit_masked && nz > 0 && nz < 256) {
          const std::uint64_t m_hash = san4::codecs::hash_masked_sprite_4bit(
              16, 16, slots.data(), mask.data());
          UpscaleJob m_job{};
          m_job.hash = m_hash;
          m_job.w = 16;
          m_job.h = 16;
          m_job.bank_hint = 0u;
          m_job.kind = ArtAssetKind::MaskedSprite;
          m_job.slot_palette = pal;
          m_job.vga_slots = slots;
          m_job.opaque_mask = mask;
          m_job.rel_path =
              std::string(subdir) + "/mask_" + std::to_string(idx) + "_" +
              hex16(m_hash) + ".png";
          m_job.category = std::string(category) + "_masked";
          enqueue_manifest_job(std::move(m_job));
          add_tile_dict_entry(m_hash, dict_group, true, gc, slots);
        }
      };

  // 2. KAODATA.S4: 340 historical officer portraits (64x80 -> 256x320).
  {
    std::vector<std::uint8_t> raw;
    if (!read_binary_file(find_case_insensitive_file(game_dir, "KAODATA.S4"),
                          raw)) {
      std::cerr << "[hdpack-gen] Failed to read KAODATA.S4\n";
      return 1;
    }
    std::vector<san4::codecs::KaodataPortrait> portraits;
    if (!san4::codecs::decode_kaodata_archive(raw, portraits)) {
      std::cerr << "[hdpack-gen] Failed to decode KAODATA.S4\n";
      return 1;
    }
    for (auto& kp : portraits) {
      const std::uint64_t h =
          HdPackManager::hash_sprite_4bit(kp.w, kp.h, kp.vga_slots.data());
      std::ostringstream rel;
      rel << "portraits/kao_" << std::setw(3) << std::setfill('0') << kp.index
          << "_" << hex16(h) << ".png";
      UpscaleJob job{};
      job.hash = h;
      job.w = kp.w;
      job.h = kp.h;
      job.bank_hint = 1u;
      job.kind = ArtAssetKind::Portrait;
      job.slot_palette = pal_master;
      job.vga_slots = std::move(kp.vga_slots);
      job.rel_path = rel.str();
      job.category = "KAODATA.S4";
      enqueue_manifest_job(std::move(job));
    }
  }

  // 3. MONDATA.S4: 16 templates x 20 parts = 320 HD parts + 8,192 index entries.
  {
    std::vector<std::uint8_t> raw;
    if (!read_binary_file(find_case_insensitive_file(game_dir, "MONDATA.S4"),
                          raw)) {
      std::cerr << "[hdpack-gen] Failed to read MONDATA.S4\n";
      return 1;
    }
    std::vector<san4::codecs::MondataTemplate> tmpls;
    if (!san4::codecs::decode_mondata_archive(raw, mondata_eye_y,
                                              mondata_mouth_y, tmpls)) {
      std::cerr << "[hdpack-gen] Failed to decode MONDATA.S4\n";
      return 1;
    }
    for (const auto& t : tmpls) {
      auto add_part = [&](const char* kind_str, int idx,
                          const san4::codecs::MondataPart& p, bool masked) {
        std::ostringstream rel;
        rel << "mondata/t" << std::setw(2) << std::setfill('0') << t.template_id
            << "_" << kind_str << "_" << idx << ".png";
        UpscaleJob job{};
        job.add_to_manifest = false;
        job.w = p.w;
        job.h = p.h;
        job.bank_hint = 1u;
        job.kind = ArtAssetKind::Portrait;
        job.slot_palette = pal_master;
        job.vga_slots = p.vga_slots;
        if (masked) {
          job.opaque_mask = p.mask;
        }
        job.rel_path = rel.str();
        job.category = "MONDATA.S4_parts";
        ++archive_counts[job.category];
        jobs.push_back(std::move(job));
      };
      for (int i = 0; i < 4; ++i) add_part("top", i, t.tops[i], false);
      for (int i = 0; i < 4; ++i) add_part("bot", i, t.bots[i], false);
      for (int i = 0; i < 8; ++i) add_part("eye", i, t.eyes[i], true);
      for (int i = 0; i < 4; ++i) add_part("mouth", i, t.mouths[i], true);

      std::vector<std::uint8_t> comp_slots;
      for (int ti = 0; ti < 4; ++ti) {
        for (int bi = 0; bi < 4; ++bi) {
          for (int ei = 0; ei < 8; ++ei) {
            for (int mi = 0; mi < 4; ++mi) {
              san4::codecs::composite_mondata_4bit(t, ti, bi, ei, mi,
                                                   comp_slots);
              MondataIndexEntry me{};
              me.hash64 =
                  HdPackManager::hash_sprite_4bit(64, 80, comp_slots.data());
              me.tmpl = static_cast<std::uint8_t>(t.template_id);
              me.top = static_cast<std::uint8_t>(ti);
              me.bot = static_cast<std::uint8_t>(bi);
              me.eye = static_cast<std::uint8_t>(ei);
              me.mouth = static_cast<std::uint8_t>(mi);
              me.eye_y = static_cast<std::uint8_t>(t.eye_y);
              me.mouth_y = static_cast<std::uint8_t>(t.mouth_y);
              mondata_index.push_back(me);
              ++archive_counts["MONDATA.S4_composites"];
            }
          }
        }
      }
    }
  }

  // 4. NPK016 / NPK16\0 CG containers (290 CGs across 10 containers).
  std::array<std::uint32_t, 16> pal_duel = pal_battle0;
  {
    static constexpr std::array<std::string_view, 10> kNpkFiles = {
        "OPENDAT.S4", "ENDDAT.S4",  "PACKGRP.S4", "PACKGRP2.S4", "PACKGRP3.S4",
        "HIBACK.S4",  "LOGO.S4",    "REVGRP.S4",  "HEGRP.S4",    "PREGRP.S4",
    };
    for (std::string_view fn : kNpkFiles) {
      std::vector<std::uint8_t> raw;
      if (!read_binary_file(find_case_insensitive_file(game_dir, fn), raw)) {
        std::cerr << "[hdpack-gen] Failed to read " << fn << "\n";
        return 1;
      }
      std::vector<san4::codecs::NpkImage> imgs;
      san4::codecs::scan_and_decode_npk_images(raw, imgs);

      // Extract embedded 48-byte [B, R, G] chapter/scene palette blocks located
      // before NPK headers in OPENDAT.S4, ENDDAT.S4, and PREGRP.S4.
      struct ScenePalBlock {
        std::size_t offset = 0;
        std::array<std::uint32_t, 16> pal{};
      };
      std::vector<ScenePalBlock> scene_pals;
      if (fn == "OPENDAT.S4" || fn == "ENDDAT.S4" || fn == "PREGRP.S4") {
        for (const auto& im : imgs) {
          std::size_t k = 0;
          std::vector<ScenePalBlock> rev_blocks;
          while (im.header_offset >= (k + 1u) * 48u) {
            const std::size_t off = im.header_offset - (k + 1u) * 48u;
            const std::span<const std::uint8_t> blk(raw.data() + off, 48u);
            bool valid = true;
            int nonzero = 0;
            for (std::uint8_t b : blk) {
              if (b > 15u) {
                valid = false;
                break;
              }
              if (b != 0u) ++nonzero;
            }
            if (!valid || nonzero < 6) {
              break;
            }
            const auto decoded_pal = san4::codecs::decode_brg48_palette(blk);
            std::uint32_t max_ch = 0u;
            for (std::uint32_t c : decoded_pal) {
              max_ch = std::max({max_ch, (c >> 16) & 0xFFu, (c >> 8) & 0xFFu,
                                 c & 0xFFu});
            }
            if (max_ch >= 64u) {
              rev_blocks.push_back(ScenePalBlock{off, decoded_pal});
            }
            ++k;
          }
          for (auto rit = rev_blocks.rbegin(); rit != rev_blocks.rend(); ++rit) {
            scene_pals.push_back(*rit);
          }
        }
      }

      std::string stem(fn.substr(0u, fn.find('.')));
      for (char& c : stem) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      }
      for (std::size_t idx = 0; idx < imgs.size(); ++idx) {
        auto& im = imgs[idx];
        const std::uint64_t h =
            HdPackManager::hash_sprite_4bit(im.w, im.h, im.vga_slots.data());
        std::ostringstream rel;
        rel << "cg/" << stem << "_" << std::setw(3) << std::setfill('0') << idx
            << "_" << im.w << "x" << im.h << "_" << hex16(h) << ".png";

        std::uint8_t bank_hint = 0u;
        std::array<std::uint32_t, 16> chosen_pal = im.slot_palette;
        if (im.has_default_pc98_palette) {
          bank_hint = 3u;  // Dynamic-palette NPK upgraded on live scene palette
          if (!scene_pals.empty()) {
            chosen_pal = scene_pals.front().pal;
            for (const auto& sp : scene_pals) {
              if (sp.offset <= im.header_offset) {
                chosen_pal = sp.pal;
              } else {
                break;
              }
            }
          } else if (fn == "HIBACK.S4" || fn == "HEGRP.S4") {
            chosen_pal = pal_battle0;
          } else {
            chosen_pal = pal_master;
          }
        }

        UpscaleJob job{};
        job.hash = h;
        job.w = im.w;
        job.h = im.h;
        job.bank_hint = bank_hint;
        job.kind = ArtAssetKind::CgIllustration;
        job.slot_palette = chosen_pal;
        job.vga_slots = std::move(im.vga_slots);
        job.rel_path = rel.str();
        job.category = std::string(fn) + "_NPK";
        enqueue_manifest_job(std::move(job));
      }
    }
  }

  // 5. MAINCG.S4 (Strategic Map 16x16 tiles).
  {
    std::vector<std::uint8_t> raw;
    std::vector<std::vector<std::uint8_t>> recs;
    if (!read_binary_file(find_case_insensitive_file(game_dir, "MAINCG.S4"),
                          raw) ||
        !san4::codecs::decompress_ls11_archive(raw, recs) || recs.empty()) {
      std::cerr << "[hdpack-gen] Failed to decode MAINCG.S4\n";
      return 1;
    }
    const auto& r0 = recs[0];
    std::vector<std::uint8_t> gc, slots;
    // 0x0000..0x9D7F: 315 base terrain tiles
    for (int i = 0; i < 315 && static_cast<std::size_t>((i + 1) * 128) <= r0.size();
         ++i) {
      if (san4::codecs::decode_plane_sequential_4p(
              std::span<const std::uint8_t>(r0.data() + i * 128, 128u), 16, 16,
              gc, slots)) {
        emit_16x16_tile_variants("MAINCG.S4", "tiles/maincg", i, gc, slots,
                                 pal_spring_map, 0u, true, true);
      }
    }
    // 0x9EC0..0xFEBF: 192 overlay & tail 16x16 tiles (cities, mountains, gates, icons)
    for (int i = 0;
         0x9EC0u + static_cast<std::size_t>((i + 1) * 128) <= r0.size(); ++i) {
      const std::size_t off = 0x9EC0u + static_cast<std::size_t>(i) * 128u;
      if (san4::codecs::decode_plane_sequential_4p(
              std::span<const std::uint8_t>(r0.data() + off, 128u), 16, 16, gc,
              slots)) {
        emit_16x16_tile_variants("MAINCG.S4", "tiles/maincg", 315 + i, gc,
                                 slots, pal_spring_map, 0u, i >= 157, true);
      }
    }
  }

  // 6. TOSHICG.S4 (City View 16x16 tiles: 375 tiles).
  {
    std::vector<std::uint8_t> raw;
    std::vector<std::vector<std::uint8_t>> recs;
    if (!read_binary_file(find_case_insensitive_file(game_dir, "TOSHICG.S4"),
                          raw) ||
        !san4::codecs::decompress_ls11_archive(raw, recs) || recs.empty()) {
      std::cerr << "[hdpack-gen] Failed to decode TOSHICG.S4\n";
      return 1;
    }
    const auto& r0 = recs[0];
    std::vector<std::uint8_t> gc, slots;
    for (int i = 0;
         static_cast<std::size_t>((i + 1) * 128) <= r0.size(); ++i) {
      if (san4::codecs::decode_plane_sequential_4p(
              std::span<const std::uint8_t>(r0.data() + i * 128, 128u), 16, 16,
              gc, slots)) {
        const bool is_base = (i >= 110 && i < 192);
        emit_16x16_tile_variants("TOSHICG.S4", "tiles/toshicg", i, gc, slots,
                                 pal_spring_city, 1u, is_base, true);
      }
    }
  }

  // 7. HEXGRP.S4 (Field Battle Hex 16x16 tiles: 395 tiles).
  {
    std::vector<std::uint8_t> raw;
    std::vector<std::vector<std::uint8_t>> recs;
    if (!read_binary_file(find_case_insensitive_file(game_dir, "HEXGRP.S4"),
                          raw) ||
        !san4::codecs::decompress_ls11_archive(raw, recs) || recs.empty()) {
      std::cerr << "[hdpack-gen] Failed to decode HEXGRP.S4\n";
      return 1;
    }
    const auto& r0 = recs[0];
    std::vector<std::uint8_t> gc, slots;
    for (int i = 0;
         static_cast<std::size_t>((i + 1) * 128) <= r0.size(); ++i) {
      if (san4::codecs::decode_plane_sequential_4p(
              std::span<const std::uint8_t>(r0.data() + i * 128, 128u), 16, 16,
              gc, slots)) {
        const bool is_base = (i < 154);
        emit_16x16_tile_variants("HEXGRP.S4", "tiles/hexgrp", i, gc, slots,
                                 pal_battle0, 2u, is_base, true);
      }
    }
  }

  // 8. HKGRP.S4 (Siege Battle 16x16 tiles: 8 LS11 records = 362 tiles).
  {
    std::vector<std::uint8_t> raw;
    std::vector<std::vector<std::uint8_t>> recs;
    if (!read_binary_file(find_case_insensitive_file(game_dir, "HKGRP.S4"),
                          raw) ||
        !san4::codecs::decompress_ls11_archive(raw, recs)) {
      std::cerr << "[hdpack-gen] Failed to decode HKGRP.S4\n";
      return 1;
    }
    std::vector<std::uint8_t> gc, slots;
    int global_idx = 0;
    for (std::size_t ri = 0; ri < recs.size(); ++ri) {
      const auto& rec = recs[ri];
      const bool is_base_rec = (ri == 0u || ri == 1u || ri == 2u || ri == 6u);
      for (int i = 0; static_cast<std::size_t>((i + 1) * 128) <= rec.size();
           ++i, ++global_idx) {
        if (san4::codecs::decode_plane_sequential_4p(
                std::span<const std::uint8_t>(rec.data() + i * 128, 128u), 16,
                16, gc, slots)) {
          emit_16x16_tile_variants("HKGRP.S4", "tiles/hkgrp", global_idx, gc,
                                   slots, pal_battle0, 3u, is_base_rec, true);
        }
      }
    }
  }

  // 9. HUNIT.S4 (Battle & Siege Unit Sprites: 3 LS11 records = 309 x 16x16 + 2 x 8x8).
  {
    std::vector<std::uint8_t> raw;
    std::vector<std::vector<std::uint8_t>> recs;
    if (!read_binary_file(find_case_insensitive_file(game_dir, "HUNIT.S4"),
                          raw) ||
        !san4::codecs::decompress_ls11_archive(raw, recs) || recs.size() < 3u) {
      std::cerr << "[hdpack-gen] Failed to decode HUNIT.S4\n";
      return 1;
    }
    std::vector<std::uint8_t> gc, slots;
    int uidx = 0;
    for (std::size_t ri = 0; ri < 3u; ++ri) {
      const auto& rec = recs[ri];
      const std::size_t tile_limit = (ri == 2u) ? (136u * 128u) : rec.size();
      for (std::size_t off = 0; off + 128u <= std::min(tile_limit, rec.size());
           off += 128u, ++uidx) {
        if (san4::codecs::decode_plane_sequential_4p(
                std::span<const std::uint8_t>(rec.data() + off, 128u), 16, 16,
                gc, slots)) {
          emit_16x16_tile_variants("HUNIT.S4", "sprites/hunit", uidx, gc, slots,
                                   pal_battle0, 4u, false, true);
        }
      }
      if (ri == 2u && rec.size() >= 0x4440u) {
        for (int pi = 0; pi < 2; ++pi) {
          const std::size_t off = 0x4400u + static_cast<std::size_t>(pi) * 32u;
          if (san4::codecs::decode_byte_interleaved(
                  std::span<const std::uint8_t>(rec.data() + off, 32u), 8, 8, 4,
                  0u, 0u, gc, slots)) {
            std::vector<std::uint8_t> mask(64u, 0u);
            for (std::size_t k = 0; k < 64u; ++k) {
              mask[k] = (gc[k] != 0u) ? 1u : 0u;
            }
            const std::uint64_t mh = san4::codecs::hash_masked_sprite_4bit(
                8, 8, slots.data(), mask.data());
            UpscaleJob mj{};
            mj.hash = mh;
            mj.w = 8;
            mj.h = 8;
            mj.bank_hint = 0u;
            mj.kind = ArtAssetKind::MaskedSprite;
            mj.slot_palette = pal_battle0;
            mj.vga_slots = slots;
            mj.opaque_mask = mask;
            mj.rel_path = "sprites/hunit/proj_" + std::to_string(pi) + "_" +
                          hex16(mh) + ".png";
            mj.category = "HUNIT.S4";
            enqueue_manifest_job(std::move(mj));
          }
        }
      }
    }
  }

  // 10. HIGRP.S4 (Duelist 96x96 & 40x36 & 8x36 animation sprites: 14 LS11 records).
  {
    std::vector<std::uint8_t> raw;
    std::vector<std::vector<std::uint8_t>> recs;
    if (!read_binary_file(find_case_insensitive_file(game_dir, "HIGRP.S4"),
                          raw) ||
        !san4::codecs::decompress_ls11_archive(raw, recs) ||
        recs.size() < 14u) {
      std::cerr << "[hdpack-gen] Failed to decode HIGRP.S4\n";
      return 1;
    }
    std::vector<std::uint8_t> gc, slots;
    auto emit_higrp_sprite = [&](std::span<const std::uint8_t> slice, int w,
                                 int h, const std::string& prefix,
                                 bool emit_opaque, bool emit_masked) {
      if (!san4::codecs::decode_byte_interleaved(slice, w, h, 4, 0u, 0u, gc,
                                                 slots)) {
        return;
      }
      if (emit_opaque) {
        const std::uint64_t oh =
            HdPackManager::hash_sprite_4bit(w, h, slots.data());
        UpscaleJob oj{};
        oj.hash = oh;
        oj.w = w;
        oj.h = h;
        oj.bank_hint = 0u;
        oj.kind = ArtAssetKind::CgIllustration;
        oj.slot_palette = pal_duel;
        oj.vga_slots = slots;
        oj.rel_path = "sprites/higrp/" + prefix + "_op_" + hex16(oh) + ".png";
        oj.category = "HIGRP.S4";
        enqueue_manifest_job(std::move(oj));
      }
      if (emit_masked) {
        std::vector<std::uint8_t> mask(gc.size(), 0u);
        int nz = 0;
        for (std::size_t i = 0; i < gc.size(); ++i) {
          if (gc[i] != 0u) {
            mask[i] = 1u;
            ++nz;
          }
        }
        if (nz > 0) {
          const std::uint64_t mh = san4::codecs::hash_masked_sprite_4bit(
              w, h, slots.data(), mask.data());
          UpscaleJob mj{};
          mj.hash = mh;
          mj.w = w;
          mj.h = h;
          mj.bank_hint = 0u;
          mj.kind = ArtAssetKind::MaskedSprite;
          mj.slot_palette = pal_duel;
          mj.vga_slots = slots;
          mj.opaque_mask = std::move(mask);
          mj.rel_path =
              "sprites/higrp/" + prefix + "_mk_" + hex16(mh) + ".png";
          mj.category = "HIGRP.S4";
          enqueue_manifest_job(std::move(mj));
        }
      }
    };

    for (std::size_t ri = 0; ri < 13u; ++ri) {
      if (recs[ri].size() >= 4608u) {
        emit_higrp_sprite(std::span<const std::uint8_t>(recs[ri].data(), 4608u),
                          96, 96, "r" + std::to_string(ri), true, true);
      }
    }
    const auto& r13 = recs[13];
    for (int i = 0; i < 9 && static_cast<std::size_t>((i + 1) * 720) <= r13.size();
         ++i) {
      emit_higrp_sprite(
          std::span<const std::uint8_t>(r13.data() + i * 720, 720u), 40, 36,
          "r13_a" + std::to_string(i), true, true);
    }
    for (int i = 0;
         i < 3 && 0x1950u + static_cast<std::size_t>((i + 1) * 144) <= r13.size();
         ++i) {
      emit_higrp_sprite(
          std::span<const std::uint8_t>(r13.data() + 0x1950u + i * 144, 144u),
          8, 36, "r13_b" + std::to_string(i), true, true);
    }
    for (int i = 0;
         i < 39 &&
         0x1B00u + static_cast<std::size_t>((i + 1) * 720) <= r13.size();
         ++i) {
      emit_higrp_sprite(
          std::span<const std::uint8_t>(r13.data() + 0x1B00u + i * 720, 720u),
          40, 36, "r13_c" + std::to_string(i), true, true);
    }
  }

  // 11. GRAPH.S4 (UI buttons, 67 Kanji labels -> 1,407 composite buttons,
  //     plaques, season icons, rings, cursors).
  {
    std::vector<std::uint8_t> raw;
    std::vector<std::vector<std::uint8_t>> recs;
    if (!read_binary_file(find_case_insensitive_file(game_dir, "GRAPH.S4"),
                          raw) ||
        !san4::codecs::decompress_ls11_archive(raw, recs) || recs.empty() ||
        recs[0].size() < 0x5A34u) {
      std::cerr << "[hdpack-gen] Failed to decode GRAPH.S4\n";
      return 1;
    }
    const auto& r0 = recs[0];
    std::vector<std::uint8_t> gc, slots;

    // 21 x 32x32 3-plane button bases (19 at 0x0000..0x1C7F + 2 at 0x2B74..0x2E73)
    std::vector<std::vector<std::uint8_t>> base_slots_list(21u);
    for (int bi = 0; bi < 21; ++bi) {
      const std::size_t off =
          (bi < 19) ? (static_cast<std::size_t>(bi) * 384u)
                    : (0x2B74u + static_cast<std::size_t>(bi - 19) * 384u);
      if (san4::codecs::decode_byte_interleaved(
              std::span<const std::uint8_t>(r0.data() + off, 384u), 32, 32, 3,
              1u, 0xFFu, gc, slots)) {
        base_slots_list[static_cast<std::size_t>(bi)] = slots;
        const std::uint64_t bh =
            HdPackManager::hash_sprite_4bit(32, 32, slots.data());
        std::ostringstream rel;
        rel << "sprites/buttons/base_" << std::setw(2) << std::setfill('0')
            << bi << ".png";
        UpscaleJob bj{};
        bj.hash = bh;
        bj.w = 32;
        bj.h = 32;
        bj.bank_hint = 1u;
        bj.kind = ArtAssetKind::UiChrome;
        bj.slot_palette = pal_master;
        bj.vga_slots = slots;
        bj.rel_path = rel.str();
        bj.category = "GRAPH.S4";
        enqueue_manifest_job(std::move(bj));
      }
    }

    // 67 x 32x16 2-plane Kanji button labels at 0x2E74..0x4FF3
    std::vector<std::vector<std::uint8_t>> label_slots_list(67u);
    for (int li = 0; li < 67; ++li) {
      const std::size_t off = 0x2E74u + static_cast<std::size_t>(li) * 128u;
      if (san4::codecs::decode_graph_kanji_label(
              std::span<const std::uint8_t>(r0.data() + off, 128u), gc,
              slots)) {
        label_slots_list[static_cast<std::size_t>(li)] = slots;
        std::ostringstream rel;
        rel << "sprites/buttons/label_" << std::setw(2) << std::setfill('0')
            << li << ".png";
        UpscaleJob lj{};
        lj.add_to_manifest = false;
        lj.w = 32;
        lj.h = 16;
        lj.bank_hint = 1u;
        lj.kind = ArtAssetKind::UiChrome;
        lj.slot_palette = pal_master;
        lj.vga_slots = slots;
        lj.rel_path = rel.str();
        lj.category = "GRAPH.S4_labels";
        ++archive_counts[lj.category];
        jobs.push_back(std::move(lj));
      }
    }

    // Precompute all 21 * 67 = 1,407 composite button hashes (`rows 0..15` base,
    // `rows 16..31` label, matching MAIN.EXE 0x2C5EA..0x2C643).
    std::vector<std::uint8_t> comp_btn(32u * 32u);
    for (int bi = 0; bi < 21; ++bi) {
      if (base_slots_list[static_cast<std::size_t>(bi)].size() != 1024u) continue;
      for (int li = 0; li < 67; ++li) {
        if (label_slots_list[static_cast<std::size_t>(li)].size() != 512u) continue;
        std::memcpy(comp_btn.data(),
                    base_slots_list[static_cast<std::size_t>(bi)].data(), 512u);
        std::memcpy(comp_btn.data() + 512u,
                    label_slots_list[static_cast<std::size_t>(li)].data(),
                    512u);
        ButtonIndexEntry be{};
        be.hash64 = HdPackManager::hash_sprite_4bit(32, 32, comp_btn.data());
        be.base_id = static_cast<std::uint8_t>(bi);
        be.label_id = static_cast<std::uint8_t>(li);
        button_index.push_back(be);
        ++archive_counts["GRAPH.S4_button_composites"];
      }
    }

    // 5 x 3-plane UI plaques (0x1C80..0x2573) + 4 x 32x32 season icons (0x2574..0x2B73)
    struct PlaqueSpec {
      std::size_t off;
      int w;
      int h;
      const char* tag;
    };
    static constexpr std::array<PlaqueSpec, 9> kPlaques = {{
        {0x1C80u, 40, 24, "plaque0"},
        {0x1DE8u, 48, 24, "plaque1"},
        {0x1F98u, 56, 20, "plaque2"},
        {0x213Cu, 72, 20, "plaque3"},
        {0x2358u, 72, 20, "plaque4"},
        {0x2574u, 32, 32, "season0"},
        {0x26F4u, 32, 32, "season1"},
        {0x2874u, 32, 32, "season2"},
        {0x29F4u, 32, 32, "season3"},
    }};
    for (const auto& ps : kPlaques) {
      const std::size_t nbytes =
          static_cast<std::size_t>(ps.w / 8) * static_cast<std::size_t>(ps.h) * 3u;
      if (san4::codecs::decode_byte_interleaved(
              std::span<const std::uint8_t>(r0.data() + ps.off, nbytes), ps.w,
              ps.h, 3, 1u, 0xFFu, gc, slots)) {
        const std::uint64_t h =
            HdPackManager::hash_sprite_4bit(ps.w, ps.h, slots.data());
        UpscaleJob pj{};
        pj.hash = h;
        pj.w = ps.w;
        pj.h = ps.h;
        pj.bank_hint = 1u;
        pj.kind = ArtAssetKind::UiChrome;
        pj.slot_palette = pal_master;
        pj.vga_slots = slots;
        pj.rel_path = std::string("sprites/ui/") + ps.tag + "_" + hex16(h) + ".png";
        pj.category = "GRAPH.S4";
        enqueue_manifest_job(std::move(pj));
      }
    }

    // 2 x 24x24 4-plane rings (0x55F4..0x5833) + 4 x 16x16 4-plane cursors (0x5834..0x5A33)
    for (int ri = 0; ri < 2; ++ri) {
      const std::size_t off = 0x55F4u + static_cast<std::size_t>(ri) * 288u;
      if (san4::codecs::decode_byte_interleaved(
              std::span<const std::uint8_t>(r0.data() + off, 288u), 24, 24, 4,
              0u, 0u, gc, slots)) {
        std::vector<std::uint8_t> mask(gc.size(), 0u);
        for (std::size_t k = 0; k < gc.size(); ++k) {
          mask[k] = (gc[k] != 0u) ? 1u : 0u;
        }
        const std::uint64_t mh = san4::codecs::hash_masked_sprite_4bit(
            24, 24, slots.data(), mask.data());
        UpscaleJob rj{};
        rj.hash = mh;
        rj.w = 24;
        rj.h = 24;
        rj.bank_hint = 0u;
        rj.kind = ArtAssetKind::UiChrome;
        rj.slot_palette = pal_master;
        rj.vga_slots = slots;
        rj.opaque_mask = std::move(mask);
        rj.rel_path =
            "sprites/ui/ring" + std::to_string(ri) + "_" + hex16(mh) + ".png";
        rj.category = "GRAPH.S4";
        enqueue_manifest_job(std::move(rj));
      }
    }
    for (int ci = 0; ci < 4; ++ci) {
      const std::size_t off = 0x5834u + static_cast<std::size_t>(ci) * 128u;
      if (san4::codecs::decode_byte_interleaved(
              std::span<const std::uint8_t>(r0.data() + off, 128u), 16, 16, 4,
              0u, 0u, gc, slots)) {
        emit_16x16_tile_variants("GRAPH.S4", "sprites/ui", ci, gc, slots,
                                 pal_master, 4u, false, true);
      }
    }
  }

  // 12. HWGRP.S4 (Battle weather, traps, fire/boulder 16x16 & 32x32 sprites).
  {
    std::vector<std::uint8_t> raw;
    if (!read_binary_file(find_case_insensitive_file(game_dir, "HWGRP.S4"),
                          raw) ||
        raw.size() < 0x3E00u) {
      std::cerr << "[hdpack-gen] Failed to read HWGRP.S4\n";
      return 1;
    }
    std::vector<std::uint8_t> gc, slots;
    for (int i = 0; i < 36; ++i) {
      if (san4::codecs::decode_plane_sequential_4p(
              std::span<const std::uint8_t>(raw.data() + i * 128, 128u), 16, 16,
              gc, slots)) {
        emit_16x16_tile_variants("HWGRP.S4", "sprites/hwgrp", i, gc, slots,
                                 pal_battle0, 4u, false, true);
      }
    }
    for (int i = 0; i < 22; ++i) {
      const std::size_t off = 0x1200u + static_cast<std::size_t>(i) * 512u;
      if (san4::codecs::decode_byte_interleaved(
              std::span<const std::uint8_t>(raw.data() + off, 512u), 32, 32, 4,
              0u, 0u, gc, slots)) {
        const std::uint64_t oh =
            HdPackManager::hash_sprite_4bit(32, 32, slots.data());
        UpscaleJob oj{};
        oj.hash = oh;
        oj.w = 32;
        oj.h = 32;
        oj.bank_hint = 0u;
        oj.kind = ArtAssetKind::MaskedSprite;
        oj.slot_palette = pal_battle0;
        oj.vga_slots = slots;
        oj.rel_path = "sprites/hwgrp/fx32_op_" + std::to_string(i) + "_" +
                      hex16(oh) + ".png";
        oj.category = "HWGRP.S4";
        enqueue_manifest_job(std::move(oj));

        std::vector<std::uint8_t> mask(gc.size(), 0u);
        int nz = 0;
        for (std::size_t k = 0; k < gc.size(); ++k) {
          if (gc[k] != 0u) {
            mask[k] = 1u;
            ++nz;
          }
        }
        if (nz > 0 && nz < 1024) {
          const std::uint64_t mh = san4::codecs::hash_masked_sprite_4bit(
              32, 32, slots.data(), mask.data());
          UpscaleJob mj{};
          mj.hash = mh;
          mj.w = 32;
          mj.h = 32;
          mj.bank_hint = 0u;
          mj.kind = ArtAssetKind::MaskedSprite;
          mj.slot_palette = pal_battle0;
          mj.vga_slots = slots;
          mj.opaque_mask = std::move(mask);
          mj.rel_path = "sprites/hwgrp/fx32_mk_" + std::to_string(i) + "_" +
                        hex16(mh) + ".png";
          mj.category = "HWGRP.S4";
          enqueue_manifest_job(std::move(mj));
        }
      }
    }
  }

  // 13. HEGRP.S4[0..0x225F] (Event/Battle 16x16, 24x20, and 32x32 sprites).
  {
    std::vector<std::uint8_t> raw;
    if (!read_binary_file(find_case_insensitive_file(game_dir, "HEGRP.S4"),
                          raw) ||
        raw.size() < 0x2260u) {
      std::cerr << "[hdpack-gen] Failed to read HEGRP.S4\n";
      return 1;
    }
    std::vector<std::uint8_t> gc, slots;
    for (int i = 0; i < 37; ++i) {
      if (san4::codecs::decode_plane_sequential_4p(
              std::span<const std::uint8_t>(raw.data() + i * 128, 128u), 16, 16,
              gc, slots)) {
        emit_16x16_tile_variants("HEGRP.S4_sprites", "sprites/hegrp", i, gc,
                                 slots, pal_battle0, 4u, false, true);
      }
    }
    for (int i = 0; i < 2; ++i) {
      const std::size_t off = 0x1280u + static_cast<std::size_t>(i) * 240u;
      if (san4::codecs::decode_byte_interleaved(
              std::span<const std::uint8_t>(raw.data() + off, 240u), 24, 20, 4,
              0u, 0u, gc, slots)) {
        std::vector<std::uint8_t> mask(gc.size(), 0u);
        for (std::size_t k = 0; k < gc.size(); ++k) {
          mask[k] = (gc[k] != 0u) ? 1u : 0u;
        }
        const std::uint64_t mh = san4::codecs::hash_masked_sprite_4bit(
            24, 20, slots.data(), mask.data());
        UpscaleJob mj{};
        mj.hash = mh;
        mj.w = 24;
        mj.h = 20;
        mj.bank_hint = 0u;
        mj.kind = ArtAssetKind::MaskedSprite;
        mj.slot_palette = pal_battle0;
        mj.vga_slots = slots;
        mj.opaque_mask = std::move(mask);
        mj.rel_path = "sprites/hegrp/sp24x20_" + std::to_string(i) + "_" +
                      hex16(mh) + ".png";
        mj.category = "HEGRP.S4_sprites";
        enqueue_manifest_job(std::move(mj));
      }
    }
    for (int i = 0; i < 3; ++i) {
      const std::size_t off = 0x1460u + static_cast<std::size_t>(i) * 512u;
      if (san4::codecs::decode_byte_interleaved(
              std::span<const std::uint8_t>(raw.data() + off, 512u), 32, 32, 4,
              0u, 0u, gc, slots)) {
        std::vector<std::uint8_t> mask(gc.size(), 0u);
        for (std::size_t k = 0; k < gc.size(); ++k) {
          mask[k] = (gc[k] != 0u) ? 1u : 0u;
        }
        const std::uint64_t mh = san4::codecs::hash_masked_sprite_4bit(
            32, 32, slots.data(), mask.data());
        UpscaleJob mj{};
        mj.hash = mh;
        mj.w = 32;
        mj.h = 32;
        mj.bank_hint = 0u;
        mj.kind = ArtAssetKind::MaskedSprite;
        mj.slot_palette = pal_battle0;
        mj.vga_slots = slots;
        mj.opaque_mask = std::move(mask);
        mj.rel_path = "sprites/hegrp/sp32x32_" + std::to_string(i) + "_" +
                      hex16(mh) + ".png";
        mj.category = "HEGRP.S4_sprites";
        enqueue_manifest_job(std::move(mj));
      }
    }
    for (int i = 0; i < 16; ++i) {
      const std::size_t off = 0x1A60u + static_cast<std::size_t>(i) * 128u;
      if (san4::codecs::decode_plane_sequential_4p(
              std::span<const std::uint8_t>(raw.data() + off, 128u), 16, 16, gc,
              slots)) {
        emit_16x16_tile_variants("HEGRP.S4_sprites", "sprites/hegrp", 37 + i,
                                 gc, slots, pal_battle0, 4u, false, true);
      }
    }
  }

  // Create output subdirectories upfront before launching worker threads.
  std::error_code ec;
  for (std::string_view sub :
       {"cg", "portraits", "mondata", "tiles/maincg", "tiles/toshicg",
        "tiles/hexgrp", "tiles/hkgrp", "sprites/hunit", "sprites/higrp",
        "sprites/buttons", "sprites/ui", "sprites/hwgrp", "sprites/hegrp"}) {
    std::filesystem::create_directories(out_dir / sub, ec);
  }

  std::cout << "[hdpack-gen] Queued " << jobs.size()
            << " unique HD PNG jobs (+ " << mondata_index.size()
            << " MONDATA faces + " << button_index.size()
            << " composite buttons + " << tile_dict.size()
            << " dynamic tile dictionary records) across " << num_jobs
            << " worker threads...\n";

  // 14. Parallel 4x artistic upscaling + PNG encoding across worker threads.
  std::atomic<std::size_t> next_idx{0u};
  std::atomic<bool> any_failed{false};
  std::mutex err_mu;

  auto worker_fn = [&]() {
    std::vector<std::uint32_t> argb_4x;
    while (true) {
      const std::size_t idx = next_idx.fetch_add(1u, std::memory_order_relaxed);
      if (idx >= jobs.size()) {
        break;
      }
      const UpscaleJob& job = jobs[idx];
      san4::hd_gen::upscale_art_4x_argb(
          job.vga_slots.data(),
          job.opaque_mask.empty() ? nullptr : job.opaque_mask.data(), job.w,
          job.h, job.slot_palette, job.kind, argb_4x);
      if (!HdPackManager::save_argb_png(
              out_dir / san4::util::utf8_to_path(job.rel_path), job.w * 4,
              job.h * 4, argb_4x.data())) {
        any_failed.store(true, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(err_mu);
        std::cerr << "[hdpack-gen] Failed writing PNG: " << job.rel_path << "\n";
      }
    }
  };

  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(num_jobs));
  for (int t = 0; t < num_jobs; ++t) {
    workers.emplace_back(worker_fn);
  }
  for (auto& th : workers) {
    th.join();
  }
  if (any_failed.load()) {
    return 1;
  }

  // 15. Write `hd_manifest.tsv`, `mondata_index.bin`, `button_index.bin`, and
  //     `tile_dict.bin`.
  {
    std::ofstream mofs(out_dir / "hd_manifest.tsv", std::ios::binary);
    if (!mofs) {
      std::cerr << "[hdpack-gen] Failed to write hd_manifest.tsv\n";
      return 1;
    }
    mofs << "# SAN4 4x HD Asset Pack Manifest (hash16\tbase_w\tbase_h\tbank\trel_path\tpal_rgb_csv)\n";
    std::size_t manifest_rows = 0u;
    for (const auto& job : jobs) {
      if (!job.add_to_manifest) continue;
      mofs << hex16(job.hash) << '\t' << job.w << '\t' << job.h << '\t'
           << static_cast<int>(job.bank_hint) << '\t' << job.rel_path << '\t'
           << format_palette_csv(job.slot_palette) << '\n';
      ++manifest_rows;
    }
    mofs.flush();
    if (!mofs) {
      std::cerr << "[hdpack-gen] I/O error finalizing hd_manifest.tsv\n";
      return 1;
    }
    std::cout << "[hdpack-gen] Wrote hd_manifest.tsv (" << manifest_rows
              << " direct hash entries)\n";
  }

  std::sort(mondata_index.begin(), mondata_index.end(),
            [](const MondataIndexEntry& a, const MondataIndexEntry& b) {
              return a.hash64 < b.hash64;
            });
  {
    std::ofstream ofs(out_dir / "mondata" / "mondata_index.bin",
                      std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(mondata_index.data()),
              static_cast<std::streamsize>(mondata_index.size() *
                                           sizeof(MondataIndexEntry)));
    if (!ofs) {
      std::cerr << "[hdpack-gen] Failed writing mondata_index.bin\n";
      return 1;
    }
  }

  std::sort(button_index.begin(), button_index.end(),
            [](const ButtonIndexEntry& a, const ButtonIndexEntry& b) {
              return a.hash64 < b.hash64;
            });
  {
    std::ofstream ofs(out_dir / "sprites" / "buttons" / "button_index.bin",
                      std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(button_index.data()),
              static_cast<std::streamsize>(button_index.size() *
                                           sizeof(ButtonIndexEntry)));
    if (!ofs) {
      std::cerr << "[hdpack-gen] Failed writing button_index.bin\n";
      return 1;
    }
  }

  {
    std::ofstream ofs(out_dir / "tiles" / "tile_dict.bin", std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(tile_dict.data()),
              static_cast<std::streamsize>(tile_dict.size() *
                                           sizeof(PackedTileDictEntry)));
    if (!ofs) {
      std::cerr << "[hdpack-gen] Failed writing tile_dict.bin\n";
      return 1;
    }
  }

  const auto end_tp = std::chrono::steady_clock::now();
  const double elapsed_sec =
      std::chrono::duration<double>(end_tp - start_tp).count();

  std::vector<std::pair<std::string, std::size_t>> sorted_counts(
      archive_counts.begin(), archive_counts.end());
  std::sort(sorted_counts.begin(), sorted_counts.end());
  std::cout << "[hdpack-gen] Per-archive summary:\n";
  for (const auto& [k, v] : sorted_counts) {
    std::cout << "  " << std::left << std::setw(28) << k << ": " << v << "\n";
  }
  std::cout << "[hdpack-gen] Completed in " << std::fixed << std::setprecision(2)
            << elapsed_sec << " s -> " << san4::util::path_to_utf8(out_dir)
            << "\n";
  return 0;
}
