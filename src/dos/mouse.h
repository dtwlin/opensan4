#pragma once

#include <array>
#include <cstdint>
#include <functional>

typedef struct x86emu_s x86emu_t;

namespace san4 {

struct DosCursorShape {
  int hot_x = 0;
  int hot_y = 0;
  std::array<std::uint32_t, 16 * 16> argb{};  // 0xAARRGGBB (16x16)
};

class MouseHle {
 public:
  MouseHle();

  void reset();

  // Host input updates (in logical 800x600 coordinates).
  void set_position(int x, int y);
  void set_button(int button_idx, bool pressed);  // 0=Left, 1=Right, 2=Middle

  int x() const { return x_; }
  int y() const { return y_; }
  bool cursor_visible() const { return hide_depth_ <= 0; }
  const DosCursorShape& cursor_shape() const { return cursor_shape_; }

  void set_warp_enabled(bool enabled) { warp_enabled_ = enabled; }
  void set_warp_callback(std::function<void(int, int)> cb) {
    warp_cb_ = std::move(cb);
  }
  void set_cursor_callback(std::function<void(const DosCursorShape&, bool)> cb) {
    cursor_cb_ = std::move(cb);
  }

  // Handles INT 33h HLE; sets out_was_status_poll = true on AX=0003h so the
  // machine scheduler can detect quiescent input-wait loops cleanly.
  void handle_int33(x86emu_t* emu, bool& out_was_status_poll);

 private:
  void decode_cursor_16x16(const std::uint16_t* and_mask,
                           const std::uint16_t* xor_mask,
                           std::uint32_t invert_argb);
  void init_default_arrow_cursor();
  void notify_cursor_changed();

  int x_ = 400;
  int y_ = 300;
  int last_motion_x_ = 400;
  int last_motion_y_ = 300;
  int min_x_ = 0;
  int max_x_ = 799;
  int min_y_ = 0;
  int max_y_ = 599;

  int hide_depth_ = 1;  // Hidden until AX=0001h Show Mouse
  std::uint16_t live_buttons_ = 0;
  std::uint16_t latched_buttons_ = 0;
  std::array<int, 3> latch_polls_remaining_{0, 0, 0};

  std::array<std::uint16_t, 3> press_count_{0, 0, 0};
  std::array<int, 3> last_press_x_{0, 0, 0};
  std::array<int, 3> last_press_y_{0, 0, 0};
  std::array<std::uint16_t, 3> release_count_{0, 0, 0};
  std::array<int, 3> last_release_x_{0, 0, 0};
  std::array<int, 3> last_release_y_{0, 0, 0};

  bool warp_enabled_ = true;
  DosCursorShape cursor_shape_{};
  std::function<void(int, int)> warp_cb_;
  std::function<void(const DosCursorShape&, bool)> cursor_cb_;
};

}  // namespace san4
