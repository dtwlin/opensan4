#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace san4::hd_gen {

void print_hdpack_gen_usage(std::ostream& out, const char* prog);

// Runs the full 15-stage clean-room 4x RGBA HD asset extraction & artistic
// upscaling pipeline from `game_dir` into `out_dir` across `num_jobs` threads.
// Returns 0 on success, 1 on archive/I/O error.
int generate_hd_pack_from_dir(const std::filesystem::path& game_dir,
                              const std::filesystem::path& out_dir,
                              int num_jobs,
                              std::ostream& out_stream,
                              std::ostream& err_stream);

// Full CLI entrypoint for `san4-hdpack-gen`. When `verify_manifest` is true
// (default in production `main()`), enforces 43-file BYOG verification via
// `resolve_and_verify_game_dir`. Setting `verify_manifest = false` in synthetic
// tests exercises the full multi-threaded archive pipeline on synthetic inputs.
int run_hdpack_gen_cli(const std::vector<std::string>& utf8_args,
                       const char* fallback_prog_name,
                       const std::filesystem::path& exe_path_override,
                       std::ostream& out_stream,
                       std::ostream& err_stream,
                       bool verify_manifest = true);

}  // namespace san4::hd_gen
