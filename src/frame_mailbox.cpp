#include "frame_mailbox.h"

#include <algorithm>
#include <cstring>

#include "util/thread_pool.h"

namespace san4 {

FrameMailbox::FrameMailbox() = default;

UpscaleDirtyRect FrameMailbox::tile_output_rect(int tile, int scale, int w,
                                                int h) {
  UpscaleDirtyRect r{};
  if (tile < 0 || tile >= kTileCount || scale <= 0) {
    return r;
  }
  const int block = kTileSize * scale;
  const int x0 = (tile % kTilesX) * block;
  const int y0 = (tile / kTilesX) * block;
  const int x1 = std::min(w, x0 + block);
  const int y1 = std::min(h, y0 + block);
  if (x0 >= x1 || y0 >= y1) {
    return r;
  }
  r.has_changes = true;
  r.x = x0;
  r.y = y0;
  r.w = x1 - x0;
  r.h = y1 - y0;
  return r;
}

std::uint64_t FrameMailbox::changed_rects(const Slot& slot,
                                          std::uint64_t since_gen,
                                          std::vector<UpscaleDirtyRect>& out) {
  out.clear();
  if (slot.w <= 0 || slot.h <= 0 || slot.scale <= 0) {
    return 0;
  }
  const int block = kTileSize * slot.scale;
  const int tiles_x = std::min(kTilesX, (slot.w + block - 1) / block);
  const int tiles_y = std::min(kTilesY, (slot.h + block - 1) / block);
  // Indices into `out` of the rects ending at the previous / current tile
  // row, in increasing x. A run of the current row with the same x-span as
  // one ending right above extends it downwards instead of starting a rect.
  std::array<std::size_t, kTilesX> above{};
  std::array<std::size_t, kTilesX> current{};
  std::size_t n_above = 0;
  std::uint64_t covered = 0;
  for (int ty = 0; ty < tiles_y; ++ty) {
    const std::uint64_t* row = slot.tile_gen.data() + ty * kTilesX;
    const int y0 = ty * block;
    const int y1 = std::min(slot.h, y0 + block);
    std::size_t n_current = 0;
    std::size_t ai = 0;
    int tx = 0;
    while (tx < tiles_x) {
      if (row[tx] <= since_gen) {
        ++tx;
        continue;
      }
      const int tx0 = tx;
      while (tx < tiles_x && row[tx] > since_gen) {
        ++tx;
      }
      const int x0 = tx0 * block;
      const int x1 = std::min(slot.w, tx * block);
      while (ai < n_above && out[above[ai]].x < x0) {
        ++ai;
      }
      if (ai < n_above && out[above[ai]].x == x0 &&
          out[above[ai]].w == x1 - x0) {
        out[above[ai]].h += y1 - y0;
        current[n_current++] = above[ai++];
      } else {
        current[n_current++] = out.size();
        out.push_back(UpscaleDirtyRect{.has_changes = true,
                                       .resized = false,
                                       .x = x0,
                                       .y = y0,
                                       .w = x1 - x0,
                                       .h = y1 - y0});
      }
      covered += static_cast<std::uint64_t>(x1 - x0) *
                 static_cast<std::uint64_t>(y1 - y0);
    }
    above = current;
    n_above = n_current;
  }
  return covered;
}

void FrameMailbox::mark_dirty_tiles(std::span<const UpscaleDirtyRect> dirty,
                                    int scale, int w, int h) {
  const int block = kTileSize * scale;
  for (const UpscaleDirtyRect& r : dirty) {
    if (!r.has_changes) {
      continue;
    }
    const int x0 = std::max(0, r.x);
    const int y0 = std::max(0, r.y);
    const int x1 = std::min(w, r.x + r.w);
    const int y1 = std::min(h, r.y + r.h);
    if (x0 >= x1 || y0 >= y1) {
      continue;
    }
    const int tx0 = x0 / block;
    const int ty0 = y0 / block;
    const int tx1 = std::min(kTilesX - 1, (x1 - 1) / block);
    const int ty1 = std::min(kTilesY - 1, (y1 - 1) / block);
    for (int ty = ty0; ty <= ty1; ++ty) {
      std::fill(master_tile_gen_.begin() + ty * kTilesX + tx0,
                master_tile_gen_.begin() + ty * kTilesX + tx1 + 1, gen_);
    }
  }
}

void FrameMailbox::copy_tiles(Slot& slot, const std::uint32_t* src) {
  const int block = kTileSize * slot.scale;
  const int tiles_x = std::min(kTilesX, (slot.w + block - 1) / block);
  const int tiles_y = std::min(kTilesY, (slot.h + block - 1) / block);
  const auto stride = static_cast<std::size_t>(slot.w);
  std::uint32_t* const dst = slot.argb.data();
  auto copy_rows = [&](int ty_begin, int ty_end) {
    for (int ty = ty_begin; ty < ty_end; ++ty) {
      const std::uint64_t* master = master_tile_gen_.data() + ty * kTilesX;
      const std::uint64_t* have = slot.tile_gen.data() + ty * kTilesX;
      const int y0 = ty * block;
      const int y1 = std::min(slot.h, y0 + block);
      int tx = 0;
      while (tx < tiles_x) {
        if (master[tx] <= have[tx]) {
          ++tx;
          continue;
        }
        const int tx0 = tx;
        while (tx < tiles_x && master[tx] > have[tx]) {
          ++tx;
        }
        const auto x0 = static_cast<std::size_t>(tx0 * block);
        const auto n = static_cast<std::size_t>(std::min(slot.w, tx * block)) - x0;
        for (int y = y0; y < y1; ++y) {
          const std::size_t off = static_cast<std::size_t>(y) * stride + x0;
          std::memcpy(dst + off, src + off, n * sizeof(std::uint32_t));
        }
      }
    }
  };
  if (pool_ != nullptr) {
    pool_->parallel_for(0, tiles_y, 1, copy_rows);
  } else {
    copy_rows(0, tiles_y);
  }
}

void FrameMailbox::publish(std::span<const std::uint32_t> argb, int w, int h,
                           UpscaleMode mode,
                           std::span<const UpscaleDirtyRect> dirty,
                           const HostStatus& status,
                           const FrameProfileRow& profile) {
  if (w <= 0 || h <= 0 ||
      argb.size() < static_cast<std::size_t>(w) * static_cast<std::size_t>(h)) {
    return;
  }
  const int scale = PixelUpscaler::scale_factor_for(mode);
  ++gen_;
  if (w != cur_w_ || h != cur_h_ || mode != cur_mode_) {
    cur_w_ = w;
    cur_h_ = h;
    cur_mode_ = mode;
    master_tile_gen_.fill(gen_);
  } else {
    mark_dirty_tiles(dirty, scale, w, h);
  }

  // Slots are resized lazily when they come back to the producer: the other
  // two may be in the consumer's hands.
  Slot& slot = slots_[write_];
  if (slot.w != w || slot.h != h || slot.mode != mode) {
    slot.w = w;
    slot.h = h;
    slot.mode = mode;
    slot.scale = scale;
    slot.argb.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h),
                     0u);
    slot.tile_gen.fill(0u);
  }
  copy_tiles(slot, argb.data());
  slot.tile_gen = master_tile_gen_;
  slot.gen = gen_;
  slot.status = status;
  slot.profile = profile;

  write_ = state_.exchange(write_ | kFresh, std::memory_order_acq_rel) &
           kIndexMask;
  {
    // Pairs with the predicate check in wait_for_fresh_until so a consumer
    // that just found nothing cannot miss this wakeup.
    std::lock_guard<std::mutex> lock(wait_mu_);
  }
  wait_cv_.notify_one();
}

const FrameMailbox::Slot* FrameMailbox::acquire() {
  if ((state_.load(std::memory_order_acquire) & kFresh) == 0u) {
    return nullptr;
  }
  read_ = state_.exchange(read_, std::memory_order_acq_rel) & kIndexMask;
  return &slots_[read_];
}

const FrameMailbox::Slot* FrameMailbox::current_acquired() const noexcept {
  return (slots_[read_].gen > 0u) ? &slots_[read_] : nullptr;
}

bool FrameMailbox::wait_for_fresh_until(
    std::chrono::steady_clock::time_point deadline) {
  std::unique_lock<std::mutex> lock(wait_mu_);
  wait_cv_.wait_until(lock, deadline, [this] {
    return wake_ || (state_.load(std::memory_order_acquire) & kFresh) != 0u;
  });
  wake_ = false;
  return (state_.load(std::memory_order_acquire) & kFresh) != 0u;
}

void FrameMailbox::notify_waiters() {
  {
    std::lock_guard<std::mutex> lock(wait_mu_);
    wake_ = true;
  }
  wait_cv_.notify_one();
}

}  // namespace san4
