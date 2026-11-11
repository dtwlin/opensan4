#include "audio.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace san4 {

Opl2Audio::Opl2Audio() {
  [[maybe_unused]] static const bool kTablesInitialized = []() {
    DBOPL::InitTables();
    return true;
  }();
  chip_.Setup(kSampleRate);
}

bool Opl2Audio::handles_port(std::uint16_t port) const {
  return port == 0x0388u || port == 0x0389u ||
         (port >= 0x0220u && port <= 0x0223u) || port == 0x0042u ||
         port == 0x0061u;
}

void Opl2Audio::on_pit_control(std::uint8_t ctrl) {
  if (((ctrl >> 6) & 3u) != 2u) {
    return;
  }
  const std::uint8_t rl = static_cast<std::uint8_t>((ctrl >> 4) & 3u);
  if (rl == 0u) {
    if (!pit_ch2_latched_) {
      pit_ch2_latched_ = true;
      pit_ch2_latched_val_ = static_cast<std::uint16_t>(pit_ch2_reload_ & 0xFFFFu);
      pit_ch2_read_msb_ = false;
    }
    return;
  }
  pit_ch2_access_mode_ = rl;
  pit_ch2_msb_ = false;
  pit_ch2_read_msb_ = false;
  pit_ch2_latched_ = false;
}

void Opl2Audio::step_adlib_timers(double dt_us) {
  if (dt_us <= 0.0) {
    return;
  }
  if ((timer_ctrl_ & 0x01u) != 0u) {
    timer1_rem_us_ -= dt_us;
    if (timer1_rem_us_ <= 0.0) {
      if ((timer_ctrl_ & 0x40u) == 0u) {
        timer1_expired_ = true;
      }
      const double period =
          static_cast<double>(256u - timer1_preset_) * 80.0;
      timer1_rem_us_ = std::max(80.0, period);
    }
  }
  if ((timer_ctrl_ & 0x02u) != 0u) {
    timer2_rem_us_ -= dt_us;
    if (timer2_rem_us_ <= 0.0) {
      if ((timer_ctrl_ & 0x20u) == 0u) {
        timer2_expired_ = true;
      }
      const double period =
          static_cast<double>(256u - timer2_preset_) * 320.0;
      timer2_rem_us_ = std::max(320.0, period);
    }
  }
}

std::uint8_t Opl2Audio::port_in8(std::uint16_t port) {
  if (port == 0x0061u) {
    port61_ ^= 0x10u;  // Refresh bit toggle expected by some DOS delay loops
    return port61_;
  }
  if (port == 0x0042u) {
    const std::uint16_t v =
        pit_ch2_latched_ ? pit_ch2_latched_val_
                         : static_cast<std::uint16_t>(pit_ch2_reload_ & 0xFFFFu);
    if (pit_ch2_access_mode_ == 1u) {
      pit_ch2_latched_ = false;
      return static_cast<std::uint8_t>(v & 0xFFu);
    }
    if (pit_ch2_access_mode_ == 2u) {
      pit_ch2_latched_ = false;
      return static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    }
    if (!pit_ch2_read_msb_) {
      pit_ch2_read_msb_ = true;
      return static_cast<std::uint8_t>(v & 0xFFu);
    }
    pit_ch2_read_msb_ = false;
    pit_ch2_latched_ = false;
    return static_cast<std::uint8_t>((v >> 8) & 0xFFu);
  }
  if (port == 0x0388u || port == 0x0220u || port == 0x0222u) {
    // Emulate ~1.5 us ISA bus cycle so tight AdLib detection loops in FMDRV.COM
    // observe Timer 1 expiry deterministically even during unthrottled bursts.
    step_adlib_timers(1.5);
    std::uint8_t status = 0x00u;
    if (timer1_expired_) {
      status |= 0xC0u;
    }
    if (timer2_expired_) {
      status |= 0xA0u;
    }
    return status;
  }
  return 0x00u;
}

void Opl2Audio::port_out8(std::uint16_t port, std::uint8_t val) {
  if (port == 0x0061u) {
    port61_ = val;
    return;
  }
  if (port == 0x0042u) {
    if (pit_ch2_access_mode_ == 1u) {
      pit_ch2_reload_ = (val == 0u) ? 65536u : static_cast<std::uint32_t>(val);
      return;
    }
    if (pit_ch2_access_mode_ == 2u) {
      const std::uint16_t div = static_cast<std::uint16_t>(val) << 8;
      pit_ch2_reload_ = (div == 0u) ? 65536u : static_cast<std::uint32_t>(div);
      return;
    }
    if (!pit_ch2_msb_) {
      pit_ch2_lo_ = val;
      pit_ch2_msb_ = true;
    } else {
      const std::uint16_t div =
          static_cast<std::uint16_t>(pit_ch2_lo_) |
          (static_cast<std::uint16_t>(val) << 8);
      pit_ch2_reload_ = (div == 0u) ? 65536u : static_cast<std::uint32_t>(div);
      pit_ch2_msb_ = false;
    }
    return;
  }

  if (port == 0x0388u || port == 0x0220u) {
    reg_index_ = chip_.WriteAddr(0, val);
    step_adlib_timers(1.0);
    return;
  }
  if (port == 0x0222u) {
    reg_index_ = chip_.WriteAddr(2, val);
    step_adlib_timers(1.0);
    return;
  }

  if (port == 0x0389u || port == 0x0221u || port == 0x0223u) {
    // Flush accumulated samples before mutating OPL2 state for write-exact FM.
    const auto whole = static_cast<std::uint32_t>(sample_accum_);
    if (whole > 0u) {
      sample_accum_ -= static_cast<double>(whole);
      render_frames(whole);
    }

    if (reg_index_ == 0x02u) {
      timer1_preset_ = val;
    } else if (reg_index_ == 0x03u) {
      timer2_preset_ = val;
    } else if (reg_index_ == 0x04u) {
      if ((val & 0x80u) != 0u) {
        timer1_expired_ = false;
        timer2_expired_ = false;
      } else {
        const std::uint8_t prev = timer_ctrl_;
        timer_ctrl_ = val;
        if ((val & 0x01u) != 0u && (prev & 0x01u) == 0u) {
          timer1_rem_us_ =
              static_cast<double>(256u - timer1_preset_) * 80.0;
        }
        if ((val & 0x02u) != 0u && (prev & 0x02u) == 0u) {
          timer2_rem_us_ =
              static_cast<double>(256u - timer2_preset_) * 320.0;
        }
      }
    } else {
      chip_.WriteReg(reg_index_, val);
    }
    return;
  }
}

void Opl2Audio::advance_seconds(double dt_sec) {
  if (!std::isfinite(dt_sec) || !(dt_sec > 0.0)) {
    return;
  }
  dt_sec = std::min(dt_sec, 0.250);
  step_adlib_timers(dt_sec * 1e6);
  sample_accum_ += dt_sec * static_cast<double>(kSampleRate);
  const auto whole = static_cast<std::uint32_t>(sample_accum_);
  if (whole > 0u) {
    sample_accum_ -= static_cast<double>(whole);
    render_frames(whole);
  }
}

std::uint32_t Opl2Audio::buffered_frames() const {
  const std::uint32_t w = write_pos_.load(std::memory_order_acquire);
  const std::uint32_t r = read_pos_.load(std::memory_order_acquire);
  return w - r;
}

void Opl2Audio::render_frames(std::uint32_t frames) {
  static constexpr std::uint32_t kChunk = 256;
  Bit32s opl_buf[kChunk * 2];

  const bool spk_on = ((port61_ & 0x03u) == 0x03u);
  const double spk_freq = std::clamp(
      1193182.0 / static_cast<double>(std::max<std::uint32_t>(1u, pit_ch2_reload_)),
      40.0, 8000.0);
  const double spk_inc = spk_freq / static_cast<double>(kSampleRate);
  const float env_step = 1.0f / (0.002f * static_cast<float>(kSampleRate));

  std::int32_t local_peak = peak_abs_sample_.load(std::memory_order_relaxed);

  while (frames > 0u) {
    const std::uint32_t n = std::min(frames, kChunk);
    frames -= n;

    if (chip_.opl3Active != 0) {
      chip_.GenerateBlock3(n, opl_buf);
    } else {
      chip_.GenerateBlock2(n, opl_buf);
    }

    std::uint32_t w = write_pos_.load(std::memory_order_relaxed);
    const std::uint32_t r = read_pos_.load(std::memory_order_acquire);

    for (std::uint32_t i = 0; i < n; ++i) {
      std::int32_t sl = 0;
      std::int32_t sr = 0;
      if (chip_.opl3Active != 0) {
        sl = opl_buf[i * 2 + 0] * 2;
        sr = opl_buf[i * 2 + 1] * 2;
      } else {
        sl = opl_buf[i] * 2;
        sr = sl;
      }

      // Soft-envelope PC speaker mix so port 0x61 clicks never pop.
      if (spk_on) {
        spk_env_ = std::min(1.0f, spk_env_ + env_step);
      } else {
        spk_env_ = std::max(0.0f, spk_env_ - env_step);
      }
      if (spk_env_ > 0.001f) {
        spk_phase_ += spk_inc;
        if (spk_phase_ >= 1.0) {
          spk_phase_ -= 1.0;
        }
        const float wave = (spk_phase_ < 0.5) ? 1.0f : -1.0f;
        const auto beep = static_cast<std::int32_t>(wave * spk_env_ * 1600.0f);
        sl += beep;
        sr += beep;
      }

      const auto cl = static_cast<std::int16_t>(std::clamp(sl, -32768, 32767));
      const auto cr = static_cast<std::int16_t>(std::clamp(sr, -32768, 32767));
      const std::int32_t stereo_abs =
          std::max(std::abs(static_cast<std::int32_t>(cl)),
                   std::abs(static_cast<std::int32_t>(cr)));
      local_peak = std::max(local_peak, stereo_abs);

      // Prevent ring overrun / latency drift if consumer is temporarily paused.
      if ((w - r) < kMaxBufferedFrames) {
        const std::uint32_t idx = (w & kRingMask) * 2u;
        ring_[idx + 0] = cl;
        ring_[idx + 1] = cr;
        ++w;
      }
    }

    write_pos_.store(w, std::memory_order_release);
    total_rendered_frames_.fetch_add(n, std::memory_order_relaxed);
  }

  peak_abs_sample_.store(local_peak, std::memory_order_relaxed);
}

std::uint32_t Opl2Audio::pull_stereo_s16(std::int16_t* dst_interleaved,
                                         std::uint32_t frames) {
  std::uint32_t r = read_pos_.load(std::memory_order_relaxed);
  const std::uint32_t w = write_pos_.load(std::memory_order_acquire);
  const std::uint32_t avail = std::min(frames, w - r);

  for (std::uint32_t i = 0; i < avail; ++i) {
    const std::uint32_t idx = ((r + i) & kRingMask) * 2u;
    last_sample_l_ = ring_[idx + 0];
    last_sample_r_ = ring_[idx + 1];
    dst_interleaved[i * 2 + 0] = last_sample_l_;
    dst_interleaved[i * 2 + 1] = last_sample_r_;
  }
  read_pos_.store(r + avail, std::memory_order_release);

  // Smooth exponential decay to zero on underrun (zero clicks/pops).
  for (std::uint32_t i = avail; i < frames; ++i) {
    last_sample_l_ = static_cast<std::int16_t>((last_sample_l_ * 31) / 32);
    last_sample_r_ = static_cast<std::int16_t>((last_sample_r_ * 31) / 32);
    dst_interleaved[i * 2 + 0] = last_sample_l_;
    dst_interleaved[i * 2 + 1] = last_sample_r_;
  }
  return avail;
}

}  // namespace san4
