#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace san4 {

struct ManifestEntry {
  const char* filename;
  std::uint64_t size;
  const char* sha256_hex;
};

struct ManifestCheckResult {
  bool ok = false;
  std::filesystem::path resolved_dir;
  std::vector<std::string> errors;
};

// Standalone clean-room SHA-256 hex digest helper.
std::string sha256_hex(std::span<const std::uint8_t> data);

// Returns the 43 immutable files of the Traditional Chinese DOS release.
std::span<const ManifestEntry> immutable_game_manifest();

// Resolves the user-supplied game directory from explicit CLI arg,
// $SAN4_GAME_DIR, <exe_dir>/san4_game_dir.txt, or $XDG_DATA_HOME/san4/game,
// and validates the 43 immutable files by size + SHA-256.
ManifestCheckResult resolve_and_verify_game_dir(
    const std::string& cli_game_dir,
    const std::filesystem::path& exe_path);

// Checks whether a loaded GRP600C.EXE image matches the expected 5-byte
// central dispatcher signature at CS:0x0018 (`E9 14 23 90 90`).
bool has_expected_grp600c_dispatch_signature(
    std::span<const std::uint8_t> grp_image);

}  // namespace san4
