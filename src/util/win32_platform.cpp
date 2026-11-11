#include "util/win32_platform.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <system_error>

#include "util/string_util.h"

namespace san4::win32_platform {

void init_process() noexcept {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  if (kernel32 != nullptr) {
    struct PowerThrottlingState {
      ULONG Version;
      ULONG ControlMask;
      ULONG StateMask;
    };
    using SetProcessInformationFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
    auto fn = reinterpret_cast<SetProcessInformationFn>(
        reinterpret_cast<void*>(
            GetProcAddress(kernel32, "SetProcessInformation")));
    if (fn != nullptr) {
      // ProcessPowerThrottling (4): clear EXECUTION_SPEED (0x1) and
      // IGNORE_TIMER_RESOLUTION (0x4) so 1 ms timer resolution stays active
      // when minimized/occluded on Windows 11.
      PowerThrottlingState throttling{};
      throttling.Version = 1u;
      throttling.ControlMask = 0x5u;
      throttling.StateMask = 0x0u;
      fn(GetCurrentProcess(), 4, &throttling,
         static_cast<DWORD>(sizeof(throttling)));
    }
  }
#endif
}

void attach_parent_console_if_needed() noexcept {
#ifdef _WIN32
  auto is_unattached = [](HANDLE h) noexcept -> bool {
    return h == nullptr || h == INVALID_HANDLE_VALUE ||
           GetFileType(h) == FILE_TYPE_UNKNOWN;
  };
  const bool need_out = is_unattached(GetStdHandle(STD_OUTPUT_HANDLE));
  const bool need_err = is_unattached(GetStdHandle(STD_ERROR_HANDLE));
  if (!need_out && !need_err) {
    return;
  }
  if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
    return;
  }
  FILE* fp = nullptr;
  if (need_out) {
    freopen_s(&fp, "CONOUT$", "w", stdout);
    std::cout.clear();
  }
  if (need_err) {
    freopen_s(&fp, "CONOUT$", "w", stderr);
    std::cerr.clear();
  }
#endif
}

void show_error_dialog(std::string_view title,
                       std::string_view message) noexcept {
#ifdef _WIN32
  const wchar_t* no_dlg = _wgetenv(L"SAN4_NO_ERROR_DIALOG");
  if (no_dlg != nullptr && *no_dlg != L'\0' &&
      !(no_dlg[0] == L'0' && no_dlg[1] == L'\0')) {
    return;
  }
  auto to_wide = [](std::string_view sv) -> std::wstring {
    if (sv.empty()) {
      return std::wstring();
    }
    const int n = MultiByteToWideChar(
        CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), nullptr, 0);
    if (n <= 0) {
      return std::wstring();
    }
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()),
                        out.data(), n);
    return out;
  };
  const std::wstring wtitle = to_wide(title);
  const std::wstring wmsg = to_wide(message);
  MessageBoxW(nullptr, wmsg.c_str(), wtitle.c_str(),
              MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
#else
  (void)title;
  (void)message;
#endif
}

std::vector<std::string> get_utf8_argv(int argc, char** argv) {
#ifdef _WIN32
  int wargc = 0;
  LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  if (wargv != nullptr && wargc > 0) {
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(wargc));
    for (int i = 0; i < wargc; ++i) {
      const wchar_t* ws = wargv[i];
      if (ws == nullptr || *ws == L'\0') {
        out.emplace_back();
        continue;
      }
      const int len =
          WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
      if (len > 1) {
        std::string utf8(static_cast<std::size_t>(len - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, ws, -1, utf8.data(), len, nullptr,
                            nullptr);
        out.push_back(std::move(utf8));
      } else {
        out.emplace_back();
      }
    }
    LocalFree(wargv);
    return out;
  }
#endif
  std::vector<std::string> out;
  if (argc > 0 && argv != nullptr) {
    out.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
      out.emplace_back(argv[i] != nullptr ? argv[i] : "");
    }
  }
  return out;
}

std::filesystem::path get_executable_path(const char* argv0) {
#ifdef _WIN32
  std::wstring buf(MAX_PATH, L'\0');
  while (buf.size() <= 32768u) {
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(),
                                       static_cast<DWORD>(buf.size()));
    if (n == 0u) {
      break;
    }
    if (n < buf.size() - 1u) {
      buf.resize(static_cast<std::size_t>(n));
      return std::filesystem::path(buf);
    }
    buf.resize(buf.size() * 2u, L'\0');
  }
#endif
  return util::utf8_to_path(argv0 != nullptr ? argv0 : "");
}

std::optional<std::filesystem::path> get_env_path(const char* name) {
  if (name == nullptr || *name == '\0') {
    return std::nullopt;
  }
#ifdef _WIN32
  const int wlen = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
  if (wlen <= 1) {
    return std::nullopt;
  }
  std::wstring wname(static_cast<std::size_t>(wlen - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, name, -1, wname.data(), wlen);
  const wchar_t* val = _wgetenv(wname.c_str());
  if (val == nullptr || *val == L'\0') {
    return std::nullopt;
  }
  return std::filesystem::path(val);
#else
  const char* val = std::getenv(name);
  if (val == nullptr || *val == '\0') {
    return std::nullopt;
  }
  return util::utf8_to_path(val);
#endif
}

bool is_directory_writable(const std::filesystem::path& dir) noexcept {
  std::error_code ec;
  if (dir.empty() || !std::filesystem::is_directory(dir, ec) || ec) {
    return false;
  }

  for (const auto& ent : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) {
      return false;
    }
    const std::string up_name =
        util::ascii_upper(util::path_to_utf8(ent.path().filename()));
    if (up_name != "SAVEDAT.S4" && up_name != "NBDATA.S4") {
      continue;
    }
#ifdef _WIN32
    const DWORD attr = GetFileAttributesW(ent.path().c_str());
    if (attr != INVALID_FILE_ATTRIBUTES &&
        (attr & FILE_ATTRIBUTE_READONLY) != 0u) {
      return false;
    }
#else
    const auto st = std::filesystem::status(ent.path(), ec);
    if (ec || (st.permissions() & std::filesystem::perms::owner_write) ==
                  std::filesystem::perms::none ||
        ::access(ent.path().c_str(), W_OK) != 0) {
      return false;
    }
#endif
    std::fstream rw_probe(ent.path(),
                          std::ios::in | std::ios::out | std::ios::binary);
    if (!rw_probe.is_open()) {
      return false;
    }
  }

#ifdef _WIN32
  const unsigned long pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
  const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
  const auto tick = static_cast<unsigned long long>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const std::filesystem::path probe =
      dir / (".san4_write_probe_" + std::to_string(pid) + "_" +
             std::to_string(tick));
  bool ok = false;
  {
    std::ofstream ofs(probe, std::ios::binary | std::ios::trunc);
    if (ofs) {
      const char byte = 0x34;
      ofs.write(&byte, 1);
      ofs.flush();
      ok = ofs.good();
    }
  }
  std::filesystem::remove(probe, ec);
  return ok;
}

bool is_game_dir_writable(const std::filesystem::path& dir) noexcept {
  return is_directory_writable(dir);
}

std::filesystem::path select_user_save_dir(
    const std::filesystem::path& exe_dir) noexcept {
  std::error_code ec;
#ifdef _WIN32
  if (auto local_appdata = get_env_path("LOCALAPPDATA");
      local_appdata && !local_appdata->empty()) {
    std::filesystem::path cand = *local_appdata / "SAN4" / "saves";
    std::filesystem::create_directories(cand, ec);
    if (is_directory_writable(cand)) {
      return cand;
    }
  }
  if (!exe_dir.empty()) {
    std::filesystem::path exe_saves = exe_dir / "saves";
    std::filesystem::create_directories(exe_saves, ec);
    if (is_directory_writable(exe_saves)) {
      return exe_saves;
    }
  }
#else
  (void)exe_dir;
  if (auto xdg = get_env_path("XDG_DATA_HOME");
      xdg && !xdg->empty() && xdg->is_absolute()) {
    std::filesystem::path cand = *xdg / "san4" / "saves";
    std::filesystem::create_directories(cand, ec);
    if (is_directory_writable(cand)) {
      return cand;
    }
  }
  if (auto home = get_env_path("HOME"); home && !home->empty()) {
    std::filesystem::path cand =
        *home / ".local" / "share" / "san4" / "saves";
    std::filesystem::create_directories(cand, ec);
    if (is_directory_writable(cand)) {
      return cand;
    }
  }
#endif
  return {};
}

}  // namespace san4::win32_platform
