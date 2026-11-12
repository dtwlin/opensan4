#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

#include "sdl_host.h"

namespace san4 {

struct ParsedRuntimeCli {
  HostOptions opts;
  std::string cli_game_dir;
  bool explicit_scale = false;
  bool no_hd_pack = false;
  bool show_help = false;
};

void print_runtime_usage(std::ostream& out, const char* prog);

// Returns 0 on success (including --help when `out.show_help == true`), or a
// non-zero CLI exit code (2) on invalid arguments.
int parse_runtime_cli(const std::vector<std::string>& utf8_args,
                      const char* fallback_prog_name,
                      ParsedRuntimeCli& out,
                      std::ostream& out_stream,
                      std::ostream& err_stream);

// Full runtime CLI entrypoint. When `verify_manifest` is true (default in
// production `main()`), enforces Bring-Your-Own-Game 43-file verification via
// `resolve_and_verify_game_dir`. Setting `verify_manifest = false` in synthetic
// tests exercises post-verification auto-discovery (`hd_pack`, read-only
// `save_dir` fallback) and `SdlHost` execution with clean-room stub binaries.
int run_runtime_cli(const std::vector<std::string>& utf8_args,
                    const char* fallback_prog_name,
                    const std::filesystem::path& exe_path_override,
                    std::ostream& out_stream,
                    std::ostream& err_stream,
                    bool verify_manifest = true);

}  // namespace san4
