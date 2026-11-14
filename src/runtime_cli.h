#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

#include "dos/embedded_vfs.h"
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
// `resolve_and_verify_game_dir` unless built with a standalone embedded archive
// (or an explicit `embedded_archive_override` in synthetic unit tests) and
// `--game-dir` was not passed on the command line.
int run_runtime_cli(const std::vector<std::string>& utf8_args,
                    const char* fallback_prog_name,
                    const std::filesystem::path& exe_path_override,
                    std::ostream& out_stream,
                    std::ostream& err_stream,
                    bool verify_manifest = true);

int run_runtime_cli(const std::vector<std::string>& utf8_args,
                    const char* fallback_prog_name,
                    const std::filesystem::path& exe_path_override,
                    std::ostream& out_stream,
                    std::ostream& err_stream,
                    bool verify_manifest,
                    const EmbeddedPackedArchive* embedded_archive_override);

}  // namespace san4
