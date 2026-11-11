#include "dos/mouse.h"

#include <algorithm>

#include "x86emu_clean.h"

namespace san4 {

MouseHle::MouseHle() { reset(); }

void MouseHle::decode_cursor_16x16(const std::uint16_t* and_mask,
                                   const std::uint16_t* xor_mask,
                                   std::uint32_t invert_argb) {
  for (int r = 0; r < 16; ++r) {
    const std::uint16_t and_row = and_mask[r];
    const std::uint16_t xor_row = xor_mask[r];
    for (int c = 0; c < 16; ++c) {
      const unsigned shift = 15u - static_cast<unsigned>(c);
      const bool a = ((and_row >> shift) & 1u) != 0u;
      const bool x = ((xor_row >> shift) & 1u) != 0u;
      std::uint32_t col = 0x00000000u;
      if (!a && !x) {
        col = 0xFF000000u;
      } else if (!a && x) {
        col = 0xFFFFFFFFu;
      } else if (a && x) {
        col = invert_argb;
      }
      cursor_shape_.argb[r * 16 + c] = col;
    }
  }
}

void MouseHle::init_default_arrow_cursor() {
  static constexpr std::uint16_t kArrowAnd[16] = {
      0x3FFFu, 0x1FFFu, 0x0FFFu, 0x07FFu, 0x03FFu, 0x01FFu, 0x00FFu, 0x007Fu,
      0x003Fu, 0x001Fu, 0x01FFu, 0x10FFu, 0x30FFu, 0xF87Fu, 0xF87Fu, 0xFCFFu,
  };
  static constexpr std::uint16_t kArrowXor[16] = {
      0x0000u, 0x4000u, 0x6000u, 0x7000u, 0x7800u, 0x7C00u, 0x7E00u, 0x7F00u,
      0x7F80u, 0x7C00u, 0x6C00u, 0x4600u, 0x0600u, 0x0300u, 0x0300u, 0x0000u,
  };
  cursor_shape_.hot_x = 0;
  cursor_shape_.hot_y = 0;
  decode_cursor_16x16(kArrowAnd, kArrowXor, 0xFF000000u);
}

void MouseHle::reset() {
  x_ = 400;
  y_ = 300;
  last_motion_x_ = 400;
  last_motion_y_ = 300;
  min_x_ = 0;
  max_x_ = 799;
  min_y_ = 0;
  max_y_ = 599;
  hide_depth_ = 1;
  live_buttons_ = 0;
  latched_buttons_ = 0;
  latch_polls_remaining_ = {0, 0, 0};
  press_count_ = {0, 0, 0};
  last_press_x_ = {0, 0, 0};
  last_press_y_ = {0, 0, 0};
  release_count_ = {0, 0, 0};
  last_release_x_ = {0, 0, 0};
  last_release_y_ = {0, 0, 0};
  init_default_arrow_cursor();
  notify_cursor_changed();
}

void MouseHle::notify_cursor_changed() {
  if (cursor_cb_) {
    cursor_cb_(cursor_shape_, cursor_visible());
  }
}

void MouseHle::set_position(int x, int y) {
  x_ = std::clamp(x, min_x_, max_x_);
  y_ = std::clamp(y, min_y_, max_y_);
}

void MouseHle::set_button(int button_idx, bool pressed) {
  if (button_idx < 0 || button_idx >= 3) {
    return;
  }
  const auto mask = static_cast<std::uint16_t>(1u << button_idx);
  if (pressed) {
    live_buttons_ |= mask;
    latched_buttons_ |= mask;
    // Hold across >= 2 guest AX=0003h polls (~35 ms) so sub-frame trackpad taps
    // never vanish between polls.
    latch_polls_remaining_[button_idx] = 2;
    ++press_count_[button_idx];
    last_press_x_[button_idx] = x_;
    last_press_y_[button_idx] = y_;
  } else {
    live_buttons_ = static_cast<std::uint16_t>(live_buttons_ & ~mask);
    ++release_count_[button_idx];
    last_release_x_[button_idx] = x_;
    last_release_y_[button_idx] = y_;
  }
}

void MouseHle::handle_int33(x86emu_t* emu, bool& out_was_status_poll) {
  out_was_status_poll = false;
  const std::uint16_t func = emu->x86.R_AX;

  switch (func) {
    case 0x0000u:  // Reset Driver and Read Status
    case 0x0021u:  // Software Reset
      reset();
      emu->x86.R_AX = 0xFFFFu;
      emu->x86.R_BX = 3u;
      break;

    case 0x0001u:  // Show Mouse Cursor
      if (hide_depth_ > 0) {
        --hide_depth_;
        if (hide_depth_ == 0) {
          notify_cursor_changed();
        }
      }
      break;

    case 0x0002u:  // Hide Mouse Cursor
      // Note: MAIN.EXE wraps redraws with AX=0010h/0002h + AX=0001h for
      // software VRAM cursors. Since we render a zero-tear OS hardware cursor,
      // track hide_depth_ without flickering the OS cursor during redraws.
      ++hide_depth_;
      break;

    case 0x0003u: {  // Return Position and Button Status
      out_was_status_poll = true;
      const std::uint16_t effective_buttons =
          static_cast<std::uint16_t>(live_buttons_ | latched_buttons_);
      emu->x86.R_BX = effective_buttons;
      emu->x86.R_CX = static_cast<std::uint16_t>(x_);
      emu->x86.R_DX = static_cast<std::uint16_t>(y_);

      for (int b = 0; b < 3; ++b) {
        const auto mask = static_cast<std::uint16_t>(1u << b);
        if ((live_buttons_ & mask) == 0u && latch_polls_remaining_[b] > 0) {
          if (--latch_polls_remaining_[b] == 0) {
            latched_buttons_ =
                static_cast<std::uint16_t>(latched_buttons_ & ~mask);
          }
        }
      }
      break;
    }

    case 0x0004u: {  // Position Mouse Cursor (auto-snap to dialog buttons)
      const int nx = std::clamp(
          static_cast<int>(static_cast<std::int16_t>(emu->x86.R_CX)), min_x_,
          max_x_);
      const int ny = std::clamp(
          static_cast<int>(static_cast<std::int16_t>(emu->x86.R_DX)), min_y_,
          max_y_);
      x_ = nx;
      y_ = ny;
      last_motion_x_ = x_;
      last_motion_y_ = y_;
      if (warp_enabled_ && warp_cb_) {
        warp_cb_(x_, y_);
      }
      break;
    }

    case 0x0005u: {  // Return Button Press Data
      emu->x86.R_AX =
          static_cast<std::uint16_t>(live_buttons_ | latched_buttons_);
      if (emu->x86.R_BX >= 3u) {
        emu->x86.R_BX = 0u;
        emu->x86.R_CX = 0u;
        emu->x86.R_DX = 0u;
        break;
      }
      const std::uint16_t btn = emu->x86.R_BX;
      emu->x86.R_BX = press_count_[btn];
      emu->x86.R_CX = static_cast<std::uint16_t>(last_press_x_[btn]);
      emu->x86.R_DX = static_cast<std::uint16_t>(last_press_y_[btn]);
      press_count_[btn] = 0;
      break;
    }

    case 0x0006u: {  // Return Button Release Data
      emu->x86.R_AX =
          static_cast<std::uint16_t>(live_buttons_ | latched_buttons_);
      if (emu->x86.R_BX >= 3u) {
        emu->x86.R_BX = 0u;
        emu->x86.R_CX = 0u;
        emu->x86.R_DX = 0u;
        break;
      }
      const std::uint16_t btn = emu->x86.R_BX;
      emu->x86.R_BX = release_count_[btn];
      emu->x86.R_CX = static_cast<std::uint16_t>(last_release_x_[btn]);
      emu->x86.R_DX = static_cast<std::uint16_t>(last_release_y_[btn]);
      release_count_[btn] = 0;
      break;
    }

    case 0x0007u: {  // Define Horizontal Cursor Range
      int lo = static_cast<int>(static_cast<std::int16_t>(emu->x86.R_CX));
      int hi = static_cast<int>(static_cast<std::int16_t>(emu->x86.R_DX));
      if (lo > hi) std::swap(lo, hi);
      // Ignore legacy 640px mode-12h default clamp so full 800px stays active.
      if (lo == 0 && hi == 639) {
        hi = 799;
      }
      min_x_ = std::clamp(lo, 0, 799);
      max_x_ = std::clamp(hi, min_x_, 799);
      x_ = std::clamp(x_, min_x_, max_x_);
      break;
    }

    case 0x0008u: {  // Define Vertical Cursor Range
      int lo = static_cast<int>(static_cast<std::int16_t>(emu->x86.R_CX));
      int hi = static_cast<int>(static_cast<std::int16_t>(emu->x86.R_DX));
      if (lo > hi) std::swap(lo, hi);
      if (lo == 0 && hi == 479) {
        hi = 599;
      }
      min_y_ = std::clamp(lo, 0, 599);
      max_y_ = std::clamp(hi, min_y_, 599);
      y_ = std::clamp(y_, min_y_, max_y_);
      break;
    }

    case 0x0009u: {  // Define Graphics Cursor (16x16 AND + XOR masks at ES:DX)
      cursor_shape_.hot_x = std::clamp(
          static_cast<int>(static_cast<std::int16_t>(emu->x86.R_BX)), 0, 15);
      cursor_shape_.hot_y = std::clamp(
          static_cast<int>(static_cast<std::int16_t>(emu->x86.R_CX)), 0, 15);
      const std::uint32_t addr =
          (static_cast<std::uint32_t>(emu->x86.R_ES) << 4) + emu->x86.R_DX;
      std::array<std::uint16_t, 16> and_rows{};
      std::array<std::uint16_t, 16> xor_rows{};
      for (unsigned r = 0; r < 16u; ++r) {
        and_rows[r] =
            static_cast<std::uint16_t>(x86emu_read_word(emu, addr + r * 2u));
        xor_rows[r] = static_cast<std::uint16_t>(
            x86emu_read_word(emu, addr + 32u + r * 2u));
      }
      decode_cursor_16x16(and_rows.data(), xor_rows.data(), 0xFF202020u);
      hide_depth_ = 0;
      notify_cursor_changed();
      break;
    }

    case 0x000Bu: {  // Read Motion Counters
      emu->x86.R_CX = static_cast<std::uint16_t>(
          static_cast<std::int16_t>((x_ - last_motion_x_) * 2));
      emu->x86.R_DX = static_cast<std::uint16_t>(
          static_cast<std::int16_t>((y_ - last_motion_y_) * 2));
      last_motion_x_ = x_;
      last_motion_y_ = y_;
      break;
    }

    case 0x000Cu:  // Define Event Handler (stubbed; MAIN.EXE polls AX=0003h)
    case 0x000Fu:  // Mickey-to-pixel ratio
    case 0x0010u:  // Conditional Off (paired with AX=0001h by MAIN.EXE)
      if (func == 0x0010u) {
        ++hide_depth_;
      }
      break;

    case 0x0012u:  // Large Graphics Cursor (accept cleanly)
    case 0x001Au:
    case 0x0028u:  // Set Video Mode (ignore 640x480 clamp, return success AX=0)
      if (func == 0x0028u) {
        emu->x86.R_AX = 0x0000u;
      }
      break;

    case 0x00FFu:  // GRPMOUSE custom cursor color extension (MAIN.EXE 0x4D63)
      break;

    default:
      break;
  }
}

}  // namespace san4
