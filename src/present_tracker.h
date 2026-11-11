#pragma once

#include <cstdint>

#include "upscaler.h"

namespace san4 {

// SDL-free bookkeeping of the host's frame texture: which published frame
// generation it holds, whether the window still needs a present without new
// pixels, and the one-time scale fallback when a texture cannot be created.
//
// Protocol per frame: needs_new_texture() decides whether the texture must
// be (re)created before uploading; texture_replaced() records a successful
// (re)creation or a lost texture (SDL render-target / device reset), which
// invalidates every earlier upload; finish_upload() records the uploaded
// generation and decides whether to present; presented() consumes a pending
// redraw.
class PresentTracker {
 public:
  // True when uploading a `w x h` `mode` frame needs a new texture, given
  // whether one exists and its geometry. A failed creation leaves no
  // texture, so the next frame retries.
  static bool needs_new_texture(bool has_texture, int tex_w, int tex_h,
                                UpscaleMode tex_mode, int w, int h,
                                UpscaleMode mode) {
    return !has_texture || w != tex_w || h != tex_h || mode != tex_mode;
  }

  // The texture's contents are undefined: nothing is uploaded and the window
  // must be redrawn.
  void texture_replaced() {
    uploaded_gen_ = 0;
    needs_redraw_ = true;
  }

  // Generation of the newest frame fully uploaded (0 = none; upload all).
  std::uint64_t uploaded_gen() const { return uploaded_gen_; }

  // Everything of generation `gen` is now uploaded, `uploaded_px` pixels of
  // it by this call. Returns whether to present: pixels changed, or a
  // redraw is pending.
  bool finish_upload(std::uint64_t gen, std::uint64_t uploaded_px) {
    uploaded_gen_ = gen;
    return uploaded_px > 0u || needs_redraw_;
  }

  void request_redraw() { needs_redraw_ = true; }
  bool needs_redraw() const { return needs_redraw_; }
  void presented() { needs_redraw_ = false; }

  // A texture for `failed_mode` could not be created. Returns true exactly
  // once for a mode other than Nearest: the caller then asks the emulation
  // side to pin Nearest (which it must not re-request every frame while the
  // command is in flight).
  bool take_scale_fallback(UpscaleMode failed_mode) {
    if (failed_mode == UpscaleMode::Nearest || fallback_requested_) {
      return false;
    }
    fallback_requested_ = true;
    return true;
  }

 private:
  std::uint64_t uploaded_gen_ = 0;
  bool needs_redraw_ = true;
  bool fallback_requested_ = false;
};

}  // namespace san4
