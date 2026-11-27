#pragma once

#include <array>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "util/function_ref.h"
#include "video.h"

namespace san4 {

class ThreadPool;

enum class UpscaleMode : int {
  Nearest = 0,   // 800x600 (1x)
  Sharp2x = 1,   // 1600x1200 (2x integer + GPU linear downsample = Sharp-Bilinear)
  Scale2x = 2,   // 1600x1200 (2x EPX)
  Xbr2x = 3,     // 1600x1200 (2x dither/glyph-guarded xBR)
  Xbr4x = 4,     // 3200x2400 (4x dither/glyph-guarded xBR)
};

struct UpscaleDirtyRect {
  bool has_changes = false;
  bool resized = false;
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

// Which canvas pixels an incremental `upscale_from_video` re-renders.
enum class UpscaleDirtyPolicy : std::uint8_t {
  // The bounding box of all dirty tiles (+2px halo). Every pixel inside the
  // returned union rect is fresh, which callers that composite in place over
  // the union rely on.
  BoundingBox = 0,
  // Per tile row, each run of dirty tiles (+2px halo) only. Pixels inside the
  // union but outside `dirty_rects()` keep their previous contents.
  TileRowSpans = 1,
};

// Rewrites `rects` (entries with `has_changes`, clipped to `[0, w) x [0, h)`)
// into `out` as pairwise-disjoint rectangles covering exactly the same pixels,
// with vertically adjacent identical spans merged. `out` must not alias
// `rects`. Returns the bounding box of `out` (`has_changes == false` when it
// is empty).
UpscaleDirtyRect normalize_dirty_rects(std::span<const UpscaleDirtyRect> rects,
                                       int w, int h,
                                       std::vector<UpscaleDirtyRect>& out);

class PixelUpscaler {
 public:
  explicit PixelUpscaler(int num_threads = 4);
  ~PixelUpscaler();

  PixelUpscaler(const PixelUpscaler&) = delete;
  PixelUpscaler& operator=(const PixelUpscaler&) = delete;

  void set_mode(UpscaleMode mode);
  UpscaleMode mode() const { return mode_; }
  UpscaleMode cycle_mode();

  // When disabled (OpenGL GPU pipeline), set_mode() updates dimensions/mode
  // without allocating the multi-megabyte CPU output canvas.
  void set_canvas_enabled(bool enabled);
  bool canvas_enabled() const noexcept { return canvas_enabled_; }

  static int scale_factor_for(UpscaleMode mode);
  static const char* mode_name(UpscaleMode mode);
  static bool parse_mode(std::string_view text, UpscaleMode& out_mode);

  int output_width() const { return out_w_; }
  int output_height() const { return out_h_; }
  int scale_factor() const { return scale_factor_for(mode_); }
  const std::vector<std::uint32_t>& output_argb() const { return canvas_; }
  std::vector<std::uint32_t>& mutable_output_argb() { return canvas_; }

  // Upscales only the dirty tile region (+2px halo) from video's latched frame
  // into canvas_, returning the union of the re-rendered output region. The
  // exact re-rendered rectangles are available from dirty_rects() afterwards.
  UpscaleDirtyRect upscale_from_video(VesaPlanarVideo& video,
                                      bool force_full = false);

  // Output-space rectangles re-rendered by the last upscale_from_video call
  // (empty when nothing changed). Under TileRowSpans these are the per-tile-
  // row spans, which may overlap vertically by the halo; otherwise a single
  // rect equal to the returned union.
  std::span<const UpscaleDirtyRect> dirty_rects() const {
    return {dirty_rects_.data(), dirty_rect_count_};
  }
  // Number of 16x16 source tiles that triggered the last upscale (all active
  // tiles for a full refresh).
  int last_dirty_tile_count() const { return last_dirty_tile_count_; }

  void set_dirty_policy(UpscaleDirtyPolicy policy) { dirty_policy_ = policy; }
  UpscaleDirtyPolicy dirty_policy() const { return dirty_policy_; }

  // Routes row rendering through a shared pool (nullptr restores the private
  // workers, which are started lazily on first use). The pool must outlive
  // this upscaler or be detached first.
  void set_thread_pool(ThreadPool* pool);

  // Direct stateless entry point for synthetic unit/benchmark tests.
  void upscale_full_buffer(
      const std::uint8_t* src_indices,
      const std::array<std::uint32_t, 16>& palette_argb,
      const std::array<std::uint8_t, VesaPlanarVideo::kTileCount>* glyph_tiles);

 private:
  struct SrcSpan {
    int x0 = 0;
    int x1 = 0;
    int y0 = 0;
    int y1 = 0;
  };
  static constexpr std::size_t kMaxSpans =
      static_cast<std::size_t>(VesaPlanarVideo::kTilesY) *
      ((VesaPlanarVideo::kTilesX + 1) / 2);

  void start_workers();
  void shutdown_workers() noexcept;
  void rebuild_distance_lut(const std::array<std::uint32_t, 16>& pal);
  void dispatch_rows(int sy0, int sy1, FunctionRef<void(int, int)> row_fn);
  void render_span_rows(
      int sy0, int sy1, const std::uint8_t* src,
      const std::array<std::uint32_t, 16>& pal,
      const std::array<std::uint8_t, VesaPlanarVideo::kTileCount>* glyph_tiles);
  void set_single_dirty_rect(const UpscaleDirtyRect& rect);
  void render_rows(
      int sy0, int sy1, int sx0, int sx1, const std::uint8_t* src,
      const std::array<std::uint32_t, 16>& pal,
      const std::array<std::uint8_t, VesaPlanarVideo::kTileCount>* glyph_tiles);
  void render_rows_nearest(int sy0, int sy1, int sx0, int sx1,
                           const std::uint8_t* src,
                           const std::array<std::uint32_t, 16>& pal);
  void render_rows_sharp2x(int sy0, int sy1, int sx0, int sx1,
                           const std::uint8_t* src,
                           const std::array<std::uint32_t, 16>& pal);
  void render_rows_scale2x(
      int sy0, int sy1, int sx0, int sx1, const std::uint8_t* src,
      const std::array<std::uint32_t, 16>& pal,
      const std::array<std::uint8_t, VesaPlanarVideo::kTileCount>* glyph_tiles);
  void render_rows_xbr(
      int sy0, int sy1, int sx0, int sx1, const std::uint8_t* src,
      const std::array<std::uint32_t, 16>& pal,
      const std::array<std::uint8_t, VesaPlanarVideo::kTileCount>* glyph_tiles,
      bool is_4x);

  UpscaleMode mode_ = UpscaleMode::Sharp2x;
  bool mode_dirty_ = true;
  bool canvas_enabled_ = true;
  int src_w_ = VesaPlanarVideo::kWidth;
  int src_h_ = VesaPlanarVideo::kHeight;
  int out_w_ = VesaPlanarVideo::kWidth * 2;
  int out_h_ = VesaPlanarVideo::kHeight * 2;
  std::vector<std::uint32_t> canvas_;
  std::array<std::array<int, 16>, 16> dist_lut_{};

  UpscaleDirtyPolicy dirty_policy_ = UpscaleDirtyPolicy::BoundingBox;
  std::array<UpscaleDirtyRect, kMaxSpans> dirty_rects_{};
  std::size_t dirty_rect_count_ = 0;
  int last_dirty_tile_count_ = 0;
  // Source-space spans grouped by tile row: spans of tile row `ty` occupy
  // [span_row_begin_[ty], span_row_begin_[ty + 1]).
  std::array<SrcSpan, kMaxSpans> src_spans_{};
  std::array<int, VesaPlanarVideo::kTilesY + 1> span_row_begin_{};

  ThreadPool* pool_ = nullptr;

  // Private worker pool, used only when no shared pool is set (workers_ MUST
  // be declared last so mutex, condition variables, and job state outlive
  // worker threads during destruction).
  int num_threads_ = 4;
  std::mutex pool_mu_;
  std::condition_variable cv_start_;
  std::condition_variable cv_done_;
  bool stop_pool_ = false;
  std::uint64_t job_epoch_ = 0;
  int active_workers_ = 0;
  int job_sy0_ = 0;
  int job_sy1_ = 0;
  const FunctionRef<void(int, int)>* job_fn_ = nullptr;
  std::exception_ptr job_exception_;
  std::vector<std::thread> workers_;
};

}  // namespace san4
