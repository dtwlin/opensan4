#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <utility>
#include <vector>

#include "dos/mouse.h"
#include "upscaler.h"
#include "video.h"

namespace san4 {

// Messages between the main (SDL) thread and the emulation thread. The main
// thread never touches X86Machine, PixelUpscaler, or HdPackManager while the
// emulation thread runs: input and hotkeys travel as HostCmd, MouseHle
// callbacks come back as HostEvent, and window-title / coordinate-mapping
// state is published with every frame as HostStatus.

// Emulator state the main thread needs, captured after each produced frame.
struct HostStatus {
  UpscaleMode mode = UpscaleMode::Sharp2x;
  int speed_multiplier = 1;
  std::size_t hd_loaded = 0;
  bool hd_dumping = false;
  bool exited = false;
  // Active guest resolution, used to map window to DOS mouse coordinates.
  int active_w = VesaPlanarVideo::kWidth;
  int active_h = VesaPlanarVideo::kHeight;

  bool operator==(const HostStatus&) const = default;
};

enum class HostCmdType : std::uint8_t {
  PushKey,        // a = ASCII, b = scancode
  SetShiftFlags,  // a = BDA shift flags
  MouseMotion,    // a, b = DOS position; also drives middle-drag panning
  MousePos,       // a, b = DOS position (button events)
  MouseButton,    // a = button index, b = pressed
  SetTurbo,       // a = held
  CycleSpeed,
  SetPackDir,     // path
  ReloadPack,
  ToggleDump,     // path = dump directory used when enabling
  MarkAllDirty,
  CycleScale,     // F5: next upscale mode; disables automatic scaling
  SetScale,       // a = UpscaleMode (fallback when the texture cannot be made)
  AutoScale,      // a = drawable height; re-evaluates the automatic scale
  SetDrainAudio,  // a = discard audio on emulation thread (no audio device)
  Quit,
};

struct HostCmd {
  HostCmdType type = HostCmdType::Quit;
  int a = 0;
  int b = 0;
  std::filesystem::path path;

  static HostCmd push_key(std::uint8_t ascii, std::uint8_t scancode) {
    return {HostCmdType::PushKey, ascii, scancode, {}};
  }
  static HostCmd set_shift_flags(std::uint8_t flags) {
    return {HostCmdType::SetShiftFlags, flags, 0, {}};
  }
  static HostCmd mouse_motion(int x, int y) {
    return {HostCmdType::MouseMotion, x, y, {}};
  }
  static HostCmd mouse_pos(int x, int y) {
    return {HostCmdType::MousePos, x, y, {}};
  }
  static HostCmd mouse_button(int button, bool pressed) {
    return {HostCmdType::MouseButton, button, pressed ? 1 : 0, {}};
  }
  static HostCmd set_turbo(bool held) {
    return {HostCmdType::SetTurbo, held ? 1 : 0, 0, {}};
  }
  static HostCmd simple(HostCmdType type) { return {type, 0, 0, {}}; }
  static HostCmd with_path(HostCmdType type, std::filesystem::path p) {
    return {type, 0, 0, std::move(p)};
  }
  static HostCmd set_scale(UpscaleMode mode) {
    return {HostCmdType::SetScale, static_cast<int>(mode), 0, {}};
  }
  static HostCmd auto_scale(int drawable_h) {
    return {HostCmdType::AutoScale, drawable_h, 0, {}};
  }
  static HostCmd set_drain_audio(bool drain) {
    return {HostCmdType::SetDrainAudio, drain ? 1 : 0, 0, {}};
  }
};

struct HostEvent {
  enum class Type : std::uint8_t { Warp, Cursor };
  Type type = Type::Warp;
  // Warp: DOS target position and the active resolution it refers to.
  int x = 0;
  int y = 0;
  int active_w = VesaPlanarVideo::kWidth;
  int active_h = VesaPlanarVideo::kHeight;
  // Cursor: new shape and visibility.
  DosCursorShape shape{};
  bool visible = false;
};

// Mutex-guarded FIFO handed over in batches: the producer pushes single
// items, the consumer swaps the whole pending batch out.
template <typename T>
class HostQueue {
 public:
  void push(T item) {
    std::lock_guard<std::mutex> lock(mu_);
    pending_.push_back(std::move(item));
  }

  // Replaces `out` with every queued item in push order.
  void drain(std::vector<T>& out) {
    out.clear();
    std::lock_guard<std::mutex> lock(mu_);
    out.swap(pending_);
  }

 private:
  std::mutex mu_;
  std::vector<T> pending_;
};

using HostCommandQueue = HostQueue<HostCmd>;
using HostEventQueue = HostQueue<HostEvent>;

}  // namespace san4
