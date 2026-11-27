#include "gpu/gpu_frame_mailbox.h"

#include <algorithm>
#include <cstring>

#include "util/thread_pool.h"

namespace san4 {

GpuFrameMailbox::GpuFrameMailbox() {
  for (Slot& s : slots_) {
    s.lut_slot_map.fill(-1);
  }
}

std::uint64_t GpuFrameMailbox::changed_tile_rects(
    const std::array<std::uint64_t, kTileCount>& tile_gen,
    std::uint64_t since_gen, int tiles_x, int tiles_y, int tile_px, int w,
    int h, int halo_px, std::vector<UpscaleDirtyRect>& out) {
  out.clear();
  if (w <= 0 || h <= 0 || tile_px <= 0 || tiles_x <= 0 || tiles_y <= 0) {
    return 0;
  }
  const int tx_lim = std::min(kTilesX, tiles_x);
  const int ty_lim = std::min(kTilesY, tiles_y);

  if (halo_px <= 0) {
    std::array<std::size_t, kTilesX> above{};
    std::array<std::size_t, kTilesX> current{};
    std::size_t n_above = 0;
    std::uint64_t covered = 0;
    for (int ty = 0; ty < ty_lim; ++ty) {
      const std::uint64_t* row = tile_gen.data() + ty * kTilesX;
      const int y0 = ty * tile_px;
      const int y1 = std::min(h, y0 + tile_px);
      if (y0 >= y1) {
        break;
      }
      std::size_t n_current = 0;
      std::size_t ai = 0;
      int tx = 0;
      while (tx < tx_lim) {
        if (row[tx] <= since_gen) {
          ++tx;
          continue;
        }
        const int tx0 = tx;
        while (tx < tx_lim && row[tx] > since_gen) {
          ++tx;
        }
        const int x0 = tx0 * tile_px;
        const int x1 = std::min(w, tx * tile_px);
        if (x0 >= x1) {
          continue;
        }
        while (ai < n_above && out[above[ai]].x < x0) {
          ++ai;
        }
        if (ai < n_above && out[above[ai]].x == x0 &&
            out[above[ai]].w == x1 - x0 &&
            out[above[ai]].y + out[above[ai]].h == y0) {
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

  thread_local std::vector<UpscaleDirtyRect> raw;
  raw.clear();
  for (int ty = 0; ty < ty_lim; ++ty) {
    const std::uint64_t* row = tile_gen.data() + ty * kTilesX;
    int tx = 0;
    while (tx < tx_lim) {
      if (row[tx] <= since_gen) {
        ++tx;
        continue;
      }
      const int tx0 = tx;
      while (tx < tx_lim && row[tx] > since_gen) {
        ++tx;
      }
      const int x0 = std::max(0, tx0 * tile_px - halo_px);
      const int y0 = std::max(0, ty * tile_px - halo_px);
      const int x1 = std::min(w, tx * tile_px + halo_px);
      const int y1 = std::min(h, (ty + 1) * tile_px + halo_px);
      if (x0 < x1 && y0 < y1) {
        raw.push_back(UpscaleDirtyRect{.has_changes = true,
                                       .resized = false,
                                       .x = x0,
                                       .y = y0,
                                       .w = x1 - x0,
                                       .h = y1 - y0});
      }
    }
  }
  if (raw.empty()) {
    return 0;
  }
  normalize_dirty_rects(raw, w, h, out);
  std::uint64_t covered = 0;
  for (const UpscaleDirtyRect& r : out) {
    covered +=
        static_cast<std::uint64_t>(r.w) * static_cast<std::uint64_t>(r.h);
  }
  return covered;
}

void GpuFrameMailbox::copy_vram_tiles(Slot& slot, const std::uint8_t* src_vram) {
  if (src_vram == nullptr) {
    return;
  }
  constexpr int kStride = VesaPlanarVideo::kWidth;
  const int tiles_x = std::min(kTilesX, (slot.src_w + kTileSize - 1) / kTileSize);
  const int tiles_y = std::min(kTilesY, (slot.src_h + kTileSize - 1) / kTileSize);
  std::uint8_t* const dst = slot.vram.data();

  for (int ty = 0; ty < tiles_y; ++ty) {
    const std::uint64_t* master = master_vram_tile_gen_.data() + ty * kTilesX;
    const std::uint64_t* have = slot.vram_tile_gen.data() + ty * kTilesX;
    const int y0 = ty * kTileSize;
    const int y1 = std::min(slot.src_h, y0 + kTileSize);
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
      const int x0 = tx0 * kTileSize;
      const int x1 = std::min(slot.src_w, tx * kTileSize);
      const auto n = static_cast<std::size_t>(x1 - x0);
      for (int y = y0; y < y1; ++y) {
        const std::size_t off =
            static_cast<std::size_t>(y) * kStride + static_cast<std::size_t>(x0);
        std::memcpy(dst + off, src_vram + off, n);
      }
    }
  }
}

void GpuFrameMailbox::copy_hd_tiles(Slot& slot, const std::uint32_t* src_hd,
                                    const std::uint16_t* src_lut_idx) {
  constexpr int kW = VesaPlanarVideo::kWidth;
  constexpr int kH = VesaPlanarVideo::kHeight;
  constexpr int kHdBlock = kTileSize * kHdScale;  // 64

  if (src_lut_idx != nullptr) {
    std::uint16_t* const dst_idx = slot.hd_lut_idx.data();
    for (int ty = 0; ty < kTilesY; ++ty) {
      const std::uint64_t* master =
          master_lut_idx_tile_gen_.data() + ty * kTilesX;
      const std::uint64_t* have = slot.lut_idx_tile_gen.data() + ty * kTilesX;
      const int y0 = ty * kTileSize;
      const int y1 = std::min(kH, y0 + kTileSize);
      int tx = 0;
      while (tx < kTilesX) {
        if (master[tx] <= have[tx]) {
          ++tx;
          continue;
        }
        const int tx0 = tx;
        while (tx < kTilesX && master[tx] > have[tx]) {
          ++tx;
        }
        const int x0 = tx0 * kTileSize;
        const int x1 = std::min(kW, tx * kTileSize);
        const auto n = static_cast<std::size_t>(x1 - x0);
        for (int y = y0; y < y1; ++y) {
          const std::size_t off =
              static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x0);
          std::memcpy(dst_idx + off, src_lut_idx + off,
                      n * sizeof(std::uint16_t));
        }
      }
    }
  }

  if (src_hd == nullptr) {
    return;
  }
  std::uint32_t* const dst_hd = slot.hd_canvas_argb.data();
  auto copy_rows = [&](int ty_begin, int ty_end) {
    for (int ty = ty_begin; ty < ty_end; ++ty) {
      const std::uint64_t* master = master_hd_tile_gen_.data() + ty * kTilesX;
      const std::uint64_t* have = slot.hd_tile_gen.data() + ty * kTilesX;
      const int hy0 = ty * kHdBlock;
      const int hy1 = std::min(kHdCanvasH, hy0 + kHdBlock);
      int tx = 0;
      while (tx < kTilesX) {
        if (master[tx] <= have[tx]) {
          ++tx;
          continue;
        }
        const int tx0 = tx;
        while (tx < kTilesX && master[tx] > have[tx]) {
          ++tx;
        }
        const auto hx0 = static_cast<std::size_t>(tx0 * kHdBlock);
        const auto hn =
            static_cast<std::size_t>(std::min(kHdCanvasW, tx * kHdBlock)) - hx0;
        for (int hy = hy0; hy < hy1; ++hy) {
          const std::size_t off =
              static_cast<std::size_t>(hy) * kHdCanvasW + hx0;
          std::memcpy(dst_hd + off, src_hd + off, hn * sizeof(std::uint32_t));
        }
      }
    }
  };
  if (pool_ != nullptr) {
    pool_->parallel_for(0, kTilesY, 1, copy_rows);
  } else {
    copy_rows(0, kTilesY);
  }
}

void GpuFrameMailbox::publish(const GpuVideoInput& video, const GpuHdInput& hd,
                              bool force_full, const HostStatus& status,
                              const FrameProfileRow& profile) {
  if (video.src_w <= 0 || video.src_h <= 0 || video.vram == nullptr ||
      video.palette_argb == nullptr) {
    return;
  }

  ++gen_;
  const int scale = PixelUpscaler::scale_factor_for(video.mode);
  const int out_w = video.src_w * scale;
  const int out_h = video.src_h * scale;

  const bool geom_changed =
      (video.src_w != cur_src_w_ || video.src_h != cur_src_h_);
  const bool mode_changed = (video.mode != cur_mode_);
  const bool hd_toggled = (hd.active != cur_hd_active_);

  cur_src_w_ = video.src_w;
  cur_src_h_ = video.src_h;
  cur_mode_ = video.mode;
  if (cur_hd_active_ && !hd.active) {
    ++hd_epoch_;
  }
  cur_hd_active_ = hd.active;

  if (force_full || geom_changed) {
    master_vram_tile_gen_.fill(gen_);
    if (video.vram_tile_gen != nullptr) {
      std::copy_n(video.vram_tile_gen, kTileCount, seen_src_vram_gen_.begin());
    }
  } else if (video.vram_tile_gen != nullptr) {
    for (int t = 0; t < kTileCount; ++t) {
      if (video.vram_tile_gen[t] != seen_src_vram_gen_[t]) {
        seen_src_vram_gen_[t] = video.vram_tile_gen[t];
        master_vram_tile_gen_[t] = gen_;
        master_compose_tile_gen_[t] = gen_;
      }
    }
  }

  if (force_full || master_palette_gen_ == 0u ||
      video.palette_gen != seen_src_palette_gen_) {
    seen_src_palette_gen_ = video.palette_gen;
    master_palette_gen_ = gen_;
    master_full_recompose_gen_ = gen_;
  }

  if (force_full || master_glyph_gen_ == 0u ||
      video.glyph_gen != seen_src_glyph_gen_) {
    seen_src_glyph_gen_ = video.glyph_gen;
    master_glyph_gen_ = gen_;
    if (video.glyph_tile_gen != nullptr && !force_full) {
      for (int t = 0; t < kTileCount; ++t) {
        if (video.glyph_tile_gen[t] != seen_src_glyph_tile_gen_[t]) {
          seen_src_glyph_tile_gen_[t] = video.glyph_tile_gen[t];
          master_compose_tile_gen_[t] = gen_;
        }
      }
    } else {
      if (video.glyph_tile_gen != nullptr) {
        std::copy_n(video.glyph_tile_gen, kTileCount,
                    seen_src_glyph_tile_gen_.begin());
      }
      master_full_recompose_gen_ = gen_;
    }
  }

  if (force_full || geom_changed || mode_changed || hd_toggled) {
    master_full_recompose_gen_ = gen_;
  }

  if (hd.active) {
    const bool full_hd = force_full || hd_toggled;
    if (full_hd) {
      master_hd_tile_gen_.fill(gen_);
      master_lut_idx_tile_gen_.fill(gen_);
      if (hd.canvas_tile_gen != nullptr) {
        std::copy_n(hd.canvas_tile_gen, kTileCount, seen_src_hd_gen_.begin());
      }
      if (hd.lut_idx_tile_gen != nullptr) {
        std::copy_n(hd.lut_idx_tile_gen, kTileCount,
                    seen_src_lut_idx_gen_.begin());
      }
    } else {
      if (hd.canvas_tile_gen != nullptr) {
        for (int t = 0; t < kTileCount; ++t) {
          if (hd.canvas_tile_gen[t] != seen_src_hd_gen_[t]) {
            seen_src_hd_gen_[t] = hd.canvas_tile_gen[t];
            master_hd_tile_gen_[t] = gen_;
            master_compose_tile_gen_[t] = gen_;
          }
        }
      }
      if (hd.lut_idx_tile_gen != nullptr) {
        for (int t = 0; t < kTileCount; ++t) {
          if (hd.lut_idx_tile_gen[t] != seen_src_lut_idx_gen_[t]) {
            seen_src_lut_idx_gen_[t] = hd.lut_idx_tile_gen[t];
            master_lut_idx_tile_gen_[t] = gen_;
            master_compose_tile_gen_[t] = gen_;
          }
        }
      }
    }

    if (full_hd || master_luts_gen_ == 0u || hd.luts_gen != seen_src_luts_gen_) {
      seen_src_luts_gen_ = hd.luts_gen;
      master_luts_gen_ = gen_;
      master_full_recompose_gen_ = gen_;
    }
  }

  Slot& slot = slots_[write_];
  if (slot.src_w != video.src_w || slot.src_h != video.src_h) {
    slot.src_w = video.src_w;
    slot.src_h = video.src_h;
    slot.vram_tile_gen.fill(0u);
  }
  slot.out_w = out_w;
  slot.out_h = out_h;
  slot.mode = video.mode;
  slot.scale = scale;

  copy_vram_tiles(slot, video.vram);
  slot.vram_tile_gen = master_vram_tile_gen_;

  slot.palette_argb = *video.palette_argb;
  slot.palette_gen = master_palette_gen_;

  if (video.glyph_tiles != nullptr && slot.glyph_gen < master_glyph_gen_) {
    std::copy_n(video.glyph_tiles, kTileCount, slot.glyph_tiles.begin());
  }
  slot.glyph_gen = master_glyph_gen_;

  slot.hd_active = hd.active;
  if (hd.active) {
    const std::size_t hd_px =
        static_cast<std::size_t>(kHdCanvasW) * kHdCanvasH;
    const std::size_t logical_px =
        static_cast<std::size_t>(VesaPlanarVideo::kWidth) *
        VesaPlanarVideo::kHeight;
    if (slot.hd_canvas_argb.size() != hd_px || slot.hd_epoch != hd_epoch_) {
      slot.hd_canvas_argb.assign(hd_px, 0u);
      slot.hd_tile_gen.fill(0u);
      slot.hd_lut_idx.assign(logical_px, 0u);
      slot.lut_idx_tile_gen.fill(0u);
      slot.luts_gen = 0u;
      slot.hd_epoch = hd_epoch_;
    }
    copy_hd_tiles(slot, hd.canvas_argb, hd.lut_idx_map);
    slot.hd_tile_gen = master_hd_tile_gen_;
    slot.lut_idx_tile_gen = master_lut_idx_tile_gen_;

    if (slot.luts_gen < master_luts_gen_) {
      slot.luts.assign(hd.luts.begin(), hd.luts.end());
      slot.lut_slot_map.fill(-1);
      if (hd.lut_slot_map != nullptr) {
        std::copy_n(hd.lut_slot_map, kMaxKeySlots, slot.lut_slot_map.begin());
      }
      slot.luts_gen = master_luts_gen_;
    }
  }

  slot.compose_tile_gen = master_compose_tile_gen_;
  slot.full_recompose_gen = master_full_recompose_gen_;
  slot.gen = gen_;
  slot.status = status;
  slot.profile = profile;

  write_ = state_.exchange(write_ | kFresh, std::memory_order_acq_rel) &
           kIndexMask;
  {
    std::lock_guard<std::mutex> lock(wait_mu_);
  }
  wait_cv_.notify_one();
}

const GpuFrameMailbox::Slot* GpuFrameMailbox::acquire() {
  if ((state_.load(std::memory_order_acquire) & kFresh) == 0u) {
    return nullptr;
  }
  read_ = state_.exchange(read_, std::memory_order_acq_rel) & kIndexMask;
  return &slots_[read_];
}

const GpuFrameMailbox::Slot* GpuFrameMailbox::current_acquired() const noexcept {
  return (slots_[read_].gen > 0u) ? &slots_[read_] : nullptr;
}

bool GpuFrameMailbox::wait_for_fresh_until(
    std::chrono::steady_clock::time_point deadline) {
  std::unique_lock<std::mutex> lock(wait_mu_);
  wait_cv_.wait_until(lock, deadline, [this] {
    return wake_ || (state_.load(std::memory_order_acquire) & kFresh) != 0u;
  });
  wake_ = false;
  return (state_.load(std::memory_order_acquire) & kFresh) != 0u;
}

void GpuFrameMailbox::notify_waiters() {
  {
    std::lock_guard<std::mutex> lock(wait_mu_);
    wake_ = true;
  }
  wait_cv_.notify_one();
}

}  // namespace san4
