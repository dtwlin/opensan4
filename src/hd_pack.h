#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <list>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "hd_gen/san4_codecs.h"
#include "upscaler.h"
#include "video.h"

namespace san4 {

class ThreadPool;

struct HdTexture {
  int base_w = 0;
  int base_h = 0;
  int hd_w = 0;
  int hd_h = 0;
  std::vector<std::uint32_t> argb;  // 0xAARRGGBB (straight alpha)
};

struct GrpClipRect {
  int x0 = 0;
  int y0 = 0;
  int x1 = VesaPlanarVideo::kWidth - 1;
  int y1 = VesaPlanarVideo::kHeight - 1;
};

// Binary index record for `mondata/mondata_index.bin` (8,192 portraits -> O(1) lookup).
struct MondataIndexEntry {
  std::uint64_t hash64 = 0;
  std::uint8_t tmpl = 0;
  std::uint8_t top = 0;
  std::uint8_t bot = 0;
  std::uint8_t eye = 0;
  std::uint8_t mouth = 0;
  std::uint8_t eye_y = 0;
  std::uint8_t mouth_y = 0;
  std::uint8_t reserved = 0;
};
static_assert(sizeof(MondataIndexEntry) == 16,
              "Unexpected MondataIndexEntry size");

// Binary index record for `sprites/buttons/button_index.bin` (1,407 buttons -> O(1) lookup).
struct ButtonIndexEntry {
  std::uint64_t hash64 = 0;
  std::uint8_t base_id = 0;
  std::uint8_t label_id = 0;
  std::uint8_t reserved[6]{};
};
static_assert(sizeof(ButtonIndexEntry) == 16,
              "Unexpected ButtonIndexEntry size");

class HdPackManager {
 public:
  static constexpr int kHdScale = 4;
  static constexpr int kCanvasW = VesaPlanarVideo::kWidth * kHdScale;   // 3200
  static constexpr int kCanvasH = VesaPlanarVideo::kHeight * kHdScale;  // 2400
  static constexpr int kMaxPngDim = 4096;
  static constexpr std::uintmax_t kMaxManifestBytes =
      32ull * 1024ull * 1024ull;
  static constexpr std::size_t kMaxManifestEntries = 262144u;
  static constexpr std::size_t kMaxSavedPatches = 32u;
  static constexpr std::size_t kMaxActiveDynamicCgs = 16u;
  static constexpr std::size_t kDefaultMaxCacheBytes =
      256ull * 1024ull * 1024ull;
  // Bound on cached grading LUTs (LRU), enforced after every composite.
  static constexpr std::size_t kMaxLutCacheEntries = 256u;

  struct SavedHdPatch {
    int w = 0;
    int h = 0;
    std::uint64_t vram_hash = 0;
    bool has_any_hd = false;
    // Union of the used-color masks (bit = VGA slot) of the captured regions.
    std::uint16_t used_mask = 0;
    std::vector<std::uint32_t> hd_pixels_4x;
    std::vector<std::uint16_t> ref_pal_ids;
    std::vector<std::uint8_t> bank_hints;
    std::vector<std::uint8_t> snap_slots;
    std::vector<std::uint8_t> active_mask;
  };

  HdPackManager();
  ~HdPackManager();

  HdPackManager(const HdPackManager&) = delete;
  HdPackManager& operator=(const HdPackManager&) = delete;

  void set_pack_dir(const std::filesystem::path& dir);
  const std::filesystem::path& pack_dir() const noexcept { return pack_dir_; }
  void set_dump_dir(const std::filesystem::path& dir);

  void set_max_cache_bytes(std::size_t max_bytes);
  std::size_t max_cache_bytes() const noexcept { return max_cache_bytes_; }

  // Splits compositing and grading-LUT builds across `pool` (nullptr, the
  // default, runs them on the caller). Output is identical either way. The
  // pool must outlive this manager or be detached first.
  void set_thread_pool(ThreadPool* pool) noexcept { pool_ = pool; }

  // Re-upscales dynamic-palette CGs (`bank_hint == 3`) as background jobs on
  // `pool` instead of synchronously during frame preparation. A pending CG
  // keeps its current pixels until its result is applied by a later frame.
  // At most one job per CG is in flight: palette changes meanwhile only
  // update the CG's target, and when the job's result is drained a job for
  // the latest target is enqueued if it differs and is not cached. A job
  // that fails (e.g. `std::bad_alloc`) is drained as a failure and retried
  // by a later frame. nullptr (the default, used by tests and headless runs)
  // restores synchronous behavior. Jobs share no state with this manager
  // beyond a reference-counted result queue, so in-flight jobs may outlive it.
  void set_async_pool(ThreadPool* pool) noexcept { async_pool_ = pool; }

  // Dynamic-CG jobs enqueued whose results have not been drained yet (at
  // most one per CG region).
  std::size_t async_jobs_in_flight() const noexcept {
    return async_inflight_.size();
  }
  // Finished dynamic-CG results waiting for the next composite.
  std::size_t async_results_ready() const noexcept;
  // True when the next composite has HD-only changes to report (finished
  // async results or a `clear_overlays` of visible HD pixels) even if the
  // base frame did not change.
  bool has_pending_hd_changes() const noexcept {
    return clear_pending_ || async_results_ready() > 0u;
  }
  std::size_t lut_cache_size() const noexcept { return lut_cache_.size(); }

  // Hands every 16x16 logical tile whose HD overlay changed since the last
  // call to `video` for re-upscaling. HD changes can land in a later frame
  // than the VRAM writes that dirtied their tiles (deferred GRP600C hooks,
  // occlusion sync, async CG results), and the compositor blends in place
  // over freshly upscaled pixels only, so hosts call this (or prepare_frame(),
  // which ends with it) before every upscale pass.
  void flush_dirty_tiles(VesaPlanarVideo& video) const;

  // Per-frame HD state update before the upscale pass: consumes VRAM
  // occlusions, applies finished async dynamic-CG results, re-upscales (or
  // enqueues) drifted dynamic CGs, then flush_dirty_tiles(). Every HD
  // repaint therefore lands on tiles the same frame re-upscales, so the
  // following composite blends it over fresh base pixels exactly once.
  // composite_overlays() skips these steps after a prepare_frame(); called
  // without one, it runs them itself (repaints then reach the upscaler only
  // in the next frame).
  void prepare_frame(VesaPlanarVideo& video) const;

  bool dumping_enabled() const noexcept { return dumping_enabled_; }
  void set_dumping_enabled(bool enabled);
  bool toggle_dumping(const std::filesystem::path& default_dir);

  // Indexes `hd_manifest.tsv`, binary indices, and loose `img_<w>x<h>_<hash>.png`
  // files lazily without decoding PNG payloads upfront (< 30 ms).
  std::size_t reload_pack();

  std::size_t loaded_texture_count() const noexcept { return indexed_count_; }
  std::size_t active_overlay_count() const noexcept {
    return active_region_count_;
  }
  std::size_t cache_resident_bytes() const noexcept { return resident_bytes_; }
  std::uint64_t cache_hits() const noexcept { return cache_hits_; }
  std::uint64_t cache_misses() const noexcept { return cache_misses_; }
  std::uint64_t cache_evictions() const noexcept { return cache_evictions_; }
  double last_reload_ms() const noexcept { return last_reload_ms_; }

  // Consumes pending `VesaPlanarVideo::hd_occlude_bits()` from raw/primitive
  // VRAM writes (`AH=0x0A..0x0E`, XOR cursors, direct `vram_write8`) so dialog
  // borders and solid fills cleanly occlude underlying HD canvas pixels.
  void sync_vram_occlusions(const VesaPlanarVideo& video) const;

  // Canonical GRP600C AH=0x0F (`BlitPlanar`) handler with full source-plane,
  // ROP (`0` opaque / `4` color-0 transparent), clipping, MONDATA, Button,
  // SavedHdPatch, and dynamic 16x16 cell compositing support.
  void on_planar_blit_source(int dst_x, int dst_y, int w, int h,
                             int active_planes, std::uint8_t p3_fill_mode,
                             std::uint8_t p3_fill_byte,
                             std::uint16_t planar_rop,
                             std::uint8_t plane_disable, GrpClipRect clip,
                             std::span<const std::uint8_t> src_planar,
                             const VesaPlanarVideo& video);

  // Post-blit VRAM handler (used by synthetic tests and unclipped opaque blits).
  void on_planar_blit_complete(int dst_x, int dst_y, int w, int h,
                               const VesaPlanarVideo& video);

  // GRP600C AH=0x19 (`BlitNpk`) handler for NPK016 full-screen & event CGs.
  void on_npk_blit_complete(int dst_x, int dst_y, int w, int h,
                            GrpClipRect clip,
                            std::span<const std::uint8_t> npk_bitstream,
                            const VesaPlanarVideo& video);

  // GRP600C AH=0x13 (`BlitMono`) crisp 4x high-DPI glyph, Gaiji & UI wallpaper
  // rasterizer onto the HD canvas.
  void on_mono_blit_1bit(int dst_x, int dst_y, int w, int h,
                         std::uint16_t mono_rop, std::uint8_t fg_slot,
                         std::uint8_t bg_slot, std::uint8_t plane_disable,
                         GrpClipRect clip,
                         std::span<const std::uint8_t> src_1bit,
                         const VesaPlanarVideo& video);

  // GRP600C AH=0x10 (`ReadPlanar`, VRAM -> Guest RAM) background snapshot save.
  void on_vram_read_rect(int src_x, int src_y, int w, int h, GrpClipRect clip,
                         const VesaPlanarVideo& video);

  // GRP600C AH=0x18 (`SwapPlanarRect`, VRAM <-> Guest RAM) pre-swap capture &
  // post-swap restore/save.
  SavedHdPatch capture_patch_snapshot(int x, int y, int w, int h,
                                      GrpClipRect clip,
                                      const VesaPlanarVideo& video) const;
  void on_vram_swap_complete(int dst_x, int dst_y, int w, int h,
                             GrpClipRect clip, SavedHdPatch&& pre_swap_patch,
                             const VesaPlanarVideo& video);

  // GRP600C AH=0x17 (`CopyRect`) overlap-safe HD canvas & metadata translation.
  void on_vram_copy_rect(int src_x, int src_y, int w, int h, int dst_x,
                         int dst_y);

  // Clears the entire HD canvas (called on GRP600C AH=0x00/0x01/0x04).
  void clear_overlays();

  // Composites the persistent 4x HD canvas onto `canvas` (`out_w x out_h`,
  // scale factor `scale` in {1, 2, 4}; other scales composite nothing) with
  // dynamic seasonal/battle palette adaptation and smooth 24-bit cross-fade
  // grading, inside `inout_dirty_rect`. HD-only changes (dynamic-CG
  // re-upscales, `clear_overlays`) are composited too and unioned into
  // `inout_dirty_rect`.
  void composite_overlays(std::vector<std::uint32_t>& canvas, int out_w,
                          int out_h, int scale, const VesaPlanarVideo& video,
                          UpscaleDirtyRect& inout_dirty_rect) const;

  // Span-aware variant: composites only inside `dirty_rects` (which may
  // overlap; every covered output pixel is blended exactly once) plus the
  // HD-only changes. On return `out_rects` holds disjoint rects covering every
  // output pixel that changed, and the return value is their bounding box.
  UpscaleDirtyRect composite_overlays(
      std::vector<std::uint32_t>& canvas, int out_w, int out_h, int scale,
      const VesaPlanarVideo& video,
      std::span<const UpscaleDirtyRect> dirty_rects,
      std::vector<UpscaleDirtyRect>& out_rects) const;

  // Helpers exposed for unit testing and sprite PNG I/O.
  static bool parse_sprite_filename(std::string_view filename, int& out_w,
                                    int& out_h, std::uint64_t& out_hash);
  static std::uint64_t hash_sprite_4bit(int w, int h,
                                        const std::uint8_t* indices_4bit);
  static bool save_argb_png(const std::filesystem::path& path, int w, int h,
                            const std::uint32_t* argb);
  static bool load_argb_png(const std::filesystem::path& path, int& out_w,
                            int& out_h, std::vector<std::uint32_t>& out_argb);
  static bool sanitize_pack_relpath(std::string_view rel_sv,
                                    std::filesystem::path& out_rel);
  static bool verify_safe_pack_file(const std::filesystem::path& base_dir,
                                    const std::filesystem::path& rel_path,
                                    std::filesystem::path& out_full,
                                    std::uintmax_t* out_size = nullptr);

 private:
  static constexpr std::uint16_t kDirectUngradedPalId = 0xFFFFu;

  struct ManifestEntry {
    int base_w = 0;
    int base_h = 0;
    std::uint16_t ref_pal_id = kDirectUngradedPalId;
    std::uint8_t bank_hint = 0;
    bool is_loose_override = false;
    std::string rel_path;
  };

  struct CacheEntry {
    HdTexture tex;
    std::uint16_t ref_pal_id = kDirectUngradedPalId;
    std::uint8_t bank_hint = 0;
    std::size_t byte_size = 0;
    std::list<std::uint64_t>::iterator lru_it;
  };

  struct UnpackedTileDictEntry {
    std::uint64_t hd_hash = 0;
    std::uint16_t ref_pal_id = kDirectUngradedPalId;
    std::uint8_t archive_group = 0;
    bool is_overlay = false;
    int non_zero_count = 0;
    std::array<std::uint8_t, 256> slots{};
    std::array<std::uint8_t, 256> mask{};
  };

  struct PaletteGradingLut {
    std::uint32_t palette_gen = 0;
    bool is_fading = false;
    bool is_identity = true;
    // 17 * 17 * 17 = 4913 RGB entries packed as 0x00RRGGBB
    std::array<std::uint32_t, 17 * 17 * 17> grid{};
  };

  struct ActiveDynamicCg {
    std::uint32_t region_id = 0;
    int dst_x = 0;
    int dst_y = 0;
    int w = 0;
    int h = 0;
    GrpClipRect clip{};
    std::uint64_t base_hash = 0;
    std::uint16_t cur_pal_id = kDirectUngradedPalId;
    // Cache key and palette of the most recently requested re-upscale
    // (target_key 0 = none).
    std::uint64_t target_key = 0;
    std::array<std::uint32_t, 16> target_pal{};
    std::vector<std::uint8_t> vga_slots;
  };

  struct LutCacheEntry {
    PaletteGradingLut lut;
    std::list<std::uint64_t>::iterator lru_it;
  };

  // Per-frame compositor state of one band of output rows. Phase 1 maps each
  // HD pixel of the band to a 1-based index into `keys` (0 = not drawn);
  // `luts[i]` is the resolved LUT for `keys[i]` (nullptr = no grading).
  struct CompositeBand {
    std::vector<std::uint64_t> keys;
    std::vector<const PaletteGradingLut*> luts;
  };

  struct AsyncCgQueue;

  // 4x CG re-upscaler run by dynamic-CG refreshes (tests substitute it).
  using CgUpscaleFn = void (*)(const std::uint8_t* vga_slots, int w, int h,
                               const std::array<std::uint32_t, 16>& palette,
                               std::vector<std::uint32_t>& out_argb);
  static void upscale_cg_4x(const std::uint8_t* vga_slots, int w, int h,
                            const std::array<std::uint32_t, 16>& palette,
                            std::vector<std::uint32_t>& out_argb);

  std::uint16_t intern_palette(const std::array<std::uint32_t, 16>& pal) const;
  std::uint32_t alloc_region_id() const;
  void clear_logical_pixel(std::size_t lidx) const;
  void clear_canvas_clipped_rect(int x0, int y0, int x1_excl, int y1_excl) const;
  // Records logical rect [x0, x1) x [y0, y1) (clamped) in hd_dirty_tiles_.
  void mark_hd_tiles_dirty(int x0, int y0, int x1, int y1) const;

  struct ResolvedTexture {
    HdTexture owned_tex{};
    const HdTexture* tex = nullptr;
    std::uint16_t ref_pal_id = kDirectUngradedPalId;
    std::uint8_t bank_hint = 0;
  };

  bool resolve_or_load_texture(std::uint64_t hash, ResolvedTexture& out);
  void publish_or_hold_texture(std::uint64_t key, HdTexture&& tex,
                               std::uint16_t ref_pal_id,
                               std::uint8_t bank_hint,
                               ResolvedTexture& out) const;
  void evict_lru_to_fit(std::size_t limit_bytes) const;
  void evict_lru_if_needed() const;

  bool try_composite_mondata(std::uint64_t hash, ResolvedTexture& out);
  bool try_composite_button(std::uint64_t hash, ResolvedTexture& out);
  // `hash` must be `hash_sprite_4bit(16, 16, target_slots_256)`; it is the
  // cache key and the `tile_dict_index_` probe.
  bool try_composite_cell_16x16(std::uint64_t hash,
                                const std::uint8_t* target_slots_256,
                                ResolvedTexture& out);

  // Paints `tex` as a new region whose used-color mask is `used_mask`
  // (bit = VGA slot of a color the asset is drawn with).
  std::uint32_t paint_hd_texture_clipped(int dst_x, int dst_y, int w, int h,
                                         GrpClipRect clip, const HdTexture& tex,
                                         std::uint16_t ref_pal_id,
                                         std::uint8_t bank_hint,
                                         std::uint16_t used_mask,
                                         bool opaque_replace,
                                         const VesaPlanarVideo& video);
  void register_dynamic_cg(std::uint32_t region_id, int dst_x, int dst_y, int w,
                           int h, GrpClipRect clip, std::uint64_t base_hash,
                           std::uint16_t cur_pal_id,
                           std::span<const std::uint8_t> vga_slots);
  // Re-upscales visible dynamic CGs whose palette drifted (synchronously, or
  // by enqueueing async jobs) and records repainted logical rects in
  // `hd_dirty_rects_`.
  void refresh_dynamic_cgs(const VesaPlanarVideo& video) const;
  // Applies finished async CG results: each successful one is cached, and
  // repainted only if its region is still alive and still targets that
  // result's key; a CG whose target moved on gets a job for its new target.
  void drain_async_results() const;
  // Enqueues a re-upscale of `cg` to its target (`target_key`, `target_pal`).
  void enqueue_dynamic_cg_job(const ActiveDynamicCg& cg) const;
  // sync_vram_occlusions + drain_async_results + refresh_dynamic_cgs.
  void update_hd_state(const VesaPlanarVideo& video) const;
  void paint_dynamic_cg(ActiveDynamicCg& cg, const HdTexture& tex,
                        std::uint16_t new_pal_id) const;
  bool try_restore_saved_patch(int dst_x, int dst_y, int w, int h,
                               GrpClipRect clip, std::uint64_t vram_hash,
                               const VesaPlanarVideo& video);
  void push_saved_patch(SavedHdPatch&& patch);
  void maybe_dump_sprite(int w, int h, std::uint64_t hash,
                         const std::uint8_t* slots,
                         const VesaPlanarVideo& video);

  // Grading-LUT colors considered for a region: its used colors restricted to
  // game colors 8..15 for `bank_hint == 1`; falls back to the whole bank when
  // that leaves nothing.
  static std::uint16_t effective_grading_mask(std::uint8_t bank_hint,
                                              std::uint16_t used_mask);
  static std::uint64_t grading_lut_key(std::uint16_t ref_pal_id,
                                       std::uint8_t bank_hint,
                                       std::uint16_t effective_mask);
  // Builds the LUT grading `ref_pal_id` towards the latched palette using the
  // slots in `effective_mask`. Pure: safe to call concurrently.
  void build_grading_lut(std::uint16_t ref_pal_id,
                         std::uint16_t effective_mask,
                         const VesaPlanarVideo& video,
                         PaletteGradingLut& lut) const;
  void resolve_band_luts(int band_begin, int band_end,
                         const VesaPlanarVideo& video) const;
  void trim_lut_cache() const;
  // fn(chunk_begin, chunk_end) over [begin, end) on pool_ (grain 1), or
  // inline without a pool.
  void run_parallel(int begin, int end, FunctionRef<void(int, int)> fn) const;
  void composite_rects(std::vector<std::uint32_t>& canvas, int out_w,
                       int out_h, int scale, const VesaPlanarVideo& video,
                       const UpscaleDirtyRect& bounds,
                       std::span<const UpscaleDirtyRect> rects) const;

  friend struct HdPackManagerTestPeer;

  std::filesystem::path pack_dir_;
  std::filesystem::path dump_dir_;
  bool dumping_enabled_ = false;
  std::size_t max_cache_bytes_ = kDefaultMaxCacheBytes;
  std::size_t indexed_count_ = 0;
  double last_reload_ms_ = 0.0;

  std::unordered_map<std::uint64_t, ManifestEntry> manifest_map_;
  std::vector<MondataIndexEntry> mondata_index_;
  std::uint16_t mondata_ref_pal_id_ = kDirectUngradedPalId;
  std::vector<ButtonIndexEntry> button_index_;
  std::uint16_t button_ref_pal_id_ = kDirectUngradedPalId;
  std::vector<UnpackedTileDictEntry> tile_dict_;
  // Opaque (`!is_overlay`) tile_dict_ entries keyed by the hash_sprite_4bit
  // of their 256 slots, for O(1) exact-match lookups.
  std::unordered_multimap<std::uint64_t, std::uint32_t> tile_dict_index_;
  std::unordered_set<std::uint64_t> dumped_hashes_;

  mutable std::vector<std::array<std::uint32_t, 16>> ref_palettes_;
  mutable std::unordered_map<std::uint64_t, std::uint16_t> ref_palette_hash_;
  // Grading LUTs keyed by grading_lut_key(), most recently used first.
  mutable std::unordered_map<std::uint64_t, LutCacheEntry> lut_cache_;
  mutable std::list<std::uint64_t> lut_lru_;

  // Bounded 256 MB LRU decoded/composited texture cache.
  mutable std::list<std::uint64_t> lru_order_;
  mutable std::unordered_map<std::uint64_t, CacheEntry> cache_;
  mutable std::size_t resident_bytes_ = 0;
  mutable std::uint64_t cache_hits_ = 0;
  mutable std::uint64_t cache_misses_ = 0;
  mutable std::uint64_t cache_evictions_ = 0;

  // Bounded 32-entry ring buffer for AH=0x10 / AH=0x18 saved HD backgrounds.
  std::deque<SavedHdPatch> saved_patches_;

  // Bounded active dynamic-palette NPK CGs (`bank_hint == 3`) currently visible.
  mutable std::deque<ActiveDynamicCg> active_dynamic_cgs_;

  ThreadPool* pool_ = nullptr;
  ThreadPool* async_pool_ = nullptr;
  std::shared_ptr<AsyncCgQueue> async_queue_;
  // Region ids of dynamic CGs with an enqueued job not yet drained.
  mutable std::unordered_set<std::uint32_t> async_inflight_;
  CgUpscaleFn cg_upscale_fn_ = &HdPackManager::upscale_cg_4x;
  // Set by prepare_frame(), consumed by the next composite.
  mutable bool frame_prepared_ = false;

  // HD-only changes since the last composite, in logical coordinates.
  mutable std::vector<UpscaleDirtyRect> hd_dirty_rects_;
  // Set by clear_overlays() when it removed HD pixels.
  mutable bool clear_pending_ = false;
  // Logical tiles whose HD overlay changed since flush_dirty_tiles().
  mutable std::array<std::uint8_t, VesaPlanarVideo::kTileCount>
      hd_dirty_tiles_{};
  mutable bool hd_dirty_any_ = false;

  // Compositor scratch, reused across frames.
  mutable std::vector<CompositeBand> bands_;
  mutable std::vector<std::uint16_t> band_lut_index_;  // 800x600
  mutable std::vector<UpscaleDirtyRect> rect_scratch_;
  mutable std::vector<UpscaleDirtyRect> legacy_rects_;
  mutable std::unordered_map<std::uint64_t, const PaletteGradingLut*>
      frame_luts_;
  mutable std::vector<std::uint64_t> lut_build_keys_;
  mutable std::vector<PaletteGradingLut> lut_build_out_;

  // Persistent 4x screen-space HD canvas (3200x2400) + logical metadata (800x600).
  mutable std::vector<std::uint32_t> hd_canvas_argb_;
  mutable std::vector<std::uint16_t> hd_ref_pal_id_;
  mutable std::vector<std::uint8_t> hd_bank_hint_;
  mutable std::vector<std::uint8_t> hd_snap_slot_;
  mutable std::vector<std::uint32_t> hd_region_id_;
  mutable std::vector<std::uint32_t> region_refcounts_;
  // Used-color mask (bit = VGA slot) per region id, parallel to
  // region_refcounts_.
  mutable std::vector<std::uint16_t> region_used_mask_;
  mutable std::vector<std::uint32_t> free_region_ids_;
  mutable std::size_t active_region_count_ = 0;
};

}  // namespace san4
