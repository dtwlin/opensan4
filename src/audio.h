#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "opl/dbopl.h"

namespace san4 {

class Opl2Audio {
 public:
  static constexpr int kSampleRate = 44100;
  static constexpr std::uint32_t kRingFrames = 4096;  // Power of two (~92.8 ms)
  static constexpr std::uint32_t kRingMask = kRingFrames - 1;
  static constexpr std::uint32_t kMaxBufferedFrames = 3200;  // ~72.5 ms clamp

  Opl2Audio();

  bool handles_port(std::uint16_t port) const;
  std::uint8_t port_in8(std::uint16_t port);
  void port_out8(std::uint16_t port, std::uint8_t val);
  void on_pit_control(std::uint8_t ctrl);

  // Advances virtual time (in seconds) up to a register write or 291.304 Hz
  // PIT tick, synthesizing exact DBOPL + PC speaker frames into the SPSC ring.
  void advance_seconds(double dt_sec);

  // Lock-free consumer called from SDL2's audio thread (or unit tests).
  // Reads `frames` stereo int16_t pairs (`frames * 2` int16_t elements),
  // zero-filling smoothly on underrun.
  std::uint32_t pull_stereo_s16(std::int16_t* dst_interleaved,
                                std::uint32_t frames);

  std::uint32_t buffered_frames() const;
  std::uint64_t total_rendered_frames() const {
    return total_rendered_frames_.load(std::memory_order_relaxed);
  }
  std::int32_t peak_abs_sample() const {
    return peak_abs_sample_.load(std::memory_order_relaxed);
  }
  void reset_peak_abs_sample() {
    peak_abs_sample_.store(0, std::memory_order_relaxed);
  }

 private:
  void step_adlib_timers(double dt_us);
  void render_frames(std::uint32_t frames);

  DBOPL::Chip chip_{};
  std::uint32_t reg_index_ = 0;

  // YM3812 hardware timers (0x02: 80 us, 0x03: 320 us, 0x04: control/IRQ)
  std::uint8_t timer1_preset_ = 0;
  std::uint8_t timer2_preset_ = 0;
  std::uint8_t timer_ctrl_ = 0;
  double timer1_rem_us_ = 0.0;
  double timer2_rem_us_ = 0.0;
  bool timer1_expired_ = false;
  bool timer2_expired_ = false;

  // PC Speaker (PIT channel 2 port 0x42 + control 0x43 + gate port 0x61)
  std::uint8_t port61_ = 0;
  std::uint32_t pit_ch2_reload_ = 1193u;  // ~1 kHz default (65536 when 0 written)
  std::uint8_t pit_ch2_access_mode_ = 3u;  // 1=LSB, 2=MSB, 3=LSB+MSB
  bool pit_ch2_msb_ = false;
  bool pit_ch2_read_msb_ = false;
  bool pit_ch2_latched_ = false;
  std::uint16_t pit_ch2_latched_val_ = 1193u;
  std::uint8_t pit_ch2_lo_ = 0;
  double spk_phase_ = 0.0;
  float spk_env_ = 0.0f;

  double sample_accum_ = 0.0;

  // Lock-free SPSC stereo ring buffer (each frame = 2 * int16_t).
  alignas(64) std::array<std::int16_t, kRingFrames * 2> ring_{};
  alignas(64) std::atomic<std::uint32_t> write_pos_{0};
  alignas(64) std::atomic<std::uint32_t> read_pos_{0};

  std::int16_t last_sample_l_ = 0;
  std::int16_t last_sample_r_ = 0;
  std::atomic<std::uint64_t> total_rendered_frames_{0};
  std::atomic<std::int32_t> peak_abs_sample_{0};
};

}  // namespace san4
