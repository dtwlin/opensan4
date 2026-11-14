#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace san4 {

struct EmbeddedFileDesc {
  const char* dos_name = nullptr;  // Uppercase DOS 8.3 name (e.g. "SAN486.COM")
  std::uint32_t offset = 0;        // Byte offset inside uncompressed blob
  std::uint32_t size = 0;          // Uncompressed byte length
};

struct EmbeddedPackedArchive {
  const std::uint8_t* compressed_data = nullptr;
  std::size_t compressed_size = 0;
  std::size_t uncompressed_size = 0;
  std::uint32_t uncompressed_crc32 = 0;
  const EmbeddedFileDesc* files = nullptr;
  std::size_t file_count = 0;
};

struct EmbeddedRawFile {
  std::string dos_name;
  std::vector<std::uint8_t> bytes;
};

// Immutable, thread-safe in-memory archive of base DOS game files.
// Decompressed once at startup into a single contiguous buffer for lock-free
// zero-copy reads across the emulator lifetime.
class EmbeddedGameVfs {
 public:
  struct OwnedPackedArchive {
    std::vector<std::uint8_t> compressed_blob;
    std::vector<std::string> owned_names;
    std::vector<EmbeddedFileDesc> descs;
    EmbeddedPackedArchive view{};

    OwnedPackedArchive() = default;
    OwnedPackedArchive(const OwnedPackedArchive&) = delete;
    OwnedPackedArchive& operator=(const OwnedPackedArchive&) = delete;
    OwnedPackedArchive(OwnedPackedArchive&&) noexcept = default;
    OwnedPackedArchive& operator=(OwnedPackedArchive&&) noexcept = default;
  };

  struct EntryView {
    std::string_view dos_name;
    std::span<const std::uint8_t> data;
  };

  EmbeddedGameVfs() = default;

  // Decompresses a zlib-packed archive, verifies exact uncompressed byte count,
  // IEEE CRC-32, DOS 8.3 leaf names, and slice bounds. Returns nullptr (and
  // populates `out_error` if non-null) on any integrity failure.
  static std::shared_ptr<const EmbeddedGameVfs> from_packed(
      const EmbeddedPackedArchive& archive,
      std::string* out_error = nullptr);

  // Constructs an in-memory VFS directly from uncompressed entries (used by
  // hermetic synthetic tests).
  static std::shared_ptr<const EmbeddedGameVfs> from_raw_files(
      std::span<const EmbeddedRawFile> files,
      std::string* out_error = nullptr);

  // Compresses `files` with zlib into a self-contained `OwnedPackedArchive`
  // for end-to-end synthetic testing of `from_packed`.
  static bool pack_raw_files(std::span<const EmbeddedRawFile> files,
                             OwnedPackedArchive& out,
                             std::string* out_error = nullptr);

  bool empty() const noexcept { return entries_.empty(); }
  std::size_t file_count() const noexcept { return entries_.size(); }
  std::size_t total_bytes() const noexcept { return blob_.size(); }

  bool contains_file(std::string_view norm_relpath) const noexcept;
  std::span<const std::uint8_t> find_file(
      std::string_view norm_relpath) const noexcept;
  std::vector<EntryView> entries() const;

 private:
  struct Slice {
    std::string dos_name;
    std::size_t offset = 0;
    std::size_t size = 0;
  };

  std::vector<std::uint8_t> blob_;
  std::vector<Slice> entries_;
  std::unordered_map<std::string, std::size_t> index_;
};

// Returns the compile-time embedded 43-file SAN4 archive when built with
// `-DSAN4_EMBED_GAME_DIR=<DIR>`, or nullptr in standard BYOG / CI builds.
const EmbeddedPackedArchive* builtin_embedded_game_archive() noexcept;

}  // namespace san4
