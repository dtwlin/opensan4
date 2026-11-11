#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace san4 {

// One `--profile-frames` TSV row.
//
// Headless: one row per presented frame, `frame` = presented frame index.
// Interactive (emulation thread): one row per published frame the main
// thread acquired. `frame` is the emulator frame index, so gaps mark frames
// superseded before the main thread picked them up (their changes are still
// uploaded). `guest_instrs` .. `composite_ms` and `dirty_tiles` are that
// frame's emulation-thread values; `upload_ms`, `present_ms` and `dirty_px`
// are main-thread values, with `present_ms = 0` when nothing changed and the
// present was skipped.
struct FrameProfileRow {
  std::uint64_t frame = 0;
  std::uint64_t guest_instrs = 0;
  double run_frame_ms = 0.0;
  bool budget_capped = false;
  const char* yield_reason = "none";
  double upscale_ms = 0.0;
  double composite_ms = 0.0;
  double upload_ms = 0.0;
  double present_ms = 0.0;
  int dirty_tiles = 0;
  std::uint64_t dirty_px = 0;
};

// Per-frame timing TSV writer. Callers guard all timing work behind
// enabled(), so a disabled profiler costs one branch per stage.
class FrameProfiler {
 public:
  using Clock = std::chrono::steady_clock;

  static double ms_between(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  }

  // Opens `path` for writing and emits the header row.
  bool open(const std::filesystem::path& path) {
    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_) {
      return false;
    }
    out_ << "frame\tguest_instrs\trun_frame_ms\tbudget_capped\tyield_reason\t"
            "upscale_ms\tcomposite_ms\tupload_ms\tpresent_ms\tdirty_tiles\t"
            "dirty_px\n";
    return static_cast<bool>(out_);
  }

  bool enabled() const { return out_.is_open(); }

  void write(const FrameProfileRow& r) {
    char line[256];
    std::snprintf(line, sizeof(line),
                  "%llu\t%llu\t%.3f\t%d\t%s\t%.3f\t%.3f\t%.3f\t%.3f\t%d\t%llu\n",
                  static_cast<unsigned long long>(r.frame),
                  static_cast<unsigned long long>(r.guest_instrs),
                  r.run_frame_ms, r.budget_capped ? 1 : 0, r.yield_reason,
                  r.upscale_ms, r.composite_ms, r.upload_ms, r.present_ms,
                  r.dirty_tiles, static_cast<unsigned long long>(r.dirty_px));
    out_ << line;
  }

  void flush() { out_.flush(); }

 private:
  std::ofstream out_;
};

}  // namespace san4
