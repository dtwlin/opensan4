#include "dos/vfs.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <unordered_map>

#include "util/string_util.h"

namespace san4 {
namespace {

std::string stem_device_name(std::string_view dos_path) {
  while (!dos_path.empty() &&
         (dos_path.back() == '\0' || dos_path.back() == '\r' ||
          dos_path.back() == '\n' || dos_path.back() == ' ')) {
    dos_path.remove_suffix(1u);
  }
  if (dos_path.size() >= 2u &&
      std::isalpha(static_cast<unsigned char>(dos_path[0])) &&
      dos_path[1] == ':') {
    dos_path.remove_prefix(2u);
  }
  const std::size_t sep = dos_path.find_last_of("/\\");
  const std::string_view leaf =
      (sep == std::string_view::npos) ? dos_path : dos_path.substr(sep + 1u);
  return util::extract_win32_device_stem_upper(leaf);
}

bool has_invalid_dos_chars(std::string_view sv) {
  for (char ch : sv) {
    const auto uc = static_cast<unsigned char>(ch);
    if (uc <= 0x1Fu || uc == 0x7Fu || ch == ':' || ch == '*' || ch == '?' ||
        ch == '<' || ch == '>' || ch == '"' || ch == '|') {
      return true;
    }
  }
  return false;
}

bool has_reserved_segment(std::string_view rel) {
  std::size_t pos = 0u;
  while (pos < rel.size()) {
    const auto next = rel.find('/', pos);
    const std::string_view part =
        (next == std::string_view::npos) ? rel.substr(pos)
                                         : rel.substr(pos, next - pos);
    if (!part.empty()) {
      const std::string stem = util::extract_win32_device_stem_upper(part);
      if (util::is_win32_reserved_device_stem(part) || stem == "MS$KANJI" ||
          stem == "$DISP" || stem == "$FONT" || stem == "EMMXXXX0") {
        return true;
      }
    }
    pos = (next == std::string_view::npos) ? rel.size() : (next + 1u);
  }
  return false;
}

bool validate_write_leaf(std::string_view dos_path, std::string& out_rel) {
  out_rel.clear();
  std::string_view raw = dos_path;
  while (!raw.empty() && raw.back() == '\0') {
    raw.remove_suffix(1u);
  }
  if (raw.empty() || raw.back() == '.' || raw.back() == ' ') {
    return false;
  }
  for (char ch : raw) {
    const auto uc = static_cast<unsigned char>(ch);
    if (uc <= 0x1Fu || uc == 0x7Fu || ch == '*' || ch == '?' || ch == '<' ||
        ch == '>' || ch == '"' || ch == '|') {
      return false;
    }
  }

  std::string rel = CaseInsensitiveVfs::normalize_dos_relpath(raw);
  if (rel.empty() || rel == "." || rel == ".." || rel.back() == '.' ||
      rel.back() == ' ' || rel.find('/') != std::string::npos ||
      rel.find("..") != std::string::npos || has_invalid_dos_chars(rel) ||
      has_reserved_segment(rel)) {
    return false;
  }
  out_rel = std::move(rel);
  return true;
}

bool contains_parent_ref(std::string_view rel) {
  std::size_t pos = 0;
  while (pos < rel.size()) {
    const auto next = rel.find('/', pos);
    const std::string_view part =
        (next == std::string_view::npos) ? rel.substr(pos)
                                         : rel.substr(pos, next - pos);
    if (part == "..") {
      return true;
    }
    pos = (next == std::string_view::npos) ? rel.size() : (next + 1);
  }
  return false;
}

std::optional<std::filesystem::path> lookup_case_insensitive(
    const std::filesystem::path& base, std::string_view rel) {
  if (base.empty() || rel.empty()) {
    return std::nullopt;
  }
  std::error_code ec;
  std::filesystem::path cur = base;
  std::size_t pos = 0;
  while (pos < rel.size()) {
    const auto next = rel.find('/', pos);
    const std::string_view part =
        (next == std::string_view::npos) ? rel.substr(pos)
                                         : rel.substr(pos, next - pos);
    pos = (next == std::string_view::npos) ? rel.size() : (next + 1);
    if (part.empty() || part == ".") {
      continue;
    }
    if (part == "..") {
      return std::nullopt;  // Never escape VFS root
    }
    const std::string want = util::ascii_upper(part);
    bool matched = false;
    for (const auto& ent : std::filesystem::directory_iterator(cur, ec)) {
      std::error_code sym_ec;
      if (ent.is_symlink(sym_ec) || sym_ec) {
        continue;
      }
      if (util::ascii_upper(util::path_to_utf8(ent.path().filename())) == want) {
        cur = ent.path();
        matched = true;
        break;
      }
    }
    if (!matched) {
      return std::nullopt;
    }
  }
  return cur;
}

}  // namespace

CaseInsensitiveVfs::CaseInsensitiveVfs(std::filesystem::path root_dir,
                                       std::filesystem::path save_dir) {
  set_directories(std::move(root_dir), std::move(save_dir));
}

void CaseInsensitiveVfs::set_directories(std::filesystem::path root_dir,
                                         std::filesystem::path save_dir) {
  root_dir_ = std::move(root_dir);
  save_dir_ = save_dir.empty() ? root_dir_ : std::move(save_dir);
}

std::string CaseInsensitiveVfs::normalize_dos_relpath(
    std::string_view dos_path) {
  std::string s(dos_path);
  // Trim trailing whitespace/CR/NUL
  while (!s.empty() &&
         (s.back() == '\0' || s.back() == '\r' || s.back() == '\n' ||
          s.back() == ' ')) {
    s.pop_back();
  }
  // Strip drive letter (e.g. C:\ or A:)
  if (s.size() >= 2 && std::isalpha(static_cast<unsigned char>(s[0])) &&
      s[1] == ':') {
    s = s.substr(2);
  }
  for (char& c : s) {
    const auto uc = static_cast<unsigned char>(c);
    if (uc <= 0x1Fu || uc == 0x7Fu || c == ':') {
      return {};
    }
    if (c == '\\') {
      c = '/';
    }
  }
  while (!s.empty() && s.front() == '/') {
    s.erase(s.begin());
  }
  while (s.starts_with("./")) {
    s = s.substr(2);
  }
  return s;
}

std::string CaseInsensitiveVfs::dos_basename_upper(std::string_view dos_path) {
  std::string norm = normalize_dos_relpath(dos_path);
  const auto slash = norm.rfind('/');
  if (slash != std::string::npos) {
    norm = norm.substr(slash + 1);
  }
  return util::ascii_upper(norm);
}

bool CaseInsensitiveVfs::is_rejected_device(std::string_view dos_path) {
  const std::string dev = stem_device_name(dos_path);
  if (dev == "MS$KANJI" || dev == "$DISP" || dev == "$FONT" || dev == "PRN" ||
      dev == "AUX" || dev == "CLOCK$" || dev == "CONIN$" || dev == "CONOUT$") {
    return true;
  }
  if (dev.size() == 4u && (dev.starts_with("COM") || dev.starts_with("LPT")) &&
      dev[3] >= '1' && dev[3] <= '9') {
    return true;
  }
  return false;
}

bool CaseInsensitiveVfs::is_emm_device(std::string_view dos_path) {
  return stem_device_name(dos_path) == "EMMXXXX0";
}

bool CaseInsensitiveVfs::is_nul_or_con_device(std::string_view dos_path) {
  const std::string dev = stem_device_name(dos_path);
  return dev == "NUL" || dev == "CON";
}

std::optional<std::filesystem::path> CaseInsensitiveVfs::resolve_for_read(
    std::string_view dos_path) const {
  if (is_rejected_device(dos_path) || is_emm_device(dos_path) ||
      is_nul_or_con_device(dos_path)) {
    return std::nullopt;
  }
  const std::string rel = normalize_dos_relpath(dos_path);
  if (rel.empty() || rel == "." || contains_parent_ref(rel) ||
      has_invalid_dos_chars(rel) || has_reserved_segment(rel)) {
    return std::nullopt;
  }
  if (save_dir_ != root_dir_) {
    if (auto hit = lookup_case_insensitive(save_dir_, rel)) {
      return hit;
    }
  }
  return lookup_case_insensitive(root_dir_, rel);
}

std::optional<std::filesystem::path> CaseInsensitiveVfs::resolve_for_write(
    std::string_view dos_path, bool truncate_or_create) const {
  std::string rel;
  if (!validate_write_leaf(dos_path, rel)) {
    return std::nullopt;
  }
  const std::filesystem::path target_base =
      save_dir_.empty() ? root_dir_ : save_dir_;
  if (target_base.empty()) {
    return std::nullopt;
  }
  const bool using_cow_save_dir = (target_base != root_dir_);
  std::error_code ec;
  std::filesystem::create_directories(target_base, ec);

  if (auto existing = lookup_case_insensitive(target_base, rel)) {
    if (using_cow_save_dir) {
      std::error_code perm_ec;
      std::filesystem::permissions(
          *existing,
          std::filesystem::perms::owner_read |
              std::filesystem::perms::owner_write,
          std::filesystem::perm_options::add, perm_ec);
    }
    return existing;
  }

  const std::filesystem::path dst =
      target_base / util::utf8_to_path(util::ascii_upper(rel));
  std::error_code sym_ec;
  const auto dst_st = std::filesystem::symlink_status(dst, sym_ec);
  if (!sym_ec && std::filesystem::is_symlink(dst_st)) {
    return std::nullopt;
  }
  if (!truncate_or_create && using_cow_save_dir) {
    if (auto orig = lookup_case_insensitive(root_dir_, rel)) {
      std::filesystem::copy_file(
          *orig, dst, std::filesystem::copy_options::overwrite_existing, ec);
      if (!ec) {
        std::error_code perm_ec;
        std::filesystem::permissions(
            dst,
            std::filesystem::perms::owner_read |
                std::filesystem::perms::owner_write,
            std::filesystem::perm_options::add, perm_ec);
        return dst;
      }
    }
    return std::nullopt;
  }
  if (!truncate_or_create) {
    return std::nullopt;
  }
  return dst;
}

std::optional<std::filesystem::path>
CaseInsensitiveVfs::resolve_existing_in_write_dir(
    std::string_view dos_path) const {
  std::string rel;
  if (!validate_write_leaf(dos_path, rel)) {
    return std::nullopt;
  }
  const std::filesystem::path target_base =
      save_dir_.empty() ? root_dir_ : save_dir_;
  if (target_base.empty()) {
    return std::nullopt;
  }
  return lookup_case_insensitive(target_base, rel);
}

bool CaseInsensitiveVfs::wildcard_match(std::string_view pattern,
                                        std::string_view name) {
  // Match DOS 8.3 stem and extension independently when pattern contains '.'
  const auto p_dot = pattern.find('.');
  if (p_dot != std::string_view::npos && pattern != "*.*") {
    const auto n_dot = name.find('.');
    const std::string_view p_stem = pattern.substr(0, p_dot);
    const std::string_view p_ext = pattern.substr(p_dot + 1);
    const std::string_view n_stem =
        (n_dot == std::string_view::npos) ? name : name.substr(0, n_dot);
    const std::string_view n_ext =
        (n_dot == std::string_view::npos) ? std::string_view{}
                                          : name.substr(n_dot + 1);
    return wildcard_match(p_stem, n_stem) && wildcard_match(p_ext, n_ext);
  }
  if (pattern == "*.*") {
    return true;
  }

  std::size_t pi = 0;
  std::size_t ni = 0;
  std::size_t star_p = std::string_view::npos;
  std::size_t star_n = 0;

  while (ni < name.size()) {
    if (pi < pattern.size() && (pattern[pi] == '?' || pattern[pi] == name[ni])) {
      ++pi;
      ++ni;
    } else if (pi < pattern.size() && pattern[pi] == '*') {
      star_p = pi++;
      star_n = ni;
    } else if (star_p != std::string_view::npos) {
      pi = star_p + 1;
      ni = ++star_n;
    } else {
      return false;
    }
  }
  while (pi < pattern.size() && pattern[pi] == '*') {
    ++pi;
  }
  return pi == pattern.size();
}

std::vector<DosDirMatch> CaseInsensitiveVfs::find_matching(
    std::string_view dos_pattern) const {
  const std::string norm = normalize_dos_relpath(dos_pattern);
  if (contains_parent_ref(norm)) {
    return {};
  }

  std::string dir_part;
  std::string file_pat = norm;
  const auto slash = norm.rfind('/');
  if (slash != std::string::npos) {
    dir_part = norm.substr(0, slash);
    file_pat = norm.substr(slash + 1);
  }
  while (dir_part == ".") {
    dir_part.clear();
  }
  if (file_pat == ".") {
    return {};
  }
  if (file_pat.empty()) {
    file_pat = "*.*";
  }
  const std::string up_pat = util::ascii_upper(file_pat);

  std::unordered_map<std::string, DosDirMatch> merged;

  auto scan_dir = [&](const std::filesystem::path& base_dir) {
    if (base_dir.empty()) {
      return;
    }
    std::filesystem::path target_dir = base_dir;
    if (!dir_part.empty()) {
      auto resolved = lookup_case_insensitive(base_dir, dir_part);
      if (!resolved) {
        return;
      }
      target_dir = std::move(*resolved);
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(target_dir, ec) || ec) {
      return;
    }
    for (const auto& ent : std::filesystem::directory_iterator(target_dir, ec)) {
      std::error_code sym_ec;
      if (ent.is_symlink(sym_ec) || sym_ec) {
        continue;
      }
      std::error_code reg_ec;
      if (!ent.is_regular_file(reg_ec) || reg_ec) {
        continue;
      }
      const std::string up_name =
          util::ascii_upper(util::path_to_utf8(ent.path().filename()));
      if (up_name.size() > 12u || !wildcard_match(up_pat, up_name)) {
        continue;
      }
      std::error_code sz_ec;
      const auto sz = std::filesystem::file_size(ent.path(), sz_ec);
      merged[up_name] = DosDirMatch{
          .dos_name = up_name,
          .size = sz_ec ? 0u : static_cast<std::uint32_t>(sz),
      };
    }
  };

  scan_dir(root_dir_);
  if (save_dir_ != root_dir_) {
    scan_dir(save_dir_);
  }

  std::vector<DosDirMatch> out;
  out.reserve(merged.size());
  for (auto& kv : merged) {
    out.push_back(std::move(kv.second));
  }
  std::sort(out.begin(), out.end(),
            [](const DosDirMatch& a, const DosDirMatch& b) {
              return a.dos_name < b.dos_name;
            });
  return out;
}

}  // namespace san4
