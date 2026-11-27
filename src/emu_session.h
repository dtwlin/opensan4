#pragma once

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <span>
#include <thread>
#include <vector>

#include "frame_mailbox.h"
#include "frame_profiler.h"
#include "gpu/gpu_frame_mailbox.h"
#include "host_channel.h"
#include "machine.h"
#include "render_policy.h"
#include "upscaler.h"

namespace san4 {

// Discards up to 2048 buffered audio frames; keeps the OPL2 ring from
// filling when no audio device consumes it.
void discard_buffered_audio(Opl2Audio& audio);

// The emulation side of the host: applies HostCmds to the machine and
// upscaler, runs guest frames, and produces finished ARGB frames (upscale,
// HD composite, dirty rects). Headless runs drive it inline from the main
// thread; interactive runs drive it from EmuThread, which then exclusively
// owns the machine, the upscaler and the HD pack. No SDL dependency.
class EmuSession {
 public:
  // Inputs of the automatic output-scale policy that do not change at
  // runtime (see select_auto_upscale_mode).
  struct AutoScalePolicy {
    RenderProfile profile = RenderProfile::Hardware;
    bool hd_pack = false;
    bool explicit_scale = false;
  };

  struct LoopOptions {
    // Guest CPU budget per emulated frame (run_frame's second argument).
    double guest_budget_sec = 0.012;
    // Discard synthesized audio each frame (no audio device is open).
    bool drain_audio = false;
    // Record per-frame timings into the published FrameProfileRow.
    bool profiling = false;
  };

  // Host frame period at 1x speed, and the shortest emulated frame: run_loop
  // clamps `dt` to [kMinFrameSec, kMaxFrameSec].
  static constexpr double kFrameSec = 1.0 / 60.0;
  static constexpr double kMinFrameSec = 1.0 / 240.0;
  static constexpr double kMaxFrameSec = 0.050;

  EmuSession(X86Machine& machine, PixelUpscaler& upscaler);

  EmuSession(const EmuSession&) = delete;
  EmuSession& operator=(const EmuSession&) = delete;

  void set_auto_scale_policy(const AutoScalePolicy& policy) {
    auto_scale_ = policy;
  }

  // Applies one host command. Quit only sets quit_requested().
  void apply(const HostCmd& cmd);
  bool quit_requested() const { return quit_; }
  // True once F5 or a SetScale fallback pinned the scale; AutoScale commands
  // are then ignored.
  bool user_cycled_scale() const { return user_cycled_scale_; }
  bool drain_audio() const { return drain_audio_; }

  // run_frame, timed into `row->run_frame_ms` and the guest counters of
  // `row` when `row` is non-null.
  void run_guest_frame(double dt_sec, double budget_sec, FrameProfileRow* row);

  // Upscales the latched frame (all of it when `force_full`, which is then
  // cleared), composites HD overlays, and leaves the disjoint changed output
  // rects in frame_rects(). Fills `upscale_ms`, `composite_ms` and
  // `dirty_tiles` of `row` when non-null.
  void produce_frame(bool& force_full, FrameProfileRow* row);
  // GPU-pipeline variant: prepares HD metadata/LUTs without CPU upscaling or
  // CPU 3200x2400 blending and publishes compact scene state to `mailbox`.
  void produce_gpu_frame(bool& force_full, GpuFrameMailbox& mailbox,
                         FrameProfileRow* row);
  std::span<const UpscaleDirtyRect> frame_rects() const { return frame_rects_; }

  HostStatus status() const;

  // Emulation-thread body: per tick, drain `cmds`, run a guest frame,
  // produce and publish it to `mailbox`, and pace to 60 Hz at 1x (back to
  // back in turbo). Returns after a Quit command or when the guest exits;
  // the frame that observed the exit is published first.
  void run_loop(FrameMailbox& mailbox, HostCommandQueue& cmds,
                const LoopOptions& opts);
  // Same, and `stop` becoming true acts like a Quit command (observed at the
  // start of the next tick).
  void run_loop(FrameMailbox& mailbox, HostCommandQueue& cmds,
                const LoopOptions& opts, const std::atomic<bool>& stop);

  // OpenGL 3.3 GPU-pipeline emulation-thread loop publishing to `GpuFrameMailbox`.
  void run_gpu_loop(GpuFrameMailbox& mailbox, HostCommandQueue& cmds,
                    const LoopOptions& opts);
  void run_gpu_loop(GpuFrameMailbox& mailbox, HostCommandQueue& cmds,
                    const LoopOptions& opts, const std::atomic<bool>& stop);

 private:
  void apply_mouse_motion(int x, int y);

  X86Machine& machine_;
  PixelUpscaler& upscaler_;
  AutoScalePolicy auto_scale_{};
  bool user_cycled_scale_ = false;
  bool drain_audio_ = false;
  bool quit_ = false;
  bool gpu_hd_was_active_ = false;
  bool middle_dragging_ = false;
  int middle_drag_accum_x_ = 0;
  int middle_drag_accum_y_ = 0;
  std::vector<UpscaleDirtyRect> frame_rects_;
};

// Runs EmuSession::run_loop on a dedicated thread.
class EmuThread {
 public:
  // Thread body for the generic start(): must return soon after its
  // argument (the stop request) becomes true.
  using Body = std::function<void(const std::atomic<bool>& stop)>;

  EmuThread() = default;
  // Same as shutdown().
  ~EmuThread();

  EmuThread(const EmuThread&) = delete;
  EmuThread& operator=(const EmuThread&) = delete;

  void start(EmuSession& session, FrameMailbox& mailbox, HostCommandQueue& cmds,
             const EmuSession::LoopOptions& opts);
  void start(EmuSession& session, GpuFrameMailbox& mailbox,
             HostCommandQueue& cmds, const EmuSession::LoopOptions& opts);
  // Runs `body` instead of EmuSession::run_loop. When it returns or throws,
  // finished() becomes true and `mailbox` waiters are woken.
  void start(Body body, FrameMailbox& mailbox);
  void start(Body body, GpuFrameMailbox& mailbox);
  bool running() const { return thread_.joinable(); }
  // True once the loop returned (Quit, guest exit, or an exception).
  bool finished() const { return finished_.load(std::memory_order_acquire); }
  // Requests a stop (the session then reports quit_requested()) and joins;
  // an exception that ended the loop is kept for stop(). Allocation-free.
  void shutdown() noexcept;
  // shutdown(), then rethrows an exception that ended the loop.
  void stop();

 private:
  std::thread thread_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> finished_{false};
  std::exception_ptr error_;
};

}  // namespace san4
