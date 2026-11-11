#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace san4::win32_platform {

// Initializes platform process settings (UTF-8 console output codepage and
// Windows 11 background timer/execution power-throttling opt-out). No-op on
// non-Windows platforms.
void init_process() noexcept;

// On Windows GUI subsystem builds (-mwindows), attaches to the parent console
// (PowerShell / cmd.exe / Windows Terminal) when stdout/stderr are not already
// redirected to a pipe or file. No-op on non-Windows platforms.
void attach_parent_console_if_needed() noexcept;

// Displays a native modal Win32 error dialog (MessageBoxW) on Windows unless
// SAN4_NO_ERROR_DIALOG is set in the environment. No-op on non-Windows.
void show_error_dialog(std::string_view title,
                       std::string_view message) noexcept;

// Returns UTF-8 command-line arguments. On Windows, decodes GetCommandLineW()
// via CommandLineToArgvW + WideCharToMultiByte(CP_UTF8) so CJK paths work
// regardless of the active ANSI code page.
std::vector<std::string> get_utf8_argv(int argc, char** argv);
inline std::vector<std::string> get_utf8_args(int argc, char** argv) {
  return get_utf8_argv(argc, argv);
}

// Returns the path to the running executable. On Windows, queries
// GetModuleFileNameW(nullptr, ...); on POSIX, converts argv0 from UTF-8.
std::filesystem::path get_executable_path(const char* argv0);

// Reads a path environment variable without narrow code-page loss (_wgetenv on
// Windows, std::getenv + utf8_to_path on POSIX).
std::optional<std::filesystem::path> get_env_path(const char* name);

// Returns true if `dir` exists, allows creating and deleting a temporary probe
// file, and does not have a read-only `SAVEDAT.S4` or `NBDATA.S4` file.
bool is_directory_writable(const std::filesystem::path& dir) noexcept;
bool is_game_dir_writable(const std::filesystem::path& dir) noexcept;

// Selects a writable user save directory when `game_dir` is read-only:
// Windows: %LOCALAPPDATA%\SAN4\saves (fallback: <exe_dir>\saves)
// POSIX:   $XDG_DATA_HOME/san4/saves or $HOME/.local/share/san4/saves
std::filesystem::path select_user_save_dir(
    const std::filesystem::path& exe_dir = {}) noexcept;

}  // namespace san4::win32_platform

