#include "emu_session.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace san4 {

void discard_buffered_audio(Opl2Audio& audio) {
  std::int16_t scratch[2048 * 2];
  const std::uint32_t avail = audio.buffered_frames();
  if (avail > 0u) {
    audio.pull_stereo_s16(scratch, std::min<std::uint32_t>(avail, 2048u));
  }
}

EmuSession::EmuSession(X86Machine& machine, PixelUpscaler& upscaler)
    : machine_(machine), upscaler_(upscaler) {}

void EmuSession::apply_mouse_motion(int x, int y) {
  const int prev_x = machine_.mouse().x();
  const int prev_y = machine_.mouse().y();
  machine_.mouse().set_position(x, y);
  if (!middle_dragging_) {
    return;
  }
  // Middle-drag pans the map: every 24 DOS pixels of travel is one arrow key.
  middle_drag_accum_x_ += (x - prev_x);
  middle_drag_accum_y_ += (y - prev_y);
  constexpr int kPanStepPx = 24;
  DosKernel& dos = machine_.dos();
  while (middle_drag_accum_x_ >= kPanStepPx) {
    dos.push_key(0x00u, 0x4Du);
    middle_drag_accum_x_ -= kPanStepPx;
  }
  while (middle_drag_accum_x_ <= -kPanStepPx) {
    dos.push_key(0x00u, 0x4Bu);
    middle_drag_accum_x_ += kPanStepPx;
  }
  while (middle_drag_accum_y_ >= kPanStepPx) {
    dos.push_key(0x00u, 0x50u);
    middle_drag_accum_y_ -= kPanStepPx;
  }
  while (middle_drag_accum_y_ <= -kPanStepPx) {
    dos.push_key(0x00u, 0x48u);
    middle_drag_accum_y_ += kPanStepPx;
  }
}

void EmuSession::apply(const HostCmd& cmd) {
  switch (cmd.type) {
    case HostCmdType::PushKey:
      machine_.dos().push_key(static_cast<std::uint8_t>(cmd.a),
                              static_cast<std::uint8_t>(cmd.b));
      return;
    case HostCmdType::SetShiftFlags:
      machine_.dos().set_shift_flags(static_cast<std::uint8_t>(cmd.a));
      return;
    case HostCmdType::MouseMotion:
      apply_mouse_motion(cmd.a, cmd.b);
      return;
    case HostCmdType::MousePos:
      machine_.mouse().set_position(cmd.a, cmd.b);
      return;
    case HostCmdType::MouseButton: {
      const bool pressed = cmd.b != 0;
      if (cmd.a == 2) {
        middle_dragging_ = pressed;
        middle_drag_accum_x_ = 0;
        middle_drag_accum_y_ = 0;
      }
      machine_.mouse().set_button(cmd.a, pressed);
      return;
    }
    case HostCmdType::SetTurbo:
      machine_.set_turbo_held(cmd.a != 0);
      return;
    case HostCmdType::CycleSpeed:
      machine_.cycle_base_speed_multiplier();
      return;
    case HostCmdType::SetPackDir:
      machine_.hd_pack().set_pack_dir(cmd.path);
      auto_scale_.hd_pack = true;
      return;
    case HostCmdType::ReloadPack:
      machine_.hd_pack().reload_pack();
      return;
    case HostCmdType::ToggleDump:
      machine_.hd_pack().toggle_dumping(cmd.path);
      return;
    case HostCmdType::MarkAllDirty:
      machine_.video().mark_all_dirty();
      return;
    case HostCmdType::CycleScale:
      upscaler_.cycle_mode();
      user_cycled_scale_ = true;
      return;
    case HostCmdType::SetScale:
      // Only sent as a fallback when the host cannot create a texture for
      // the current mode: pin it so AutoScale cannot re-select that mode.
      upscaler_.set_mode(static_cast<UpscaleMode>(
          std::clamp(cmd.a, static_cast<int>(UpscaleMode::Nearest),
                     static_cast<int>(UpscaleMode::Xbr4x))));
      user_cycled_scale_ = true;
      return;
    case HostCmdType::AutoScale: {
      const UpscaleMode next = select_auto_upscale_mode(AutoScaleInputs{
          .profile = auto_scale_.profile,
          .drawable_h = cmd.a,
          .hd_pack = auto_scale_.hd_pack,
          .explicit_scale = auto_scale_.explicit_scale,
          .user_cycled = user_cycled_scale_,
          .current = upscaler_.mode(),
      });
      if (next != upscaler_.mode()) {
        upscaler_.set_mode(next);
      }
      return;
    }
    case HostCmdType::SetDrainAudio:
      drain_audio_ = (cmd.a != 0);
      return;
    case HostCmdType::Quit:
      quit_ = true;
      return;
  }
}

void EmuSession::run_guest_frame(double dt_sec, double budget_sec,
                                 FrameProfileRow* row) {
  if (row == nullptr) {
    machine_.run_frame(dt_sec, budget_sec);
    return;
  }
  const auto t0 = FrameProfiler::Clock::now();
  machine_.run_frame(dt_sec, budget_sec);
  row->run_frame_ms = FrameProfiler::ms_between(t0, FrameProfiler::Clock::now());
  const GuestFrameStats& gs = machine_.last_frame_stats();
  row->guest_instrs = gs.guest_instrs;
  row->budget_capped = (gs.yield_reason == GuestYieldReason::Budget);
  row->yield_reason = guest_yield_reason_name(gs.yield_reason);
}

void EmuSession::produce_frame(bool& force_full, FrameProfileRow* row) {
  FrameProfiler::Clock::time_point t_upscale0;
  if (row != nullptr) {
    t_upscale0 = FrameProfiler::Clock::now();
  }
  // HD repaints (occlusions, async CG results, dynamic-CG refreshes) must
  // dirty their tiles before this frame's upscale.
  machine_.hd_pack().prepare_frame(machine_.video());
  const UpscaleDirtyRect dirty =
      upscaler_.upscale_from_video(machine_.video(), force_full);
  force_full = false;
  const std::span<const UpscaleDirtyRect> rects = upscaler_.dirty_rects();

  FrameProfiler::Clock::time_point t_composite0;
  if (row != nullptr) {
    t_composite0 = FrameProfiler::Clock::now();
  }
  const HdPackManager& hd = machine_.hd_pack();
  if ((dirty.has_changes && hd.active_overlay_count() > 0u) ||
      hd.has_pending_hd_changes()) {
    // Blends each pixel of the (possibly overlapping) spans exactly once and
    // adds HD-only changes; frame_rects_ comes back disjoint.
    hd.composite_overlays(upscaler_.mutable_output_argb(),
                          upscaler_.output_width(), upscaler_.output_height(),
                          upscaler_.scale_factor(), machine_.video(), rects,
                          frame_rects_);
  } else {
    normalize_dirty_rects(rects, upscaler_.output_width(),
                          upscaler_.output_height(), frame_rects_);
  }

  if (row != nullptr) {
    const auto t_end = FrameProfiler::Clock::now();
    row->upscale_ms = FrameProfiler::ms_between(t_upscale0, t_composite0);
    row->composite_ms = FrameProfiler::ms_between(t_composite0, t_end);
    row->dirty_tiles = upscaler_.last_dirty_tile_count();
  }
}

void EmuSession::produce_gpu_frame(bool& force_full, GpuFrameMailbox& mailbox,
                                   FrameProfileRow* row) {
  machine_.hd_pack().prepare_frame(machine_.video());

  std::array<std::uint8_t, VesaPlanarVideo::kTileCount> dirty{};
  bool pal_changed = false;
  machine_.video().consume_dirty_tiles(dirty, pal_changed);
  const int active_w = machine_.video().latched_width();
  const int active_h = machine_.video().latched_height();
  const int tiles_x =
      (active_w + VesaPlanarVideo::kTileSize - 1) / VesaPlanarVideo::kTileSize;
  const int tiles_y =
      (active_h + VesaPlanarVideo::kTileSize - 1) / VesaPlanarVideo::kTileSize;
  int dirty_count = 0;
  if (force_full) {
    dirty_count = tiles_x * tiles_y;
  } else {
    for (int ty = 0; ty < tiles_y; ++ty) {
      const int row_base = ty * VesaPlanarVideo::kTilesX;
      for (int tx = 0; tx < tiles_x; ++tx) {
        if (dirty[static_cast<std::size_t>(row_base + tx)] != 0u) {
          ++dirty_count;
        }
      }
    }
  }

  FrameProfiler::Clock::time_point t_composite0;
  if (row != nullptr) {
    t_composite0 = FrameProfiler::Clock::now();
  }
  GpuHdInput gpu_hd{};
  const HdPackManager& hd = machine_.hd_pack();
  if (hd.active_overlay_count() > 0u || hd.has_pending_hd_changes() ||
      gpu_hd_was_active_) {
    hd.prepare_gpu_composite_state(machine_.video(), force_full, gpu_hd);
  }
  gpu_hd_was_active_ = gpu_hd.active;

  if (row != nullptr) {
    row->upscale_ms = 0.0;
    row->composite_ms =
        FrameProfiler::ms_between(t_composite0, FrameProfiler::Clock::now());
    row->dirty_tiles = dirty_count;
  }

  const VesaPlanarVideo& vid = machine_.video();
  const GpuVideoInput gpu_vid{
      .src_w = active_w,
      .src_h = active_h,
      .mode = upscaler_.mode(),
      .vram = vid.latched_vram().data(),
      .vram_tile_gen = vid.latched_vram_tile_gen().data(),
      .palette_argb = &vid.latched_palette_argb(),
      .palette_gen = vid.palette_generation(),
      .glyph_tiles = vid.latched_glyph_tiles().data(),
      .glyph_tile_gen = vid.latched_glyph_tile_gen().data(),
      .glyph_gen = vid.latched_glyph_generation(),
  };
  mailbox.publish(gpu_vid, gpu_hd, force_full, status(),
                  row != nullptr ? *row : FrameProfileRow{});
  force_full = false;
}

HostStatus EmuSession::status() const {
  return HostStatus{
      .mode = upscaler_.mode(),
      .speed_multiplier = machine_.effective_speed_multiplier(),
      .hd_loaded = machine_.hd_pack().loaded_texture_count(),
      .hd_dumping = machine_.hd_pack().dumping_enabled(),
      .exited = machine_.exited(),
      .active_w = machine_.video().active_width(),
      .active_h = machine_.video().active_height(),
  };
}

void EmuSession::run_loop(FrameMailbox& mailbox, HostCommandQueue& cmds,
                          const LoopOptions& opts) {
  const std::atomic<bool> never_stop{false};
  run_loop(mailbox, cmds, opts, never_stop);
}

void EmuSession::run_loop(FrameMailbox& mailbox, HostCommandQueue& cmds,
                          const LoopOptions& opts,
                          const std::atomic<bool>& stop) {
  using Clock = std::chrono::steady_clock;
  const auto tick = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double>(kFrameSec));
  const auto min_frame = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double>(kMinFrameSec));
  bool force_full = true;
  drain_audio_ = opts.drain_audio;
  std::vector<HostCmd> batch;
  auto prev = Clock::now();
  auto next_tick = prev + tick;

  while (true) {
    const auto start = Clock::now();
    const double dt = std::clamp(
        std::chrono::duration<double>(start - prev).count(), kMinFrameSec,
        kMaxFrameSec);
    prev = start;

    cmds.drain(batch);
    for (const HostCmd& cmd : batch) {
      apply(cmd);
    }
    if (stop.load(std::memory_order_acquire)) {
      quit_ = true;
    }
    if (quit_) {
      return;
    }

    FrameProfileRow row{};
    FrameProfileRow* const row_ptr = opts.profiling ? &row : nullptr;
    run_guest_frame(dt, opts.guest_budget_sec, row_ptr);
    produce_frame(force_full, row_ptr);
    mailbox.publish(upscaler_.output_argb(), upscaler_.output_width(),
                    upscaler_.output_height(), upscaler_.mode(), frame_rects_,
                    status(), row);
    if (drain_audio_) {
      discard_buffered_audio(machine_.audio());
    }
    if (machine_.exited()) {
      return;
    }

    const auto now = Clock::now();
    if (machine_.effective_speed_multiplier() > 1) {
      // Turbo runs bursts back to back, but never faster than the dt floor
      // so guest time (PIT, OPL2 tempo) cannot outrun the wall clock.
      std::this_thread::sleep_until(start + min_frame);
      next_tick = Clock::now() + tick;
      continue;
    }
    if (now < next_tick) {
      std::this_thread::sleep_until(next_tick);
    }
    next_tick += tick;
    // More than a tick behind (slow frame): resync instead of bursting.
    if (next_tick < now) {
      next_tick = now + tick;
    }
  }
}

void EmuSession::run_gpu_loop(GpuFrameMailbox& mailbox, HostCommandQueue& cmds,
                              const LoopOptions& opts) {
  const std::atomic<bool> never_stop{false};
  run_gpu_loop(mailbox, cmds, opts, never_stop);
}

void EmuSession::run_gpu_loop(GpuFrameMailbox& mailbox, HostCommandQueue& cmds,
                              const LoopOptions& opts,
                              const std::atomic<bool>& stop) {
  using Clock = std::chrono::steady_clock;
  const auto tick = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double>(kFrameSec));
  const auto min_frame = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double>(kMinFrameSec));
  bool force_full = true;
  drain_audio_ = opts.drain_audio;
  std::vector<HostCmd> batch;
  auto prev = Clock::now();
  auto next_tick = prev + tick;

  while (true) {
    const auto start = Clock::now();
    const double dt = std::clamp(
        std::chrono::duration<double>(start - prev).count(), kMinFrameSec,
        kMaxFrameSec);
    prev = start;

    cmds.drain(batch);
    for (const HostCmd& cmd : batch) {
      apply(cmd);
    }
    if (stop.load(std::memory_order_acquire)) {
      quit_ = true;
    }
    if (quit_) {
      return;
    }

    FrameProfileRow row{};
    FrameProfileRow* const row_ptr = opts.profiling ? &row : nullptr;
    run_guest_frame(dt, opts.guest_budget_sec, row_ptr);
    produce_gpu_frame(force_full, mailbox, row_ptr);
    if (drain_audio_) {
      discard_buffered_audio(machine_.audio());
    }
    if (machine_.exited()) {
      return;
    }

    const auto now = Clock::now();
    if (machine_.effective_speed_multiplier() > 1) {
      std::this_thread::sleep_until(start + min_frame);
      next_tick = Clock::now() + tick;
      continue;
    }
    if (now < next_tick) {
      std::this_thread::sleep_until(next_tick);
    }
    next_tick += tick;
    if (next_tick < now) {
      next_tick = now + tick;
    }
  }
}

EmuThread::~EmuThread() { shutdown(); }

void EmuThread::start(EmuSession& session, FrameMailbox& mailbox,
                      HostCommandQueue& cmds,
                      const EmuSession::LoopOptions& opts) {
  start(
      [&session, &mailbox, &cmds, opts](const std::atomic<bool>& stop) {
        session.run_loop(mailbox, cmds, opts, stop);
      },
      mailbox);
}

void EmuThread::start(EmuSession& session, GpuFrameMailbox& mailbox,
                      HostCommandQueue& cmds,
                      const EmuSession::LoopOptions& opts) {
  start(
      [&session, &mailbox, &cmds, opts](const std::atomic<bool>& stop) {
        session.run_gpu_loop(mailbox, cmds, opts, stop);
      },
      mailbox);
}

void EmuThread::start(Body body, FrameMailbox& mailbox) {
  stop_requested_.store(false, std::memory_order_release);
  finished_.store(false, std::memory_order_release);
  error_ = nullptr;
  thread_ = std::thread([this, body = std::move(body), &mailbox] {
    try {
      body(stop_requested_);
    } catch (...) {
      error_ = std::current_exception();
    }
    finished_.store(true, std::memory_order_release);
    mailbox.notify_waiters();
  });
}

void EmuThread::start(Body body, GpuFrameMailbox& mailbox) {
  stop_requested_.store(false, std::memory_order_release);
  finished_.store(false, std::memory_order_release);
  error_ = nullptr;
  thread_ = std::thread([this, body = std::move(body), &mailbox] {
    try {
      body(stop_requested_);
    } catch (...) {
      error_ = std::current_exception();
    }
    finished_.store(true, std::memory_order_release);
    mailbox.notify_waiters();
  });
}

void EmuThread::shutdown() noexcept {
  if (thread_.joinable()) {
    stop_requested_.store(true, std::memory_order_release);
    thread_.join();
  }
}

void EmuThread::stop() {
  shutdown();
  if (error_) {
    std::rethrow_exception(std::exchange(error_, nullptr));
  }
}

}  // namespace san4
