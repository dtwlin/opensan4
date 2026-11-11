#pragma once

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace san4::util {

inline std::filesystem::path utf8_to_path(std::string_view utf8) {
  if (utf8.empty()) {
    return {};
  }
  const auto* p = reinterpret_cast<const char8_t*>(utf8.data());
  return std::filesystem::path(std::u8string_view(p, utf8.size()));
}

inline std::string path_to_utf8(const std::filesystem::path& p) {
  const std::u8string u8 = p.u8string();
  return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

inline std::string ascii_upper(std::string_view sv) {
  std::string out(sv);
  for (char& ch : out) {
    if (ch >= 'a' && ch <= 'z') {
      ch = static_cast<char>(ch - 'a' + 'A');
    }
  }
  return out;
}

inline std::string ascii_lower(std::string_view sv) {
  std::string out(sv);
  for (char& ch : out) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return out;
}

inline bool is_ascii_digit(char ch) {
  return ch >= '0' && ch <= '9';
}

inline bool is_ascii_hex_digit(char ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
         (ch >= 'A' && ch <= 'F');
}

inline bool parse_int_strict(std::string_view sv, int min_val, int max_val,
                             int& out) {
  if (sv.empty() || min_val > max_val) {
    return false;
  }
  if (sv.front() == '-') {
    if (sv.size() < 2u || !is_ascii_digit(sv[1])) {
      return false;
    }
  } else if (!is_ascii_digit(sv.front())) {
    return false;
  }

  int parsed = 0;
  const char* begin = sv.data();
  const char* end = sv.data() + sv.size();
  const auto [ptr, ec] = std::from_chars(begin, end, parsed, 10);
  if (ec != std::errc{} || ptr != end || parsed < min_val || parsed > max_val) {
    return false;
  }
  out = parsed;
  return true;
}

inline bool parse_u64_hex_strict(std::string_view sv, std::uint64_t& out) {
  if (sv.empty() || !is_ascii_hex_digit(sv.front())) {
    return false;
  }

  std::uint64_t parsed = 0;
  const char* begin = sv.data();
  const char* end = sv.data() + sv.size();
  const auto [ptr, ec] = std::from_chars(begin, end, parsed, 16);
  if (ec != std::errc{} || ptr != end) {
    return false;
  }
  out = parsed;
  return true;
}

inline std::string extract_win32_device_stem_upper(std::string_view segment) {
  const std::size_t dot = segment.find('.');
  std::string_view stem =
      (dot == std::string_view::npos) ? segment : segment.substr(0u, dot);
  while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.')) {
    stem.remove_suffix(1u);
  }
  return ascii_upper(stem);
}

inline bool is_win32_reserved_device_stem(std::string_view segment) {
  const std::string up = extract_win32_device_stem_upper(segment);
  if (up == "CON" || up == "PRN" || up == "AUX" || up == "NUL" ||
      up == "CLOCK$" || up == "CONIN$" || up == "CONOUT$") {
    return true;
  }
  if (up.size() == 4u && (up.starts_with("COM") || up.starts_with("LPT")) &&
      up[3] >= '1' && up[3] <= '9') {
    return true;
  }
  return false;
}

}  // namespace san4::util
