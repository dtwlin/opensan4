#include "upscaler.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "util/string_util.h"
#include "util/thread_pool.h"

namespace san4 {
namespace {

// Dirty regions narrower than this many source rows render on the caller.
constexpr int kMinParallelRows = 32;
// Smallest row chunk handed to a shared-pool worker.
constexpr int kMinPoolRowGrain = 8;
// Source-pixel halo around dirty tiles for 5x5 xBR stencil continuity.
constexpr int kHaloPx = 2;

constexpr std::uint32_t blend50(std::uint32_t a, std::uint32_t b) {
  return 0xFF000000u |
         ((((a ^ b) & 0x00FEFEFEu) >> 1) + (a & b & 0x00FFFFFFu));
}

constexpr std::uint32_t blend75(std::uint32_t primary,
                                std::uint32_t secondary) {
  return blend50(primary, blend50(primary, secondary));
}

constexpr bool is_checkerboard_dither(std::uint8_t a, std::uint8_t b,
                                      std::uint8_t c, std::uint8_t d,
                                      std::uint8_t e, std::uint8_t f,
                                      std::uint8_t g, std::uint8_t h,
                                      std::uint8_t i) {
  return (e == a && b == d && e != b) || (e == c && b == f && e != b) ||
         (e == g && d == h && e != d) || (e == i && f == h && e != f);
}

inline std::uint8_t sample_clamped(const std::uint8_t* src, int x, int y,
                                   int max_w, int max_h) {
  const int cx = std::clamp(x, 0, max_w - 1);
  const int cy = std::clamp(y, 0, max_h - 1);
  return src[cy * VesaPlanarVideo::kWidth + cx] & 0x0Fu;
}

}  // namespace

UpscaleDirtyRect normalize_dirty_rects(std::span<const UpscaleDirtyRect> rects,
                                       int w, int h,
                                       std::vector<UpscaleDirtyRect>& out) {
  struct Box {
    int x0, y0, x1, y1;
  };
  struct Span {
    int x0, x1;
  };
  // Called once or twice per frame; per-thread scratch keeps it free of
  // steady-state allocations.
  struct Scratch {
    std::vector<Box> boxes;
    std::vector<int> ys;
    std::vector<Box> active;
    std::vector<Span> spans;
    std::vector<std::size_t> open;
    std::vector<std::size_t> next_open;
  };
  thread_local Scratch scratch;
  std::vector<Box>& boxes = scratch.boxes;
  std::vector<int>& ys = scratch.ys;
  std::vector<Box>& active = scratch.active;
  std::vector<Span>& spans = scratch.spans;
  std::vector<std::size_t>& open = scratch.open;
  std::vector<std::size_t>& next_open = scratch.next_open;
  boxes.clear();
  ys.clear();
  active.clear();
  open.clear();
  out.clear();
  for (const UpscaleDirtyRect& r : rects) {
    if (!r.has_changes) {
      continue;
    }
    const int x0 = std::clamp(r.x, 0, std::max(w, 0));
    const int y0 = std::clamp(r.y, 0, std::max(h, 0));
    const int x1 = static_cast<int>(std::clamp<std::int64_t>(
        static_cast<std::int64_t>(r.x) + r.w, 0, std::max(w, 0)));
    const int y1 = static_cast<int>(std::clamp<std::int64_t>(
        static_cast<std::int64_t>(r.y) + r.h, 0, std::max(h, 0)));
    if (x0 >= x1 || y0 >= y1) {
      continue;
    }
    boxes.push_back(Box{x0, y0, x1, y1});
    ys.push_back(y0);
    ys.push_back(y1);
  }
  if (boxes.empty()) {
    return UpscaleDirtyRect{};
  }
  std::sort(boxes.begin(), boxes.end(),
            [](const Box& a, const Box& b) { return a.y0 < b.y0; });
  std::sort(ys.begin(), ys.end());
  ys.erase(std::unique(ys.begin(), ys.end()), ys.end());

  // Sweep the horizontal bands between consecutive box edges. In each band
  // the covering boxes' x-intervals are merged; a merged span identical to
  // one emitted for the band directly above extends that rect downwards.
  std::size_t next_box = 0;
  for (std::size_t yi = 0; yi + 1u < ys.size(); ++yi) {
    const int ya = ys[yi];
    const int yb = ys[yi + 1u];
    std::erase_if(active, [ya](const Box& b) { return b.y1 <= ya; });
    while (next_box < boxes.size() && boxes[next_box].y0 <= ya) {
      active.push_back(boxes[next_box++]);
    }
    spans.clear();
    for (const Box& b : active) {
      spans.push_back(Span{b.x0, b.x1});
    }
    std::sort(spans.begin(), spans.end(),
              [](const Span& a, const Span& b) { return a.x0 < b.x0; });
    next_open.clear();
    std::size_t si = 0;
    while (si < spans.size()) {
      Span m = spans[si++];
      while (si < spans.size() && spans[si].x0 <= m.x1) {
        m.x1 = std::max(m.x1, spans[si++].x1);
      }
      bool extended = false;
      for (std::size_t oi : open) {
        UpscaleDirtyRect& o = out[oi];
        if (o.x == m.x0 && o.x + o.w == m.x1 && o.y + o.h == ya) {
          o.h += yb - ya;
          next_open.push_back(oi);
          extended = true;
          break;
        }
      }
      if (!extended) {
        next_open.push_back(out.size());
        out.push_back(UpscaleDirtyRect{.has_changes = true,
                                       .x = m.x0,
                                       .y = ya,
                                       .w = m.x1 - m.x0,
                                       .h = yb - ya});
      }
    }
    open.swap(next_open);
  }

  UpscaleDirtyRect u = out.front();
  int ux1 = u.x + u.w;
  int uy1 = u.y + u.h;
  for (const UpscaleDirtyRect& r : out) {
    u.x = std::min(u.x, r.x);
    u.y = std::min(u.y, r.y);
    ux1 = std::max(ux1, r.x + r.w);
    uy1 = std::max(uy1, r.y + r.h);
  }
  u.w = ux1 - u.x;
  u.h = uy1 - u.y;
  return u;
}

PixelUpscaler::PixelUpscaler(int num_threads)
    : num_threads_(std::clamp(num_threads, 1, 16)) {
  canvas_.assign(
      static_cast<std::size_t>(out_w_) * static_cast<std::size_t>(out_h_),
      0xFF000000u);
}

PixelUpscaler::~PixelUpscaler() { shutdown_workers(); }

void PixelUpscaler::start_workers() {
  stop_pool_ = false;
  // Workers spawned after earlier jobs must not mistake the current epoch
  // for a new job.
  const std::uint64_t start_epoch = job_epoch_;
  workers_.reserve(static_cast<std::size_t>(num_threads_));
  try {
    for (int tid = 0; tid < num_threads_; ++tid) {
      workers_.emplace_back([this, tid, start_epoch]() {
        std::uint64_t seen_epoch = start_epoch;
        while (true) {
          const FunctionRef<void(int, int)>* fn = nullptr;
          int r0 = 0;
          int r1 = 0;
          {
            std::unique_lock<std::mutex> lk(pool_mu_);
            cv_start_.wait(
                lk, [&]() { return stop_pool_ || job_epoch_ != seen_epoch; });
            if (stop_pool_) {
              return;
            }
            seen_epoch = job_epoch_;
            const int total = job_sy1_ - job_sy0_;
            r0 = job_sy0_ + (total * tid) / num_threads_;
            r1 = job_sy0_ + (total * (tid + 1)) / num_threads_;
            fn = job_fn_;
          }
          std::exception_ptr worker_ex;
          if (r0 < r1 && fn != nullptr) {
            try {
              (*fn)(r0, r1);
            } catch (...) {
              worker_ex = std::current_exception();
            }
          }
          {
            std::lock_guard<std::mutex> lk(pool_mu_);
            if (worker_ex && !job_exception_) {
              job_exception_ = worker_ex;
            }
            if (--active_workers_ == 0) {
              cv_done_.notify_one();
            }
          }
        }
      });
    }
  } catch (...) {
    shutdown_workers();
    throw;
  }
}

void PixelUpscaler::shutdown_workers() noexcept {
  {
    std::lock_guard<std::mutex> lk(pool_mu_);
    stop_pool_ = true;
  }
  cv_start_.notify_all();
  for (auto& th : workers_) {
    if (th.joinable()) {
      th.join();
    }
  }
  workers_.clear();
}

void PixelUpscaler::set_thread_pool(ThreadPool* pool) {
  pool_ = pool;
  if (pool_ != nullptr) {
    shutdown_workers();
  }
}

int PixelUpscaler::scale_factor_for(UpscaleMode mode) {
  switch (mode) {
    case UpscaleMode::Nearest:
      return 1;
    case UpscaleMode::Sharp2x:
    case UpscaleMode::Scale2x:
    case UpscaleMode::Xbr2x:
      return 2;
    case UpscaleMode::Xbr4x:
      return 4;
  }
  return 2;
}

const char* PixelUpscaler::mode_name(UpscaleMode mode) {
  switch (mode) {
    case UpscaleMode::Nearest:
      return "nearest";
    case UpscaleMode::Sharp2x:
      return "sharp2x";
    case UpscaleMode::Scale2x:
      return "scale2x";
    case UpscaleMode::Xbr2x:
      return "xbr2x";
    case UpscaleMode::Xbr4x:
      return "xbr4x";
  }
  return "sharp2x";
}

bool PixelUpscaler::parse_mode(std::string_view text, UpscaleMode& out_mode) {
  const std::string lower = util::ascii_lower(text);
  if (lower == "nearest" || lower == "1x") {
    out_mode = UpscaleMode::Nearest;
    return true;
  }
  if (lower == "sharp2x" || lower == "sharp" || lower == "sharp-bilinear") {
    out_mode = UpscaleMode::Sharp2x;
    return true;
  }
  if (lower == "scale2x" || lower == "epx") {
    out_mode = UpscaleMode::Scale2x;
    return true;
  }
  if (lower == "xbr2x") {
    out_mode = UpscaleMode::Xbr2x;
    return true;
  }
  if (lower == "xbr4x" || lower == "xbr") {
    out_mode = UpscaleMode::Xbr4x;
    return true;
  }
  return false;
}

void PixelUpscaler::set_mode(UpscaleMode mode) {
  if (mode_ == mode && !mode_dirty_) {
    return;
  }
  mode_ = mode;
  const int s = scale_factor_for(mode_);
  out_w_ = src_w_ * s;
  out_h_ = src_h_ * s;
  canvas_.assign(static_cast<std::size_t>(out_w_ * out_h_), 0xFF000000u);
  mode_dirty_ = true;
}

UpscaleMode PixelUpscaler::cycle_mode() {
  const int next = (static_cast<int>(mode_) + 1) % 5;
  set_mode(static_cast<UpscaleMode>(next));
  return mode_;
}

void PixelUpscaler::rebuild_distance_lut(
    const std::array<std::uint32_t, 16>& pal) {
  for (int i = 0; i < 16; ++i) {
    const int r0 = static_cast<int>((pal[i] >> 16) & 0xFFu);
    const int g0 = static_cast<int>((pal[i] >> 8) & 0xFFu);
    const int b0 = static_cast<int>(pal[i] & 0xFFu);
    for (int j = 0; j < 16; ++j) {
      const int dr = r0 - static_cast<int>((pal[j] >> 16) & 0xFFu);
      const int dg = g0 - static_cast<int>((pal[j] >> 8) & 0xFFu);
      const int db = b0 - static_cast<int>(pal[j] & 0xFFu);
      dist_lut_[i][j] = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
    }
  }
}

void PixelUpscaler::dispatch_rows(int sy0, int sy1,
                                  FunctionRef<void(int, int)> row_fn) {
  if (sy0 >= sy1) {
    return;
  }
  if ((sy1 - sy0) < kMinParallelRows) {
    row_fn(sy0, sy1);
    return;
  }
  if (pool_ != nullptr) {
    const int lanes = std::max(1, static_cast<int>(pool_->size()) * 4);
    const int grain =
        std::max(kMinPoolRowGrain, (sy1 - sy0 + lanes - 1) / lanes);
    pool_->parallel_for(sy0, sy1, grain, row_fn);
    return;
  }
  if (num_threads_ <= 1) {
    row_fn(sy0, sy1);
    return;
  }
  if (workers_.empty()) {
    start_workers();
  }
  std::exception_ptr ex;
  {
    std::unique_lock<std::mutex> lk(pool_mu_);
    job_sy0_ = sy0;
    job_sy1_ = sy1;
    job_fn_ = &row_fn;
    job_exception_ = nullptr;
    active_workers_ = static_cast<int>(workers_.size());
    ++job_epoch_;
    cv_start_.notify_all();
    cv_done_.wait(lk, [&]() { return active_workers_ == 0; });
    job_fn_ = nullptr;
    ex = job_exception_;
    job_exception_ = nullptr;
  }
  if (ex) {
    std::rethrow_exception(ex);
  }
}

void PixelUpscaler::set_single_dirty_rect(const UpscaleDirtyRect& rect) {
  dirty_rects_[0] = rect;
  dirty_rects_[0].resized = false;
  dirty_rect_count_ = 1;
}

UpscaleDirtyRect PixelUpscaler::upscale_from_video(VesaPlanarVideo& video,
                                                   bool force_full) {
  UpscaleDirtyRect rect{};
  dirty_rect_count_ = 0;
  last_dirty_tile_count_ = 0;
  std::array<std::uint8_t, VesaPlanarVideo::kTileCount> dirty{};
  bool palette_changed = false;
  const bool any_dirty = video.consume_dirty_tiles(dirty, palette_changed);

  const int vid_w = video.latched_width();
  const int vid_h = video.latched_height();
  if (vid_w != src_w_ || vid_h != src_h_) {
    src_w_ = vid_w;
    src_h_ = vid_h;
    const int s = scale_factor_for(mode_);
    out_w_ = src_w_ * s;
    out_h_ = src_h_ * s;
    canvas_.assign(static_cast<std::size_t>(out_w_ * out_h_), 0xFF000000u);
    mode_dirty_ = true;
  }

  const bool need_full = force_full || mode_dirty_ || palette_changed;
  if (!any_dirty && !need_full) {
    return rect;
  }

  const auto& pal = video.latched_palette_argb();
  if (palette_changed || mode_dirty_) {
    rebuild_distance_lut(pal);
  }

  rect.resized = mode_dirty_;
  mode_dirty_ = false;

  constexpr int kTile = VesaPlanarVideo::kTileSize;
  const int tiles_x = (src_w_ + kTile - 1) / kTile;
  const int tiles_y = (src_h_ + kTile - 1) / kTile;
  int sx0 = 0;
  int sy0 = 0;
  int sx1 = src_w_;
  int sy1 = src_h_;
  bool render_spans = false;

  if (need_full) {
    last_dirty_tile_count_ = tiles_x * tiles_y;
  } else {
    // Merge each tile row's dirty tiles into [tx0, tx1) runs, haloed and
    // clamped to the active source area.
    std::size_t n_spans = 0;
    int dirty_tiles = 0;
    for (int ty = 0; ty < tiles_y; ++ty) {
      span_row_begin_[static_cast<std::size_t>(ty)] = static_cast<int>(n_spans);
      const std::uint8_t* row = dirty.data() + ty * VesaPlanarVideo::kTilesX;
      int tx = 0;
      while (tx < tiles_x) {
        if (row[tx] == 0u) {
          ++tx;
          continue;
        }
        const int tx0 = tx;
        while (tx < tiles_x && row[tx] != 0u) {
          ++tx;
        }
        dirty_tiles += tx - tx0;
        src_spans_[n_spans++] = SrcSpan{
            .x0 = std::max(0, tx0 * kTile - kHaloPx),
            .x1 = std::min(src_w_, tx * kTile + kHaloPx),
            .y0 = std::max(0, ty * kTile - kHaloPx),
            .y1 = std::min(src_h_, (ty + 1) * kTile + kHaloPx),
        };
      }
    }
    span_row_begin_[static_cast<std::size_t>(tiles_y)] =
        static_cast<int>(n_spans);
    // Tile rows beyond the active area (640x480 modes) stay empty.
    for (int ty = tiles_y + 1; ty <= VesaPlanarVideo::kTilesY; ++ty) {
      span_row_begin_[static_cast<std::size_t>(ty)] = static_cast<int>(n_spans);
    }
    if (n_spans == 0u) {
      return rect;
    }
    last_dirty_tile_count_ = dirty_tiles;
    sx0 = src_w_;
    sy0 = src_h_;
    sx1 = 0;
    sy1 = 0;
    for (std::size_t i = 0; i < n_spans; ++i) {
      sx0 = std::min(sx0, src_spans_[i].x0);
      sy0 = std::min(sy0, src_spans_[i].y0);
      sx1 = std::max(sx1, src_spans_[i].x1);
      sy1 = std::max(sy1, src_spans_[i].y1);
    }
    render_spans = (dirty_policy_ == UpscaleDirtyPolicy::TileRowSpans);
  }

  const std::uint8_t* src = video.latched_vram().data();
  const auto* glyph_ptr = &video.latched_glyph_tiles();
  const int s = scale_factor_for(mode_);
  if (render_spans) {
    dispatch_rows(sy0, sy1, [&](int r0, int r1) {
      render_span_rows(r0, r1, src, pal, glyph_ptr);
    });
    const std::size_t n_spans = static_cast<std::size_t>(
        span_row_begin_[static_cast<std::size_t>(tiles_y)]);
    for (std::size_t i = 0; i < n_spans; ++i) {
      const SrcSpan& sp = src_spans_[i];
      dirty_rects_[i] = UpscaleDirtyRect{
          .has_changes = true,
          .resized = false,
          .x = sp.x0 * s,
          .y = sp.y0 * s,
          .w = (sp.x1 - sp.x0) * s,
          .h = (sp.y1 - sp.y0) * s,
      };
    }
    dirty_rect_count_ = n_spans;
  } else {
    dispatch_rows(sy0, sy1, [&](int r0, int r1) {
      render_rows(r0, r1, sx0, sx1, src, pal, glyph_ptr);
    });
  }

  rect.has_changes = true;
  rect.x = sx0 * s;
  rect.y = sy0 * s;
  rect.w = (sx1 - sx0) * s;
  rect.h = (sy1 - sy0) * s;
  if (!render_spans) {
    set_single_dirty_rect(rect);
  }
  return rect;
}

void PixelUpscaler::render_span_rows(
    int sy0, int sy1, const std::uint8_t* src,
    const std::array<std::uint32_t, 16>& pal,
    const std::array<std::uint8_t, VesaPlanarVideo::kTileCount>* glyph_tiles) {
  constexpr int kTile = VesaPlanarVideo::kTileSize;
  // A haloed span of tile row `ty` covers source rows
  // [ty * 16 - 2, ty * 16 + 18), so row `y` is touched only by tile rows
  // (y - 2) / 16 .. (y + 2) / 16, which is at most two of them.
  constexpr std::size_t kMaxRowIntervals =
      2u * ((VesaPlanarVideo::kTilesX + 1) / 2);
  constexpr int kLastTileRow = VesaPlanarVideo::kTilesY - 1;
  std::array<std::pair<int, int>, kMaxRowIntervals> iv{};
  for (int y = sy0; y < sy1; ++y) {
    std::size_t n = 0;
    const int ty0 = std::max(0, (y - kHaloPx) / kTile);
    const int ty1 = std::min(kLastTileRow, (y + kHaloPx) / kTile);
    for (int ty = ty0; ty <= ty1; ++ty) {
      const int b = span_row_begin_[static_cast<std::size_t>(ty)];
      const int e = span_row_begin_[static_cast<std::size_t>(ty) + 1u];
      for (int i = b; i < e; ++i) {
        const SrcSpan& sp = src_spans_[static_cast<std::size_t>(i)];
        if (y >= sp.y0 && y < sp.y1) {
          iv[n++] = {sp.x0, sp.x1};
        }
      }
    }
    if (n == 0u) {
      continue;
    }
    std::sort(iv.begin(), iv.begin() + static_cast<std::ptrdiff_t>(n));
    int cur0 = iv[0].first;
    int cur1 = iv[0].second;
    for (std::size_t i = 1; i < n; ++i) {
      if (iv[i].first <= cur1) {
        cur1 = std::max(cur1, iv[i].second);
      } else {
        render_rows(y, y + 1, cur0, cur1, src, pal, glyph_tiles);
        cur0 = iv[i].first;
        cur1 = iv[i].second;
      }
    }
    render_rows(y, y + 1, cur0, cur1, src, pal, glyph_tiles);
  }
}

void PixelUpscaler::upscale_full_buffer(
    const std::uint8_t* src_indices,
    const std::array<std::uint32_t, 16>& palette_argb,
    const std::array<std::uint8_t, VesaPlanarVideo::kTileCount>* glyph_tiles) {
  if (src_w_ != VesaPlanarVideo::kWidth || src_h_ != VesaPlanarVideo::kHeight) {
    src_w_ = VesaPlanarVideo::kWidth;
    src_h_ = VesaPlanarVideo::kHeight;
    const int s = scale_factor_for(mode_);
    out_w_ = src_w_ * s;
    out_h_ = src_h_ * s;
    canvas_.assign(static_cast<std::size_t>(out_w_ * out_h_), 0xFF000000u);
  }
  rebuild_distance_lut(palette_argb);
  mode_dirty_ = false;
  dispatch_rows(0, src_h_, [&](int r0, int r1) {
    render_rows(r0, r1, 0, src_w_, src_indices, palette_argb, glyph_tiles);
  });
  last_dirty_tile_count_ = VesaPlanarVideo::kTileCount;
  set_single_dirty_rect(UpscaleDirtyRect{
      .has_changes = true, .resized = false, .x = 0, .y = 0, .w = out_w_,
      .h = out_h_});
}

void PixelUpscaler::render_rows(
    int sy0, int sy1, int sx0, int sx1, const std::uint8_t* src,
    const std::array<std::uint32_t, 16>& pal,
    const std::array<std::uint8_t, VesaPlanarVideo::kTileCount>* glyph_tiles) {
  switch (mode_) {
    case UpscaleMode::Nearest:
      render_rows_nearest(sy0, sy1, sx0, sx1, src, pal);
      return;
    case UpscaleMode::Sharp2x:
      render_rows_sharp2x(sy0, sy1, sx0, sx1, src, pal);
      return;
    case UpscaleMode::Scale2x:
      render_rows_scale2x(sy0, sy1, sx0, sx1, src, pal, glyph_tiles);
      return;
    case UpscaleMode::Xbr2x:
      render_rows_xbr(sy0, sy1, sx0, sx1, src, pal, glyph_tiles, false);
      return;
    case UpscaleMode::Xbr4x:
      render_rows_xbr(sy0, sy1, sx0, sx1, src, pal, glyph_tiles, true);
      return;
  }
}

void PixelUpscaler::render_rows_nearest(
    int sy0, int sy1, int sx0, int sx1, const std::uint8_t* src,
    const std::array<std::uint32_t, 16>& pal) {
  constexpr int kStride = VesaPlanarVideo::kWidth;
  for (int y = sy0; y < sy1; ++y) {
    const std::uint8_t* srow = src + y * kStride;
    std::uint32_t* drow = canvas_.data() + y * out_w_;
    for (int x = sx0; x < sx1; ++x) {
      drow[x] = pal[srow[x] & 0x0Fu];
    }
  }
}

void PixelUpscaler::render_rows_sharp2x(
    int sy0, int sy1, int sx0, int sx1, const std::uint8_t* src,
    const std::array<std::uint32_t, 16>& pal) {
  constexpr int kStride = VesaPlanarVideo::kWidth;
  for (int y = sy0; y < sy1; ++y) {
    const std::uint8_t* srow = src + y * kStride;
    std::uint32_t* drow0 = canvas_.data() + (y * 2) * out_w_;
    std::uint32_t* drow1 = drow0 + out_w_;
    for (int x = sx0; x < sx1; ++x) {
      const std::uint32_t c = pal[srow[x] & 0x0Fu];
      drow0[x * 2 + 0] = c;
      drow0[x * 2 + 1] = c;
      drow1[x * 2 + 0] = c;
      drow1[x * 2 + 1] = c;
    }
  }
}

void PixelUpscaler::render_rows_scale2x(
    int sy0, int sy1, int sx0, int sx1, const std::uint8_t* src,
    const std::array<std::uint32_t, 16>& pal,
    const std::array<std::uint8_t, VesaPlanarVideo::kTileCount>* glyph_tiles) {
  constexpr int kStride = VesaPlanarVideo::kWidth;
  const int W = src_w_;
  const int H = src_h_;

  for (int y = sy0; y < sy1; ++y) {
    const std::uint8_t* row_m = src + std::max(0, y - 1) * kStride;
    const std::uint8_t* row_0 = src + y * kStride;
    const std::uint8_t* row_p = src + std::min(H - 1, y + 1) * kStride;
    std::uint32_t* drow0 = canvas_.data() + (y * 2) * out_w_;
    std::uint32_t* drow1 = drow0 + out_w_;

    for (int x = sx0; x < sx1; ++x) {
      const int xm = std::max(0, x - 1);
      const int xp = std::min(W - 1, x + 1);
      const std::uint8_t B = row_m[x] & 0x0Fu;
      const std::uint8_t D = row_0[xm] & 0x0Fu;
      const std::uint8_t E = row_0[x] & 0x0Fu;
      const std::uint8_t F = row_0[xp] & 0x0Fu;
      const std::uint8_t Hh = row_p[x] & 0x0Fu;

      std::uint8_t e0 = E, e1 = E, e2 = E, e3 = E;
      const bool is_glyph =
          glyph_tiles &&
          (*glyph_tiles)[(y >> 4) * VesaPlanarVideo::kTilesX + (x >> 4)] != 0;
      if (!is_glyph && B != Hh && D != F) {
        if (D == B) e0 = D;
        if (B == F) e1 = F;
        if (D == Hh) e2 = D;
        if (Hh == F) e3 = F;
      }
      drow0[x * 2 + 0] = pal[e0];
      drow0[x * 2 + 1] = pal[e1];
      drow1[x * 2 + 0] = pal[e2];
      drow1[x * 2 + 1] = pal[e3];
    }
  }
}

void PixelUpscaler::render_rows_xbr(
    int sy0, int sy1, int sx0, int sx1, const std::uint8_t* src,
    const std::array<std::uint32_t, 16>& pal,
    const std::array<std::uint8_t, VesaPlanarVideo::kTileCount>* glyph_tiles,
    bool is_4x) {
  // Shared 5x5 xBR kernel for Xbr2x and Xbr4x with fast interior path +
  // glyph-cell and 2x2 checkerboard-dither protection.
  constexpr int kStride = VesaPlanarVideo::kWidth;
  const int W = src_w_;
  const int H = src_h_;
  const auto d = [this](std::uint8_t p, std::uint8_t q) -> int {
    return dist_lut_[p][q];
  };

  for (int y = sy0; y < sy1; ++y) {
    const bool interior_y = (y >= 2 && y < H - 2);
    const std::uint8_t* rm2 = interior_y ? (src + (y - 2) * kStride) : nullptr;
    const std::uint8_t* rm1 = interior_y ? (src + (y - 1) * kStride) : nullptr;
    const std::uint8_t* r0 = src + y * kStride;
    const std::uint8_t* rp1 = interior_y ? (src + (y + 1) * kStride) : nullptr;
    const std::uint8_t* rp2 = interior_y ? (src + (y + 2) * kStride) : nullptr;

    for (int x = sx0; x < sx1; ++x) {
      const std::uint8_t e = r0[x] & 0x0Fu;
      const std::uint32_t pe = pal[e];
      const bool is_glyph =
          glyph_tiles &&
          (*glyph_tiles)[(y >> 4) * VesaPlanarVideo::kTilesX + (x >> 4)] != 0;

      // Fast solid emit helper
      auto emit_solid = [&]() {
        if (!is_4x) {
          std::uint32_t* d0 = canvas_.data() + (y * 2) * out_w_ + x * 2;
          std::uint32_t* d1 = d0 + out_w_;
          d0[0] = pe;
          d0[1] = pe;
          d1[0] = pe;
          d1[1] = pe;
        } else {
          std::uint32_t* base = canvas_.data() + (y * 4) * out_w_ + x * 4;
          for (int ry = 0; ry < 4; ++ry) {
            std::uint32_t* row = base + ry * out_w_;
            row[0] = pe;
            row[1] = pe;
            row[2] = pe;
            row[3] = pe;
          }
        }
      };

      if (is_glyph) {
        emit_solid();
        continue;
      }

      // 5x5 neighborhood:
      //      a1 b1 c1
      // a0   a  b  c   c4
      // d0   d  e  f   f4
      // g0   g  h  i   i4
      //      g5 h5 i5
      std::uint8_t a1, b1, c1, a0, a, b, c, c4, d0, d_px, f, f4, g0, g, h, i,
          i4, g5, h5, i5;
      if (interior_y && x >= 2 && x < W - 2) {
        a = rm1[x - 1] & 0x0Fu;
        b = rm1[x] & 0x0Fu;
        c = rm1[x + 1] & 0x0Fu;
        d_px = r0[x - 1] & 0x0Fu;
        f = r0[x + 1] & 0x0Fu;
        g = rp1[x - 1] & 0x0Fu;
        h = rp1[x] & 0x0Fu;
        i = rp1[x + 1] & 0x0Fu;
        if (a == e && b == e && c == e && d_px == e && f == e && g == e &&
            h == e && i == e) {
          emit_solid();
          continue;
        }
        if (is_checkerboard_dither(a, b, c, d_px, e, f, g, h, i)) {
          emit_solid();
          continue;
        }
        a1 = rm2[x - 1] & 0x0Fu;
        b1 = rm2[x] & 0x0Fu;
        c1 = rm2[x + 1] & 0x0Fu;
        a0 = rm1[x - 2] & 0x0Fu;
        c4 = rm1[x + 2] & 0x0Fu;
        d0 = r0[x - 2] & 0x0Fu;
        f4 = r0[x + 2] & 0x0Fu;
        g0 = rp1[x - 2] & 0x0Fu;
        i4 = rp1[x + 2] & 0x0Fu;
        g5 = rp2[x - 1] & 0x0Fu;
        h5 = rp2[x] & 0x0Fu;
        i5 = rp2[x + 1] & 0x0Fu;
      } else {
        a = sample_clamped(src, x - 1, y - 1, W, H);
        b = sample_clamped(src, x, y - 1, W, H);
        c = sample_clamped(src, x + 1, y - 1, W, H);
        d_px = sample_clamped(src, x - 1, y, W, H);
        f = sample_clamped(src, x + 1, y, W, H);
        g = sample_clamped(src, x - 1, y + 1, W, H);
        h = sample_clamped(src, x, y + 1, W, H);
        i = sample_clamped(src, x + 1, y + 1, W, H);
        if (is_checkerboard_dither(a, b, c, d_px, e, f, g, h, i)) {
          emit_solid();
          continue;
        }
        a1 = sample_clamped(src, x - 1, y - 2, W, H);
        b1 = sample_clamped(src, x, y - 2, W, H);
        c1 = sample_clamped(src, x + 1, y - 2, W, H);
        a0 = sample_clamped(src, x - 2, y - 1, W, H);
        c4 = sample_clamped(src, x + 2, y - 1, W, H);
        d0 = sample_clamped(src, x - 2, y, W, H);
        f4 = sample_clamped(src, x + 2, y, W, H);
        g0 = sample_clamped(src, x - 2, y + 1, W, H);
        i4 = sample_clamped(src, x + 2, y + 1, W, H);
        g5 = sample_clamped(src, x - 1, y + 2, W, H);
        h5 = sample_clamped(src, x, y + 2, W, H);
        i5 = sample_clamped(src, x + 1, y + 2, W, H);
      }

      // Hyllian xBR edge weights for the 4 corners (BR, BL, TR, TL)
      auto xbr_corner = [&](std::uint8_t p_diag, std::uint8_t p_h,
                            std::uint8_t p_v, std::uint8_t p_opp_v,
                            std::uint8_t p_opp_h, std::uint8_t p_far_h1,
                            std::uint8_t p_far_h2, std::uint8_t p_far_v1,
                            std::uint8_t p_far_v2, int& out_diff,
                            std::uint32_t& out_color) -> bool {
        if (e == p_h && e == p_v) {
          out_diff = 0;
          return false;
        }
        const int wd1 = d(e, p_diag) + d(e, p_opp_v) + d(e, p_opp_h) +
                        d(p_far_v2, p_h) + d(p_far_h2, p_v) +
                        4 * d(p_v, p_h);
        const int wd2 = d(p_v, p_far_v1) + d(p_v, p_opp_h) +
                        d(p_h, p_far_h1) + d(p_h, p_opp_v) +
                        4 * d(e, p_diag);
        if (wd1 < wd2) {
          const std::uint8_t cand = (d(e, p_h) <= d(e, p_v)) ? p_h : p_v;
          if (cand != e) {
            out_diff = wd2 - wd1;
            out_color = pal[cand];
            return true;
          }
        }
        out_diff = 0;
        return false;
      };

      int diff_br = 0, diff_bl = 0, diff_tr = 0, diff_tl = 0;
      std::uint32_t col_br = pe, col_bl = pe, col_tr = pe, col_tl = pe;

      const bool ed_br = xbr_corner(i, f, h, c, g, i4, f4, i5, h5, diff_br, col_br);
      const bool ed_bl = xbr_corner(g, d_px, h, a, i, g0, d0, g5, h5, diff_bl, col_bl);
      const bool ed_tr = xbr_corner(c, f, b, i, a, c4, f4, c1, b1, diff_tr, col_tr);
      const bool ed_tl = xbr_corner(a, d_px, b, g, c, a0, d0, a1, b1, diff_tl, col_tl);

      if (!is_4x) {
        std::uint32_t* d0 = canvas_.data() + (y * 2) * out_w_ + x * 2;
        std::uint32_t* d1 = d0 + out_w_;
        d0[0] = ed_tl ? blend50(pe, col_tl) : pe;
        d0[1] = ed_tr ? blend50(pe, col_tr) : pe;
        d1[0] = ed_bl ? blend50(pe, col_bl) : pe;
        d1[1] = ed_br ? blend50(pe, col_br) : pe;
      } else {
        std::uint32_t block[4][4] = {
            {pe, pe, pe, pe},
            {pe, pe, pe, pe},
            {pe, pe, pe, pe},
            {pe, pe, pe, pe},
        };

        auto fill_4x_corner = [&](bool active, std::uint32_t cc,
                                  std::uint8_t p_h, std::uint8_t p_v,
                                  std::uint8_t p_adj_h, std::uint8_t p_adj_v,
                                  int r_outer, int c_outer, int r_step,
                                  int c_step) {
          if (!active) {
            return;
          }
          const int fg = d(p_h, p_adj_v);
          const int hc = d(p_v, p_adj_h);
          const bool steep_v = (2 * fg <= hc) && (e != p_adj_v) && (d_px != p_adj_v || f != p_adj_v);
          const bool steep_h = (2 * hc <= fg) && (e != p_adj_h);

          block[r_outer][c_outer] = cc;
          if (steep_v && steep_h) {
            block[r_outer + r_step][c_outer] = blend75(cc, pe);
            block[r_outer][c_outer + c_step] = blend75(cc, pe);
            block[r_outer + r_step][c_outer + c_step] = blend50(cc, pe);
          } else if (steep_v) {
            block[r_outer + r_step][c_outer] = cc;
            block[r_outer][c_outer + c_step] = blend50(cc, pe);
            block[r_outer + r_step * 2][c_outer] = blend50(cc, pe);
          } else if (steep_h) {
            block[r_outer][c_outer + c_step] = cc;
            block[r_outer + r_step][c_outer] = blend50(cc, pe);
            block[r_outer][c_outer + c_step * 2] = blend50(cc, pe);
          } else {
            block[r_outer + r_step][c_outer] = blend50(cc, pe);
            block[r_outer][c_outer + c_step] = blend50(cc, pe);
          }
        };

        fill_4x_corner(ed_tl, col_tl, d_px, b, c, g, 0, 0, 1, 1);
        fill_4x_corner(ed_tr, col_tr, f, b, a, i, 0, 3, 1, -1);
        fill_4x_corner(ed_bl, col_bl, d_px, h, i, a, 3, 0, -1, 1);
        fill_4x_corner(ed_br, col_br, f, h, c, g, 3, 3, -1, -1);

        std::uint32_t* base = canvas_.data() + (y * 4) * out_w_ + x * 4;
        for (int ry = 0; ry < 4; ++ry) {
          std::memcpy(base + ry * out_w_, block[ry], 4 * sizeof(std::uint32_t));
        }
      }
    }
  }
}

}  // namespace san4
