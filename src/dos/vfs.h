#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dos/embedded_vfs.h"

namespace san4 {

struct DosDirMatch {
  std::string dos_name;  // Uppercase 8.3 name (<= 12 chars + NUL)
  std::uint32_t size = 0;
  std::uint16_t dos_time = 0;
  std::uint16_t dos_date = 0x2121;  // 1996-09-01 default
  std::uint8_t attr = 0x20;         // Archive
};

class CaseInsensitiveVfs {
 public:
  CaseInsensitiveVfs() = default;
  explicit CaseInsensitiveVfs(std::filesystem::path root_dir,
                              std::filesystem::path save_dir = {});
  CaseInsensitiveVfs(std::filesystem::path root_dir,
                     std::filesystem::path save_dir,
                     std::shared_ptr<const EmbeddedGameVfs> embedded_vfs);

  void set_directories(std::filesystem::path root_dir,
                       std::filesystem::path save_dir = {});
  void set_embedded_vfs(std::shared_ptr<const EmbeddedGameVfs> embedded_vfs) {
    embedded_vfs_ = std::move(embedded_vfs);
  }

  const std::filesystem::path& root_dir() const { return root_dir_; }
  const std::filesystem::path& save_dir() const { return save_dir_; }
  const std::shared_ptr<const EmbeddedGameVfs>& embedded_vfs() const {
    return embedded_vfs_;
  }

  static bool is_rejected_device(std::string_view dos_path);
  static bool is_emm_device(std::string_view dos_path);
  static bool is_nul_or_con_device(std::string_view dos_path);
  static std::string normalize_dos_relpath(std::string_view dos_path);
  static std::string dos_basename_upper(std::string_view dos_path);

  std::optional<std::filesystem::path> resolve_for_read(
      std::string_view dos_path) const;
  std::optional<std::span<const std::uint8_t>> resolve_embedded_for_read(
      std::string_view dos_path) const;
  bool read_all_bytes(std::string_view dos_path,
                      std::vector<std::uint8_t>& out) const;
  std::optional<std::filesystem::path> resolve_for_write(
      std::string_view dos_path, bool truncate_or_create) const;
  std::optional<std::filesystem::path> resolve_existing_in_write_dir(
      std::string_view dos_path) const;

  std::vector<DosDirMatch> find_matching(std::string_view dos_pattern) const;

 private:
  static bool wildcard_match(std::string_view pattern, std::string_view name);

  std::filesystem::path root_dir_;
  std::filesystem::path save_dir_;
  std::shared_ptr<const EmbeddedGameVfs> embedded_vfs_;
};

}  // namespace san4
