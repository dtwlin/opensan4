#include "runtime_cli.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

#include "game_manifest.h"
#include "render_policy.h"
#include "upscaler.h"
#include "util/string_util.h"
#include "util/win32_platform.h"

namespace san4 {

void print_runtime_usage(std::ostream& out, const char* prog) {
  out << "Usage: " << (prog != nullptr ? prog : "san4-linux") << " [options]\n\n"
      << "Clean-room native Linux runtime for Romance of the Three Kingdoms IV (DOS).\n"
      << "Requires a user-supplied copy of the original DOS game files (BYOG).\n\n"
      << "Options:\n"
      << "  --game-dir <DIR>       Path to original SAN4 DOS game directory\n"
      << "  --save-dir <DIR>       Optional separate directory for SAVEDAT.S4 writes\n"
      << "  --scale <MODE>         Upscaler: nearest | sharp2x (default) | scale2x | xbr2x | xbr4x\n"
      << "                         (with an HD pack: xbr4x, or xbr2x on software renderers\n"
      << "                         with a drawable height <= 1440)\n"
      << "  --window-scale <1..8>  Initial 4:3 window multiplier (default: 2 -> 1600x1200)\n"
      << "  --speed <1|2|4>        Initial gameplay speed multiplier (default: 1)\n"
      << "  --hd-pack <DIR>        Directory of 32-bit RGBA PNG sprite replacements\n"
      << "  --no-hd-pack           Disable HD texture pack loading (classic 4-bit rendering)\n"
      << "  --dump-sprites <DIR>   Export unique 4-bit planar sprites as indexed PNGs\n"
      << "  --skip-intro           Bypass OPEN.EXE intro animation directly into MAIN.EXE\n"
      << "  --fullscreen           Start in borderless desktop fullscreen\n"
      << "  --windowed             Start in windowed mode (default)\n"
      << "  --no-mouse-warp        Disable host cursor auto-snap on DOS INT 33h AX=0004h\n"
      << "  --headless-script <S>  Run deterministic automation commands and exit\n"
      << "  --deterministic        With --headless-script: budget guest bursts by\n"
      << "                         instruction count (not wall clock) and derive DOS\n"
      << "                         time from guest ticks so screenshots replay exactly\n"
      << "  --renderer <MODE>      Present profile: auto (default) | gpu | cpu\n"
      << "  --profile-frames <TSV> Write per-frame timings to TSV and print a GRP600C\n"
      << "                         call census at exit\n"
      << "  --threads <0..64>      Worker threads for upscaling/compositing (default 0 = auto)\n"
      << "  -h, --help             Show this help message\n\n"
      << "In-Game Hotkeys:\n"
      << "  F4                     Cycle gameplay speed (1x / 2x / 4x, music stays 1.0x)\n"
      << "  Hold Tab               16x Turbo fast-forward while held\n"
      << "  F5                     Cycle upscaler (nearest / sharp2x / scale2x / xbr2x / xbr4x)\n"
      << "  F8                     Hot-reload HD PNG texture pack\n"
      << "  F9                     Toggle live sprite PNG dumping\n"
      << "  F11 / Alt+Enter        Toggle borderless fullscreen\n"
      << "  Mouse Wheel            Scroll officer/city lists (Shift+Wheel = PgUp/PgDn)\n"
      << "  Middle-Drag / WASD     Pan strategic and tactical maps\n";
}

int parse_runtime_cli(const std::vector<std::string>& utf8_args,
                      const char* fallback_prog_name,
                      ParsedRuntimeCli& out,
                      std::ostream& out_stream,
                      std::ostream& err_stream) {
  out = ParsedRuntimeCli{};
  const char* prog_name =
      !utf8_args.empty()
          ? utf8_args[0].c_str()
          : (fallback_prog_name != nullptr ? fallback_prog_name : "san4-linux");

  for (std::size_t i = 1; i < utf8_args.size(); ++i) {
    const std::string_view arg(utf8_args[i]);
    std::string opt_val;
    auto match_value_flag = [&](std::string_view flag) -> int {
      if (arg == flag) {
        if (i + 1u >= utf8_args.size()) {
          err_stream << "[san4] Missing argument for " << flag << "\n";
          return -1;
        }
        opt_val = utf8_args[++i];
        return 1;
      }
      if (arg.size() > flag.size() && arg.starts_with(flag) &&
          arg[flag.size()] == '=') {
        opt_val = std::string(arg.substr(flag.size() + 1u));
        if (opt_val.empty()) {
          err_stream << "[san4] Missing argument for " << flag << "\n";
          return -1;
        }
        return 1;
      }
      return 0;
    };

    int m = 0;
    if (arg == "-h" || arg == "--help") {
      print_runtime_usage(out_stream, prog_name);
      out.show_help = true;
      return 0;
    } else if ((m = match_value_flag("--game-dir")) != 0) {
      if (m < 0) return 2;
      out.cli_game_dir = opt_val;
    } else if ((m = match_value_flag("--save-dir")) != 0) {
      if (m < 0) return 2;
      out.opts.save_dir = util::utf8_to_path(opt_val);
    } else if ((m = match_value_flag("--scale")) != 0) {
      if (m < 0) return 2;
      if (!PixelUpscaler::parse_mode(opt_val, out.opts.upscale_mode)) {
        err_stream << "[san4] Unknown --scale mode: " << opt_val
                   << " (expected nearest, sharp2x, scale2x, xbr2x, or xbr4x)\n";
        return 2;
      }
      out.explicit_scale = true;
    } else if ((m = match_value_flag("--window-scale")) != 0) {
      if (m < 0) return 2;
      int ws = 0;
      if (!util::parse_int_strict(opt_val, 1, 8, ws)) {
        err_stream << "[san4] Invalid --window-scale: " << opt_val
                   << " (expected integer in 1..8)\n";
        return 2;
      }
      out.opts.window_scale = ws;
    } else if ((m = match_value_flag("--speed")) != 0) {
      if (m < 0) return 2;
      int sp = 0;
      if (!util::parse_int_strict(opt_val, 1, 4, sp) ||
          (sp != 1 && sp != 2 && sp != 4)) {
        err_stream << "[san4] Invalid --speed: " << opt_val
                   << " (expected 1, 2, or 4)\n";
        return 2;
      }
      out.opts.speed_multiplier = sp;
    } else if ((m = match_value_flag("--hd-pack")) != 0) {
      if (m < 0) return 2;
      out.opts.hd_pack_dir = util::utf8_to_path(opt_val);
      out.no_hd_pack = false;
    } else if (arg == "--no-hd-pack") {
      out.no_hd_pack = true;
      out.opts.hd_pack_dir.clear();
    } else if ((m = match_value_flag("--dump-sprites")) != 0) {
      if (m < 0) return 2;
      out.opts.dump_sprites_dir = util::utf8_to_path(opt_val);
    } else if (arg == "--skip-intro") {
      out.opts.skip_intro = true;
    } else if (arg == "--fullscreen") {
      out.opts.fullscreen = true;
    } else if (arg == "--windowed") {
      out.opts.fullscreen = false;
    } else if (arg == "--no-mouse-warp") {
      out.opts.mouse_warp = false;
    } else if ((m = match_value_flag("--headless-script")) != 0) {
      if (m < 0) return 2;
      out.opts.headless_script = opt_val;
    } else if (arg == "--deterministic") {
      out.opts.deterministic = true;
    } else if ((m = match_value_flag("--renderer")) != 0) {
      if (m < 0) return 2;
      if (!parse_renderer_preference(opt_val, out.opts.renderer)) {
        err_stream << "[san4] Unknown --renderer: " << opt_val
                   << " (expected auto, gpu, or cpu)\n";
        return 2;
      }
    } else if ((m = match_value_flag("--profile-frames")) != 0) {
      if (m < 0) return 2;
      out.opts.profile_frames_path = util::utf8_to_path(opt_val);
    } else if ((m = match_value_flag("--threads")) != 0) {
      if (m < 0) return 2;
      int threads = 0;
      if (!util::parse_int_strict(opt_val, 0, 64, threads)) {
        err_stream << "[san4] Invalid --threads: " << opt_val
                   << " (expected 0..64, 0 = auto)\n";
        return 2;
      }
      out.opts.threads = static_cast<unsigned>(threads);
    } else {
      err_stream << "[san4] Unknown option: " << arg << " (try --help)\n";
      return 2;
    }
  }

  if (out.opts.deterministic && out.opts.headless_script.empty()) {
    err_stream << "[san4] --deterministic requires --headless-script\n";
    return 2;
  }
  return 0;
}

int run_runtime_cli(const std::vector<std::string>& utf8_args,
                    const char* fallback_prog_name,
                    const std::filesystem::path& exe_path_override,
                    std::ostream& out_stream,
                    std::ostream& err_stream,
                    bool verify_manifest) {
  return run_runtime_cli(utf8_args, fallback_prog_name, exe_path_override,
                         out_stream, err_stream, verify_manifest,
                         builtin_embedded_game_archive());
}

int run_runtime_cli(const std::vector<std::string>& utf8_args,
                    const char* fallback_prog_name,
                    const std::filesystem::path& exe_path_override,
                    std::ostream& out_stream,
                    std::ostream& err_stream,
                    bool verify_manifest,
                    const EmbeddedPackedArchive* embedded_archive_override) {
  ParsedRuntimeCli parsed;
  const int parse_rc = parse_runtime_cli(utf8_args, fallback_prog_name, parsed,
                                         out_stream, err_stream);
  if (parse_rc != 0 || parsed.show_help) {
    return parse_rc;
  }

  const char* prog_name =
      !utf8_args.empty()
          ? utf8_args[0].c_str()
          : (fallback_prog_name != nullptr ? fallback_prog_name : "san4-linux");

  std::filesystem::path exe_path =
      !exe_path_override.empty()
          ? exe_path_override
          : win32_platform::get_executable_path(prog_name);
  if (exe_path.empty()) {
    exe_path = std::filesystem::current_path() / "san4-linux";
  }

  std::filesystem::path resolved_game_dir;
  if (parsed.cli_game_dir.empty() && embedded_archive_override != nullptr) {
    std::string embed_err;
    auto embedded =
        EmbeddedGameVfs::from_packed(*embedded_archive_override, &embed_err);
    if (!embedded) {
      const std::string err_msg =
          std::string("[san4] Embedded game archive verification failed: ") +
          embed_err + "\n";
      err_stream << err_msg;
      if (parsed.opts.headless_script.empty()) {
        win32_platform::show_error_dialog("SAN4 Native Runtime", err_msg);
      }
      return 1;
    }
    parsed.opts.embedded_vfs = std::move(embedded);
  } else if (verify_manifest) {
    const ManifestCheckResult check =
        resolve_and_verify_game_dir(parsed.cli_game_dir, exe_path);
    if (!check.ok) {
      std::ostringstream oss;
      oss << "[san4] Game data verification failed (Bring-Your-Own-Game required).\n";
      if (!check.resolved_dir.empty()) {
        oss << "       Checked directory: "
            << util::path_to_utf8(check.resolved_dir) << "\n";
      }
      const std::size_t show_n = std::min<std::size_t>(check.errors.size(), 8u);
      for (std::size_t i = 0; i < show_n; ++i) {
        oss << "       - " << check.errors[i] << "\n";
      }
      if (check.errors.size() > show_n) {
        oss << "       ... and " << (check.errors.size() - show_n)
            << " more issue(s).\n";
      }
      oss << "\nPlace your copy of the Traditional Chinese DOS SAN4 directory in\n"
          << "a 'SAN4' folder next to this executable, or run:\n"
          << "  " << prog_name << " --game-dir /path/to/SAN4\n"
          << "or set the SAN4_GAME_DIR environment variable.\n";
      const std::string err_msg = oss.str();
      err_stream << err_msg;
      if (parsed.opts.headless_script.empty()) {
        win32_platform::show_error_dialog("SAN4 Native Runtime", err_msg);
      }
      return 1;
    }
    resolved_game_dir = check.resolved_dir;
  } else {
    resolved_game_dir = util::utf8_to_path(parsed.cli_game_dir);
  }

  struct ScopedHeadlessSaveDir {
    std::filesystem::path path;
    ~ScopedHeadlessSaveDir() {
      if (!path.empty()) {
        std::error_code rm_ec;
        std::filesystem::remove_all(path, rm_ec);
      }
    }
  } scoped_headless_saves;

  HostOptions& opts = parsed.opts;
  std::error_code ec;
  const std::filesystem::path exe_dir =
      std::filesystem::weakly_canonical(exe_path, ec).parent_path();
  if (!parsed.no_hd_pack && opts.hd_pack_dir.empty() &&
      opts.headless_script.empty() && !exe_dir.empty()) {
    const std::filesystem::path cand1 = exe_dir / "hd_pack";
    const std::filesystem::path cand2 = exe_dir.parent_path() / "hd_pack";
    if (std::filesystem::is_regular_file(cand1 / "hd_manifest.tsv", ec)) {
      opts.hd_pack_dir = cand1;
    } else if (!exe_dir.parent_path().empty() &&
               exe_dir.parent_path() != exe_dir &&
               std::filesystem::is_regular_file(cand2 / "hd_manifest.tsv", ec)) {
      opts.hd_pack_dir = cand2;
    }
  }
  if (opts.embedded_vfs != nullptr && resolved_game_dir.empty()) {
    if (opts.save_dir.empty()) {
      if (opts.headless_script.empty()) {
        std::filesystem::path user_saves =
            win32_platform::select_user_save_dir(exe_dir);
        if (user_saves.empty()) {
          const std::string err_msg =
              "[san4] Failed to create writable user save directory for "
              "standalone embedded mode. Pass --save-dir <DIR>.\n";
          err_stream << err_msg;
          win32_platform::show_error_dialog("SAN4 Native Runtime", err_msg);
          return 1;
        }
        opts.save_dir = std::move(user_saves);
      } else {
        const std::filesystem::path tmp_root =
            std::filesystem::temp_directory_path(ec);
        if (!ec && !tmp_root.empty()) {
          const auto tick = static_cast<unsigned long long>(
              std::chrono::steady_clock::now().time_since_epoch().count());
          for (unsigned attempt = 0; attempt < 32u; ++attempt) {
            std::filesystem::path cand =
                tmp_root / ("san4_hl_saves_" + std::to_string(tick) + "_" +
                            std::to_string(attempt));
            if (!std::filesystem::exists(cand, ec) &&
                std::filesystem::create_directories(cand, ec) && !ec) {
              scoped_headless_saves.path = cand;
              opts.save_dir = std::move(cand);
              break;
            }
          }
        }
      }
    } else {
      std::filesystem::create_directories(opts.save_dir, ec);
    }
  } else if (opts.save_dir.empty() && opts.headless_script.empty() &&
             !win32_platform::is_game_dir_writable(resolved_game_dir)) {
    std::filesystem::path fallback_saves =
        win32_platform::select_user_save_dir(exe_dir);
    if (!fallback_saves.empty()) {
      opts.save_dir = std::move(fallback_saves);
    }
  }

  if (parsed.no_hd_pack) {
    opts.hd_pack_dir.clear();
  } else if (!opts.hd_pack_dir.empty() && !parsed.explicit_scale) {
    // Interactive runs refine this per renderer profile once SDL is up.
    opts.upscale_mode = UpscaleMode::Xbr4x;
  }
  opts.explicit_scale = parsed.explicit_scale;
  opts.game_dir = resolved_game_dir;

  // Heap-allocate SdlHost (~1.65 MB due to embedded guest RAM/VRAM buffers)
  // so main()'s stack frame stays small on platforms with 1 MB default stacks.
  try {
    auto host = std::make_unique<SdlHost>(opts);
    return host->run();
  } catch (const std::exception& ex) {
    const std::string msg =
        std::string("[san4] Fatal error: ") + ex.what() + "\n";
    err_stream << msg;
    if (opts.headless_script.empty()) {
      win32_platform::show_error_dialog("SAN4 Native Runtime - Fatal Error",
                                        msg);
    }
  } catch (...) {
    const std::string msg = "[san4] Fatal error: unknown exception\n";
    err_stream << msg;
    if (opts.headless_script.empty()) {
      win32_platform::show_error_dialog("SAN4 Native Runtime - Fatal Error",
                                        msg);
    }
  }
  return 1;
}

}  // namespace san4
