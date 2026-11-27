#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gpu/gl_loader.h"
#include "gpu/gpu_frame_mailbox.h"

namespace san4::gpu {

// Two-pass OpenGL 3.3 Core GPU renderer:
//   Pass 1 (`prog_compose_`): Renders integer-exact Nearest/Sharp2x/Scale2x/
//     Xbr2x/Xbr4x base upscaling + optional 3200x2400 HD overlay compositing +
//     17x17x17 3D-LUT trilinear color grading into `fbo_composed_` (`tex_composed_`),
//     scissored strictly to dirty tile rects (+2 source-px xBR halo).
//   Pass 2 (`prog_present_`): Draws `tex_composed_` onto default framebuffer 0
//     inside the aspect-fitted 4:3 viewport with GL_NEAREST (1x) or GL_LINEAR
//     (2x/4x Sharp-Bilinear / supersampled).
class GlRenderer {
 public:
  struct UploadResult {
    bool ok = false;
    bool recomposed = false;
    std::uint64_t uploaded_texels = 0;
  };

  GlRenderer() = default;
  ~GlRenderer();

  GlRenderer(const GlRenderer&) = delete;
  GlRenderer& operator=(const GlRenderer&) = delete;

  bool init(const GlProcs& procs, std::string* out_error = nullptr);
  void shutdown() noexcept;
  bool initialized() const noexcept { return initialized_; }

  // Invalidates all GPU upload cursors (e.g. after context recreation).
  void reset_tracking() noexcept;

  int composed_width() const noexcept { return composed_w_; }
  int composed_height() const noexcept { return composed_h_; }
  UpscaleMode composed_mode() const noexcept { return composed_mode_; }

  // Uploads dirty textures/uniforms of `slot` and runs Pass 1 into
  // `fbo_composed_` when any pixel changed.
  UploadResult upload_and_compose(const GpuFrameMailbox::Slot& slot);

  // Runs Pass 2 onto default framebuffer 0 (`vp_x, vp_y` in top-left window
  // drawable coordinates; converted internally to OpenGL bottom-left viewport).
  void present(int drawable_w, int drawable_h, int vp_x, int vp_y, int vp_w,
               int vp_h);

  // Reads `tex_composed_` (`composed_w_ x composed_h_`) back as top-down
  // `0xFFRRGGBB` ARGB8888 pixels for automated GPU-vs-CPU parity verification.
  bool readback_composed_argb(std::vector<std::uint32_t>& out) const;

 private:
  static constexpr int kLutCols = 32;
  static constexpr int kLutTileW = 17;
  static constexpr int kLutTileH = 17 * 17;  // 289
  static constexpr int kLutAtlasW = kLutCols * kLutTileW;  // 544

  bool compile_program(const char* vert_src, const char* frag_src,
                       GLuint& out_prog, std::string* out_error);
  bool ensure_composed_target(int w, int h, UpscaleMode mode);
  bool ensure_hd_textures(std::size_t num_luts);
  void update_composed_filter(UpscaleMode mode);

  GlProcs gl_{};
  bool initialized_ = false;
  GLint max_tex_size_ = 4096;

  GLuint vao_ = 0;
  GLuint prog_compose_ = 0;
  GLuint prog_present_ = 0;

  // Uniform locations for prog_compose_
  GLint loc_comp_flip_y_ = -1;
  GLint loc_comp_vram_ = -1;
  GLint loc_comp_glyph_ = -1;
  GLint loc_comp_hd_canvas_ = -1;
  GLint loc_comp_hd_lut_idx_ = -1;
  GLint loc_comp_luts_ = -1;
  GLint loc_comp_palette_ = -1;
  GLint loc_comp_lut_slot_map_ = -1;
  GLint loc_comp_src_size_ = -1;
  GLint loc_comp_mode_ = -1;
  GLint loc_comp_scale_ = -1;
  GLint loc_comp_hd_active_ = -1;

  // Uniform locations for prog_present_
  GLint loc_pres_flip_y_ = -1;
  GLint loc_pres_frame_ = -1;

  GLuint tex_vram_ = 0;
  GLuint tex_glyph_ = 0;
  GLuint tex_hd_canvas_ = 0;
  GLuint tex_hd_lut_idx_ = 0;
  GLuint tex_luts_ = 0;
  int lut_atlas_rows_ = 0;

  GLuint tex_composed_ = 0;
  GLuint fbo_composed_ = 0;
  int composed_w_ = 0;
  int composed_h_ = 0;
  UpscaleMode composed_mode_ = UpscaleMode::Sharp2x;

  // Dummy 1x1 integer textures bound to HD sampler units when hd_active == false
  // so every sampler uniform always points at a complete, matching texture type.
  GLuint tex_dummy_rgba8ui_ = 0;
  GLuint tex_dummy_r16ui_ = 0;

  // Incremental upload / compose cursors (in GpuFrameMailbox's unified `gen`).
  int uploaded_src_w_ = 0;
  int uploaded_src_h_ = 0;
  std::uint64_t uploaded_vram_gen_ = 0;
  std::uint64_t uploaded_palette_gen_ = 0;
  std::uint64_t uploaded_glyph_gen_ = 0;
  std::uint64_t uploaded_hd_epoch_ = 0;
  std::uint64_t uploaded_hd_gen_ = 0;
  std::uint64_t uploaded_lut_idx_gen_ = 0;
  std::uint64_t uploaded_luts_gen_ = 0;
  std::uint64_t composed_gen_ = 0;

  std::vector<UpscaleDirtyRect> rect_buf_;
};

}  // namespace san4::gpu
