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

// Producer-side inputs for one GPU frame publish.
struct GpuVideoInput {
  int src_w = VesaPlanarVideo::kWidth;
  int src_h = VesaPlanarVideo::kHeight;
  UpscaleMode mode = UpscaleMode::Sharp2x;
  const std::uint8_t* vram = nullptr;             // 800 x 600 (stride 800)
  const std::uint64_t* vram_tile_gen = nullptr;   // kTileCount
  const std::array<std::uint32_t, 16>* palette_argb = nullptr;
  std::uint64_t palette_gen = 0;
  const std::uint8_t* glyph_tiles = nullptr;      // kTileCount
  const std::uint64_t* glyph_tile_gen = nullptr;  // kTileCount
  std::uint64_t glyph_gen = 0;
};

struct GpuHdInput {
  static constexpr std::size_t kLutGridSize = 17u * 17u * 17u;
  static constexpr std::size_t kMaxKeySlots = 256u;

  bool active = false;
  const std::uint32_t* canvas_argb = nullptr;        // 3200 x 2400 when active
  const std::uint64_t* canvas_tile_gen = nullptr;    // kTileCount
  const std::uint16_t* lut_idx_map = nullptr;        // 800 x 600 (0=none, 1..256=slot)
  const std::uint64_t* lut_idx_tile_gen = nullptr;   // kTileCount
  std::span<const std::array<std::uint32_t, kLutGridSize>> luts{};
  const int* lut_slot_map = nullptr;                 // 256 ints (-1=identity, >=0=lut idx)
  std::uint64_t luts_gen = 0;
};

// Lock-free triple buffer of compact GPU scene state from EmuThread to the
// main OpenGL thread. Uses a single unified publish generation (`gen`) across
// all per-tile and per-uniform timestamps so the consumer can track independent
// upload cursors (`uploaded_vram_gen`, `uploaded_hd_gen`, `composed_gen`, etc.)
// and skip redundant PCIe uploads across skipped frames, F5 scale changes, and
// 60-144 Hz palette crossfades.
class GpuFrameMailbox {
 public:
  static constexpr int kTileSize = VesaPlanarVideo::kTileSize;
  static constexpr int kTilesX = VesaPlanarVideo::kTilesX;
  static constexpr int kTilesY = VesaPlanarVideo::kTilesY;
  static constexpr int kTileCount = VesaPlanarVideo::kTileCount;
  static constexpr int kHdScale = 4;
  static constexpr int kHdCanvasW = VesaPlanarVideo::kWidth * kHdScale;
  static constexpr int kHdCanvasH = VesaPlanarVideo::kHeight * kHdScale;
  static constexpr std::size_t kLutGridSize = GpuHdInput::kLutGridSize;
  static constexpr std::size_t kMaxKeySlots = GpuHdInput::kMaxKeySlots;

  struct Slot {
    int src_w = VesaPlanarVideo::kWidth;
    int src_h = VesaPlanarVideo::kHeight;
    int out_w = VesaPlanarVideo::kWidth * 2;
    int out_h = VesaPlanarVideo::kHeight * 2;
    UpscaleMode mode = UpscaleMode::Sharp2x;
    int scale = 2;

    std::array<std::uint8_t, VesaPlanarVideo::kWidth * VesaPlanarVideo::kHeight>
        vram{};
    std::array<std::uint64_t, kTileCount> vram_tile_gen{};

    std::array<std::uint32_t, 16> palette_argb{};
    std::uint64_t palette_gen = 0;

    std::array<std::uint8_t, kTileCount> glyph_tiles{};
    std::uint64_t glyph_gen = 0;

    bool hd_active = false;
    // Lazily sized to 3200*2400 only when hd_active becomes true.
    std::vector<std::uint32_t> hd_canvas_argb;
    std::array<std::uint64_t, kTileCount> hd_tile_gen{};
    std::uint64_t hd_epoch = 0;

    std::vector<std::uint16_t> hd_lut_idx;  // 800*600 when hd_active
    std::array<std::uint64_t, kTileCount> lut_idx_tile_gen{};

    std::vector<std::array<std::uint32_t, kLutGridSize>> luts;
    std::array<int, kMaxKeySlots> lut_slot_map{};
    std::uint64_t luts_gen = 0;

    // Output recompose tracking: per-tile changes (VRAM, glyph, HD, LUT-idx)
    // plus full-frame recompose events (palette, LUT values, mode, size, HD on/off).
    std::array<std::uint64_t, kTileCount> compose_tile_gen{};
    std::uint64_t full_recompose_gen = 0;

    std::uint64_t gen = 0;
    HostStatus status{};
    FrameProfileRow profile{};
  };

  GpuFrameMailbox();

  GpuFrameMailbox(const GpuFrameMailbox&) = delete;
  GpuFrameMailbox& operator=(const GpuFrameMailbox&) = delete;

  void set_thread_pool(ThreadPool* pool) noexcept { pool_ = pool; }

  void publish(const GpuVideoInput& video, const GpuHdInput& hd,
               bool force_full, const HostStatus& status,
               const FrameProfileRow& profile);

  std::uint64_t published_gen() const noexcept { return gen_; }

  const Slot* acquire();
  const Slot* current_acquired() const noexcept;

  bool wait_for_fresh_until(std::chrono::steady_clock::time_point deadline);
  void notify_waiters();

  // Emits pairwise-disjoint rectangles (in pixel coordinates of a `w x h`
  // surface where each logical tile covers `tile_px x tile_px` pixels) for all
  // tiles in `0..tiles_x*tiles_y` with `tile_gen[t] > since_gen`, expanded by
  // `halo_px` on each side and clamped to `[0, w) x [0, h)`. Returns total
  // covered pixels in `out`.
  static std::uint64_t changed_tile_rects(
      const std::array<std::uint64_t, kTileCount>& tile_gen,
      std::uint64_t since_gen, int tiles_x, int tiles_y, int tile_px, int w,
      int h, int halo_px, std::vector<UpscaleDirtyRect>& out);

 private:
  static constexpr std::uint32_t kIndexMask = 3u;
  static constexpr std::uint32_t kFresh = 4u;

  void copy_vram_tiles(Slot& slot, const std::uint8_t* src_vram);
  void copy_hd_tiles(Slot& slot, const std::uint32_t* src_hd,
                     const std::uint16_t* src_lut_idx);

  std::array<Slot, 3> slots_{};
  std::atomic<std::uint32_t> state_{1u};

  // Producer-only state.
  std::uint32_t write_ = 0;
  std::uint64_t gen_ = 0;
  int cur_src_w_ = 0;
  int cur_src_h_ = 0;
  UpscaleMode cur_mode_ = UpscaleMode::Sharp2x;
  bool cur_hd_active_ = false;
  std::uint64_t hd_epoch_ = 1;

  std::array<std::uint64_t, kTileCount> seen_src_vram_gen_{};
  std::array<std::uint64_t, kTileCount> seen_src_glyph_tile_gen_{};
  std::uint64_t seen_src_palette_gen_ = 0;
  std::uint64_t seen_src_glyph_gen_ = 0;
  std::array<std::uint64_t, kTileCount> seen_src_hd_gen_{};
  std::array<std::uint64_t, kTileCount> seen_src_lut_idx_gen_{};
  std::uint64_t seen_src_luts_gen_ = 0;

  std::array<std::uint64_t, kTileCount> master_vram_tile_gen_{};
  std::uint64_t master_palette_gen_ = 0;
  std::uint64_t master_glyph_gen_ = 0;
  std::array<std::uint64_t, kTileCount> master_hd_tile_gen_{};
  std::array<std::uint64_t, kTileCount> master_lut_idx_tile_gen_{};
  std::uint64_t master_luts_gen_ = 0;
  std::array<std::uint64_t, kTileCount> master_compose_tile_gen_{};
  std::uint64_t master_full_recompose_gen_ = 0;

  ThreadPool* pool_ = nullptr;

  // Consumer-only state.
  std::uint32_t read_ = 2;

  std::mutex wait_mu_;
  std::condition_variable wait_cv_;
  bool wake_ = false;
};

}  // namespace san4
