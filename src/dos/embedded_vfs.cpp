#include "dos/embedded_vfs.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>

#include "util/string_util.h"

namespace san4 {
namespace {

constexpr std::size_t kMaxArchiveBytes = 256u * 1024u * 1024u;

void set_err(std::string* out_error, std::string msg) {
  if (out_error != nullptr) {
    *out_error = std::move(msg);
  }
}

bool normalize_valid_dos_leaf(std::string_view raw, std::string& out_upper) {
  out_upper.clear();
  if (raw.empty() || raw.size() > 12u || raw == "." || raw == ".." ||
      raw.back() == '.' || raw.back() == ' ') {
    return false;
  }
  std::size_t dot_count = 0u;
  std::size_t dot_pos = std::string_view::npos;
  for (std::size_t i = 0; i < raw.size(); ++i) {
    const char ch = raw[i];
    const auto uc = static_cast<unsigned char>(ch);
    if (uc <= 0x20u || uc >= 0x7Fu || ch == '/' || ch == '\\' || ch == ':' ||
        ch == '*' || ch == '?' || ch == '<' || ch == '>' || ch == '"' ||
        ch == '|') {
      return false;
    }
    if (ch == '.') {
      ++dot_count;
      dot_pos = i;
    }
  }
  if (dot_count > 1u) {
    return false;
  }
  const std::size_t stem_len =
      (dot_pos == std::string_view::npos) ? raw.size() : dot_pos;
  const std::size_t ext_len =
      (dot_pos == std::string_view::npos) ? 0u : (raw.size() - dot_pos - 1u);
  if (stem_len == 0u || stem_len > 8u || ext_len > 3u) {
    return false;
  }
  const std::string stem = util::extract_win32_device_stem_upper(raw);
  if (util::is_win32_reserved_device_stem(raw) || stem == "MS$KANJI" ||
      stem == "$DISP" || stem == "$FONT" || stem == "EMMXXXX0") {
    return false;
  }
  out_upper = util::ascii_upper(raw);
  return true;
}

std::uint32_t compute_crc32(std::span<const std::uint8_t> data) noexcept {
  uLong crc = ::crc32(0L, Z_NULL, 0);
  std::size_t offset = 0u;
  while (offset < data.size()) {
    const std::size_t chunk =
        std::min<std::size_t>(data.size() - offset, 64u * 1024u * 1024u);
    crc = ::crc32(crc, data.data() + offset, static_cast<uInt>(chunk));
    offset += chunk;
  }
  return static_cast<std::uint32_t>(crc & 0xFFFFFFFFu);
}

}  // namespace

std::shared_ptr<const EmbeddedGameVfs> EmbeddedGameVfs::from_packed(
    const EmbeddedPackedArchive& archive, std::string* out_error) {
  if (archive.file_count == 0u || archive.files == nullptr) {
    set_err(out_error, "Embedded archive contains zero file entries.");
    return nullptr;
  }
  if (archive.compressed_data == nullptr || archive.compressed_size == 0u ||
      archive.compressed_size > kMaxArchiveBytes ||
      archive.uncompressed_size > kMaxArchiveBytes) {
    set_err(out_error, "Embedded archive size metadata is invalid.");
    return nullptr;
  }

  auto vfs = std::make_shared<EmbeddedGameVfs>();
  vfs->blob_.resize(archive.uncompressed_size);

  uLongf dest_len = static_cast<uLongf>(archive.uncompressed_size);
  const uLong src_len = static_cast<uLong>(archive.compressed_size);
  std::uint8_t dummy = 0u;
  Bytef* dst_ptr = vfs->blob_.empty() ? &dummy : vfs->blob_.data();
  const int zrc = ::uncompress(dst_ptr, &dest_len, archive.compressed_data,
                               src_len);
  if (zrc != Z_OK ||
      static_cast<std::size_t>(dest_len) != archive.uncompressed_size) {
    set_err(out_error, "Failed to decompress embedded game archive.");
    return nullptr;
  }

  const std::uint32_t actual_crc = compute_crc32(vfs->blob_);
  if (actual_crc != archive.uncompressed_crc32) {
    set_err(out_error, "Embedded game archive CRC-32 integrity check failed.");
    return nullptr;
  }

  vfs->entries_.reserve(archive.file_count);
  vfs->index_.reserve(archive.file_count);
  for (std::size_t i = 0; i < archive.file_count; ++i) {
    const EmbeddedFileDesc& desc = archive.files[i];
    if (desc.dos_name == nullptr) {
      set_err(out_error, "Embedded file entry has null filename.");
      return nullptr;
    }
    std::string up_name;
    if (!normalize_valid_dos_leaf(desc.dos_name, up_name)) {
      set_err(out_error,
              std::string("Invalid DOS 8.3 filename in embedded archive: ") +
                  desc.dos_name);
      return nullptr;
    }
    const auto off = static_cast<std::size_t>(desc.offset);
    const auto sz = static_cast<std::size_t>(desc.size);
    if (off > vfs->blob_.size() || sz > (vfs->blob_.size() - off)) {
      set_err(out_error,
              "Embedded file slice exceeds uncompressed archive bounds: " +
                  up_name);
      return nullptr;
    }
    if (vfs->index_.contains(up_name)) {
      set_err(out_error,
              "Duplicate filename in embedded archive: " + up_name);
      return nullptr;
    }
    const std::size_t idx = vfs->entries_.size();
    vfs->entries_.push_back(Slice{
        .dos_name = up_name,
        .offset = off,
        .size = sz,
    });
    vfs->index_.emplace(std::move(up_name), idx);
  }

  return vfs;
}

std::shared_ptr<const EmbeddedGameVfs> EmbeddedGameVfs::from_raw_files(
    std::span<const EmbeddedRawFile> files, std::string* out_error) {
  if (files.empty()) {
    set_err(out_error, "Cannot construct EmbeddedGameVfs from empty file list.");
    return nullptr;
  }

  std::size_t total = 0u;
  for (const auto& f : files) {
    if (f.bytes.size() > kMaxArchiveBytes ||
        total > kMaxArchiveBytes - f.bytes.size()) {
      set_err(out_error, "Raw embedded archive exceeds maximum size.");
      return nullptr;
    }
    total += f.bytes.size();
  }

  auto vfs = std::make_shared<EmbeddedGameVfs>();
  vfs->blob_.resize(total);
  vfs->entries_.reserve(files.size());
  vfs->index_.reserve(files.size());

  std::size_t cursor = 0u;
  for (const auto& f : files) {
    std::string up_name;
    if (!normalize_valid_dos_leaf(f.dos_name, up_name)) {
      set_err(out_error, "Invalid DOS 8.3 filename: " + f.dos_name);
      return nullptr;
    }
    if (vfs->index_.contains(up_name)) {
      set_err(out_error, "Duplicate DOS filename: " + up_name);
      return nullptr;
    }
    if (!f.bytes.empty()) {
      std::memcpy(vfs->blob_.data() + cursor, f.bytes.data(), f.bytes.size());
    }
    const std::size_t idx = vfs->entries_.size();
    vfs->entries_.push_back(Slice{
        .dos_name = up_name,
        .offset = cursor,
        .size = f.bytes.size(),
    });
    vfs->index_.emplace(std::move(up_name), idx);
    cursor += f.bytes.size();
  }

  return vfs;
}

bool EmbeddedGameVfs::pack_raw_files(std::span<const EmbeddedRawFile> files,
                                     OwnedPackedArchive& out,
                                     std::string* out_error) {
  out = OwnedPackedArchive{};
  if (files.empty()) {
    set_err(out_error, "Cannot pack empty file list.");
    return false;
  }

  std::size_t total = 0u;
  for (const auto& f : files) {
    if (f.bytes.size() > kMaxArchiveBytes ||
        total > kMaxArchiveBytes - f.bytes.size()) {
      set_err(out_error, "Archive exceeds maximum size.");
      return false;
    }
    total += f.bytes.size();
  }

  std::vector<std::uint8_t> raw_blob(total);
  out.owned_names.reserve(files.size());
  out.descs.reserve(files.size());

  std::size_t cursor = 0u;
  std::unordered_map<std::string, bool> seen;
  for (const auto& f : files) {
    std::string up_name;
    if (!normalize_valid_dos_leaf(f.dos_name, up_name)) {
      set_err(out_error, "Invalid DOS 8.3 filename: " + f.dos_name);
      return false;
    }
    if (seen.contains(up_name)) {
      set_err(out_error, "Duplicate DOS filename: " + up_name);
      return false;
    }
    seen.emplace(up_name, true);
    if (!f.bytes.empty()) {
      std::memcpy(raw_blob.data() + cursor, f.bytes.data(), f.bytes.size());
    }
    out.owned_names.push_back(std::move(up_name));
    out.descs.push_back(EmbeddedFileDesc{
        .dos_name = out.owned_names.back().c_str(),
        .offset = static_cast<std::uint32_t>(cursor),
        .size = static_cast<std::uint32_t>(f.bytes.size()),
    });
    cursor += f.bytes.size();
  }

  uLongf comp_bound = ::compressBound(static_cast<uLong>(raw_blob.size()));
  out.compressed_blob.resize(static_cast<std::size_t>(comp_bound));
  std::uint8_t dummy = 0u;
  const Bytef* src_ptr = raw_blob.empty() ? &dummy : raw_blob.data();
  const int zrc = ::compress2(out.compressed_blob.data(), &comp_bound, src_ptr,
                              static_cast<uLong>(raw_blob.size()),
                              Z_BEST_COMPRESSION);
  if (zrc != Z_OK) {
    set_err(out_error, "zlib compress2 failed.");
    return false;
  }
  out.compressed_blob.resize(static_cast<std::size_t>(comp_bound));

  out.view = EmbeddedPackedArchive{
      .compressed_data = out.compressed_blob.data(),
      .compressed_size = out.compressed_blob.size(),
      .uncompressed_size = raw_blob.size(),
      .uncompressed_crc32 = compute_crc32(raw_blob),
      .files = out.descs.data(),
      .file_count = out.descs.size(),
  };
  return true;
}

bool EmbeddedGameVfs::contains_file(std::string_view norm_relpath) const noexcept {
  if (norm_relpath.empty() ||
      norm_relpath.find_first_of("/\\") != std::string_view::npos) {
    return false;
  }
  return index_.contains(util::ascii_upper(norm_relpath));
}

std::span<const std::uint8_t> EmbeddedGameVfs::find_file(
    std::string_view norm_relpath) const noexcept {
  if (norm_relpath.empty() ||
      norm_relpath.find_first_of("/\\") != std::string_view::npos) {
    return {};
  }
  const auto it = index_.find(util::ascii_upper(norm_relpath));
  if (it == index_.end()) {
    return {};
  }
  const Slice& s = entries_[it->second];
  return std::span<const std::uint8_t>(blob_.data() + s.offset, s.size);
}

std::vector<EmbeddedGameVfs::EntryView> EmbeddedGameVfs::entries() const {
  std::vector<EntryView> out;
  out.reserve(entries_.size());
  for (const Slice& s : entries_) {
    out.push_back(EntryView{
        .dos_name = s.dos_name,
        .data = std::span<const std::uint8_t>(blob_.data() + s.offset, s.size),
    });
  }
  return out;
}

}  // namespace san4
