#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

#include "frame_profiler.h"
#include "host_channel.h"
#include "upscaler.h"
#include "video.h"

namespace san4 {

class ThreadPool;

// Lock-free triple buffer of finished ARGB frames (output resolution) from
// the emulation thread to the main thread, with per-tile change tracking so
// both sides copy only what changed.
//
// Change tracking uses the 16x16 logical tile grid of VesaPlanarVideo (tile
// `t = ty * kTilesX + tx`). Logical tile `t` maps to the output block
// `[16 s tx, 16 s (tx + 1)) x [16 s ty, 16 s (ty + 1))` clipped to the frame,
// where `s` is the frame's scale factor; the last tile row of an 800x600
// frame is therefore only 8 s rows tall.
//
// Every publish bumps a generation counter and stamps the tiles it changed
// with it. Each slot remembers the generation of every tile it holds, so a
// publish copies only tiles newer than the slot's copy, and the consumer
// uploads only tiles newer than what it uploaded last; frames the consumer
// never acquired are not lost, their tiles still carry newer generations.
//
// Threading: exactly one producer thread calls publish(); exactly one
// consumer thread calls acquire() / wait_for_fresh_until(). A slot is owned
// by one side at a time and changes hands through one atomic exchange.
class FrameMailbox {
 public:
  static constexpr int kTileSize = VesaPlanarVideo::kTileSize;
  static constexpr int kTilesX = VesaPlanarVideo::kTilesX;
  static constexpr int kTilesY = VesaPlanarVideo::kTilesY;
  static constexpr int kTileCount = VesaPlanarVideo::kTileCount;

  struct Slot {
    int w = 0;
    int h = 0;
    UpscaleMode mode = UpscaleMode::Sharp2x;
    int scale = 1;
    std::vector<std::uint32_t> argb;
    // Generation of the newest content of each tile held by this slot.
    std::array<std::uint64_t, kTileCount> tile_gen{};
    // Generation of the publish that filled this slot (1-based).
    std::uint64_t gen = 0;
    HostStatus status{};
    // Producer-side timings and counters of the frame (`frame`,
    // `upload_ms`, `present_ms` and `dirty_px` are left for the consumer).
    FrameProfileRow profile{};
  };

  FrameMailbox();

  FrameMailbox(const FrameMailbox&) = delete;
  FrameMailbox& operator=(const FrameMailbox&) = delete;

  // Splits publish copies across `pool` (nullptr copies on the producer).
  // The pool must outlive every publish call.
  void set_thread_pool(ThreadPool* pool) noexcept { pool_ = pool; }

  // Producer: publishes the complete frame `argb` (`w x h`, `mode`), of
  // which only the output pixels inside `dirty` changed since the previous
  // publish. A change of `w`, `h`, or `mode` marks every tile changed.
  void publish(std::span<const std::uint32_t> argb, int w, int h,
               UpscaleMode mode, std::span<const UpscaleDirtyRect> dirty,
               const HostStatus& status, const FrameProfileRow& profile);

  // Producer: generation of the latest publish (0 before the first).
  std::uint64_t published_gen() const noexcept { return gen_; }

  // Consumer: the newest published frame if one arrived since the previous
  // acquire, else nullptr. The slot stays valid and unchanged until the
  // next acquire.
  const Slot* acquire();

  // Consumer: the slot most recently returned by acquire(), or nullptr before
  // the first acquire. Safe to read on the consumer thread between acquire()
  // calls (e.g. to re-upload after a renderer/device reset).
  const Slot* current_acquired() const noexcept;

  // Consumer: blocks until a frame is ready to acquire, notify_waiters() is
  // called, or `deadline` passes. Returns true when a frame is ready.
  bool wait_for_fresh_until(std::chrono::steady_clock::time_point deadline);

  // Wakes a consumer blocked in wait_for_fresh_until (e.g. on shutdown).
  void notify_waiters();

  // Output rect of logical tile `tile` in a `w x h` frame at `scale`
  // (`has_changes == false` when the tile lies outside the frame).
  static UpscaleDirtyRect tile_output_rect(int tile, int scale, int w, int h);

  // Appends to `out` (after clearing it) the disjoint output rects of every
  // tile of `slot` newer than `since_gen`: horizontally adjacent tiles of a
  // tile row form one run, and runs with the same x-span in consecutive tile
  // rows merge into one rect. Returns the number of covered pixels.
  static std::uint64_t changed_rects(const Slot& slot, std::uint64_t since_gen,
                                     std::vector<UpscaleDirtyRect>& out);

 private:
  static constexpr std::uint32_t kIndexMask = 3u;
  static constexpr std::uint32_t kFresh = 4u;

  void mark_dirty_tiles(std::span<const UpscaleDirtyRect> dirty, int scale,
                        int w, int h);
  void copy_tiles(Slot& slot, const std::uint32_t* src);

  std::array<Slot, 3> slots_{};
  // Index of the slot ready for the consumer (bits 0-1) and whether it holds
  // a frame the consumer has not acquired yet (bit 2).
  std::atomic<std::uint32_t> state_{1u};

  // Producer-only state.
  std::uint32_t write_ = 0;
  std::uint64_t gen_ = 0;
  int cur_w_ = 0;
  int cur_h_ = 0;
  UpscaleMode cur_mode_ = UpscaleMode::Sharp2x;
  std::array<std::uint64_t, kTileCount> master_tile_gen_{};
  ThreadPool* pool_ = nullptr;

  // Consumer-only state.
  std::uint32_t read_ = 2;

  // Wakeup for wait_for_fresh_until.
  std::mutex wait_mu_;
  std::condition_variable wait_cv_;
  bool wake_ = false;
};

}  // namespace san4
