#include "gpu/gl_renderer.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "gpu/gl_shaders.h"

namespace san4::gpu {

GlRenderer::~GlRenderer() { shutdown(); }

bool GlRenderer::compile_program(const char* vert_src, const char* frag_src,
                                 GLuint& out_prog, std::string* out_error) {
  out_prog = 0;
  auto compile_stage = [&](GLenum type, const char* src, GLuint& sh) -> bool {
    sh = gl_.CreateShader(type);
    if (sh == 0u) {
      if (out_error != nullptr) *out_error = "glCreateShader returned 0";
      return false;
    }
    gl_.ShaderSource(sh, 1, &src, nullptr);
    gl_.CompileShader(sh);
    GLint ok = 0;
    gl_.GetShaderiv(sh, kGlCompileStatus, &ok);
    if (ok == 0) {
      if (out_error != nullptr) {
        GLint len = 0;
        gl_.GetShaderiv(sh, kGlInfoLogLength, &len);
        std::string log(static_cast<std::size_t>(std::max(len, 1)), '\0');
        gl_.GetShaderInfoLog(sh, static_cast<GLsizei>(log.size()), nullptr,
                             log.data());
        *out_error = std::string("Shader compile failed: ") + log.c_str();
      }
      gl_.DeleteShader(sh);
      sh = 0u;
      return false;
    }
    return true;
  };

  GLuint vs = 0;
  GLuint fs = 0;
  if (!compile_stage(kGlVertexShader, vert_src, vs)) {
    return false;
  }
  if (!compile_stage(kGlFragmentShader, frag_src, fs)) {
    gl_.DeleteShader(vs);
    return false;
  }

  const GLuint prog = gl_.CreateProgram();
  if (prog == 0u) {
    gl_.DeleteShader(vs);
    gl_.DeleteShader(fs);
    if (out_error != nullptr) *out_error = "glCreateProgram returned 0";
    return false;
  }
  gl_.AttachShader(prog, vs);
  gl_.AttachShader(prog, fs);
  gl_.LinkProgram(prog);
  gl_.DeleteShader(vs);
  gl_.DeleteShader(fs);

  GLint linked = 0;
  gl_.GetProgramiv(prog, kGlLinkStatus, &linked);
  if (linked == 0) {
    if (out_error != nullptr) {
      GLint len = 0;
      gl_.GetProgramiv(prog, kGlInfoLogLength, &len);
      std::string log(static_cast<std::size_t>(std::max(len, 1)), '\0');
      gl_.GetProgramInfoLog(prog, static_cast<GLsizei>(log.size()), nullptr,
                            log.data());
      *out_error = std::string("Program link failed: ") + log.c_str();
    }
    gl_.DeleteProgram(prog);
    return false;
  }

  out_prog = prog;
  return true;
}

bool GlRenderer::init(const GlProcs& procs, std::string* out_error) {
  shutdown();
  gl_ = procs;

  gl_.GetIntegerv(kGlMaxTextureSize, &max_tex_size_);
  if (max_tex_size_ < 4096) {
    if (out_error != nullptr) {
      *out_error = "GL_MAX_TEXTURE_SIZE < 4096";
    }
    return false;
  }

  if (!compile_program(fullscreen_vertex_shader_src(),
                       upscale_composite_fragment_shader_src(), prog_compose_,
                       out_error)) {
    shutdown();
    return false;
  }
  if (!compile_program(fullscreen_vertex_shader_src(),
                       present_fragment_shader_src(), prog_present_,
                       out_error)) {
    shutdown();
    return false;
  }

  gl_.GenVertexArrays(1, &vao_);
  gl_.GenFramebuffers(1, &fbo_composed_);
  if (vao_ == 0u || fbo_composed_ == 0u) {
    if (out_error != nullptr) *out_error = "Failed to allocate GL VAO/FBO";
    shutdown();
    return false;
  }

  auto init_int_tex = [&](GLuint& tex, GLint internal_fmt, GLsizei w, GLsizei h,
                          GLenum fmt, GLenum type, const void* zero_data,
                          GLint unpack_align) {
    gl_.GenTextures(1, &tex);
    gl_.BindTexture(kGlTexture2D, tex);
    gl_.TexParameteri(kGlTexture2D, kGlTextureMinFilter,
                      static_cast<GLint>(kGlNearest));
    gl_.TexParameteri(kGlTexture2D, kGlTextureMagFilter,
                      static_cast<GLint>(kGlNearest));
    gl_.TexParameteri(kGlTexture2D, kGlTextureWrapS,
                      static_cast<GLint>(kGlClampToEdge));
    gl_.TexParameteri(kGlTexture2D, kGlTextureWrapT,
                      static_cast<GLint>(kGlClampToEdge));
    gl_.PixelStorei(kGlUnpackAlignment, unpack_align);
    gl_.PixelStorei(kGlUnpackRowLength, 0);
    gl_.PixelStorei(kGlUnpackSkipPixels, 0);
    gl_.PixelStorei(kGlUnpackSkipRows, 0);
    gl_.TexImage2D(kGlTexture2D, 0, internal_fmt, w, h, 0, fmt, type,
                   zero_data);
  };

  std::vector<std::uint8_t> zero_vram(
      static_cast<std::size_t>(VesaPlanarVideo::kWidth) *
          VesaPlanarVideo::kHeight,
      0u);
  std::array<std::uint8_t, VesaPlanarVideo::kTileCount> zero_glyph{};
  const std::uint32_t zero_u32 = 0u;
  const std::uint16_t zero_u16 = 0u;

  while (gl_.GetError() != kGlNoError) {
  }

  init_int_tex(tex_vram_, static_cast<GLint>(kGlR8ui), VesaPlanarVideo::kWidth,
               VesaPlanarVideo::kHeight, kGlRedInteger, kGlUnsignedByte,
               zero_vram.data(), 1);
  init_int_tex(tex_glyph_, static_cast<GLint>(kGlR8ui),
               VesaPlanarVideo::kTilesX, VesaPlanarVideo::kTilesY,
               kGlRedInteger, kGlUnsignedByte, zero_glyph.data(), 1);
  init_int_tex(tex_dummy_rgba8ui_, static_cast<GLint>(kGlRgba8ui), 1, 1,
               kGlRgbaInteger, kGlUnsignedByte, &zero_u32, 4);
  init_int_tex(tex_dummy_r16ui_, static_cast<GLint>(kGlR16ui), 1, 1,
               kGlRedInteger, kGlUnsignedShort, &zero_u16, 2);
  gl_.PixelStorei(kGlUnpackAlignment, 4);

  if (gl_.GetError() != kGlNoError || tex_vram_ == 0u || tex_glyph_ == 0u ||
      tex_dummy_rgba8ui_ == 0u || tex_dummy_r16ui_ == 0u) {
    if (out_error != nullptr) {
      *out_error = "Failed to create base integer textures";
    }
    shutdown();
    return false;
  }

  loc_comp_flip_y_ = gl_.GetUniformLocation(prog_compose_, "u_flip_y");
  loc_comp_vram_ = gl_.GetUniformLocation(prog_compose_, "u_vram");
  loc_comp_glyph_ = gl_.GetUniformLocation(prog_compose_, "u_glyph");
  loc_comp_hd_canvas_ = gl_.GetUniformLocation(prog_compose_, "u_hd_canvas");
  loc_comp_hd_lut_idx_ = gl_.GetUniformLocation(prog_compose_, "u_hd_lut_idx");
  loc_comp_luts_ = gl_.GetUniformLocation(prog_compose_, "u_luts");
  loc_comp_palette_ = gl_.GetUniformLocation(prog_compose_, "u_palette");
  loc_comp_lut_slot_map_ =
      gl_.GetUniformLocation(prog_compose_, "u_lut_slot_map");
  loc_comp_src_size_ = gl_.GetUniformLocation(prog_compose_, "u_src_size");
  loc_comp_mode_ = gl_.GetUniformLocation(prog_compose_, "u_mode");
  loc_comp_scale_ = gl_.GetUniformLocation(prog_compose_, "u_scale");
  loc_comp_hd_active_ = gl_.GetUniformLocation(prog_compose_, "u_hd_active");

  gl_.UseProgram(prog_compose_);
  gl_.Uniform1i(loc_comp_flip_y_, 0);
  gl_.Uniform1i(loc_comp_vram_, 0);
  gl_.Uniform1i(loc_comp_glyph_, 1);
  gl_.Uniform1i(loc_comp_hd_canvas_, 2);
  gl_.Uniform1i(loc_comp_hd_lut_idx_, 3);
  gl_.Uniform1i(loc_comp_luts_, 4);
  std::array<GLint, GpuFrameMailbox::kMaxKeySlots> init_map{};
  init_map.fill(-1);
  gl_.Uniform1iv(loc_comp_lut_slot_map_,
                 static_cast<GLsizei>(init_map.size()), init_map.data());

  loc_pres_flip_y_ = gl_.GetUniformLocation(prog_present_, "u_flip_y");
  loc_pres_frame_ = gl_.GetUniformLocation(prog_present_, "u_frame");
  gl_.UseProgram(prog_present_);
  gl_.Uniform1i(loc_pres_flip_y_, 1);
  gl_.Uniform1i(loc_pres_frame_, 0);
  gl_.UseProgram(0u);

  reset_tracking();
  initialized_ = true;
  return true;
}

void GlRenderer::reset_tracking() noexcept {
  uploaded_src_w_ = 0;
  uploaded_src_h_ = 0;
  uploaded_vram_gen_ = 0;
  uploaded_palette_gen_ = 0;
  uploaded_glyph_gen_ = 0;
  uploaded_hd_epoch_ = 0;
  uploaded_hd_gen_ = 0;
  uploaded_lut_idx_gen_ = 0;
  uploaded_luts_gen_ = 0;
  composed_gen_ = 0;
}

void GlRenderer::shutdown() noexcept {
  if (gl_.DeleteTextures != nullptr) {
    const GLuint texs[] = {tex_vram_,         tex_glyph_,       tex_hd_canvas_,
                           tex_hd_lut_idx_,   tex_luts_,        tex_composed_,
                           tex_dummy_rgba8ui_, tex_dummy_r16ui_};
    for (GLuint t : texs) {
      if (t != 0u) {
        gl_.DeleteTextures(1, &t);
      }
    }
  }
  tex_vram_ = 0;
  tex_glyph_ = 0;
  tex_hd_canvas_ = 0;
  tex_hd_lut_idx_ = 0;
  tex_luts_ = 0;
  lut_atlas_rows_ = 0;
  tex_composed_ = 0;
  tex_dummy_rgba8ui_ = 0;
  tex_dummy_r16ui_ = 0;

  if (fbo_composed_ != 0u && gl_.DeleteFramebuffers != nullptr) {
    gl_.DeleteFramebuffers(1, &fbo_composed_);
  }
  fbo_composed_ = 0;
  composed_w_ = 0;
  composed_h_ = 0;

  if (vao_ != 0u && gl_.DeleteVertexArrays != nullptr) {
    gl_.DeleteVertexArrays(1, &vao_);
  }
  vao_ = 0;

  if (prog_compose_ != 0u && gl_.DeleteProgram != nullptr) {
    gl_.DeleteProgram(prog_compose_);
  }
  prog_compose_ = 0;
  if (prog_present_ != 0u && gl_.DeleteProgram != nullptr) {
    gl_.DeleteProgram(prog_present_);
  }
  prog_present_ = 0;

  reset_tracking();
  initialized_ = false;
}

void GlRenderer::update_composed_filter(UpscaleMode mode) {
  if (tex_composed_ == 0u) {
    return;
  }
  const GLint filter =
      static_cast<GLint>((mode == UpscaleMode::Nearest) ? kGlNearest : kGlLinear);
  gl_.BindTexture(kGlTexture2D, tex_composed_);
  gl_.TexParameteri(kGlTexture2D, kGlTextureMinFilter, filter);
  gl_.TexParameteri(kGlTexture2D, kGlTextureMagFilter, filter);
}

bool GlRenderer::ensure_composed_target(int w, int h, UpscaleMode mode) {
  if (w <= 0 || h <= 0 || w > max_tex_size_ || h > max_tex_size_) {
    return false;
  }
  if (tex_composed_ != 0u && composed_w_ == w && composed_h_ == h) {
    if (composed_mode_ != mode) {
      composed_mode_ = mode;
      update_composed_filter(mode);
      composed_gen_ = 0u;
    }
    return true;
  }

  while (gl_.GetError() != kGlNoError) {
  }

  if (tex_composed_ == 0u) {
    gl_.GenTextures(1, &tex_composed_);
    if (tex_composed_ == 0u) {
      return false;
    }
  }
  gl_.BindTexture(kGlTexture2D, tex_composed_);
  gl_.TexParameteri(kGlTexture2D, kGlTextureWrapS,
                    static_cast<GLint>(kGlClampToEdge));
  gl_.TexParameteri(kGlTexture2D, kGlTextureWrapT,
                    static_cast<GLint>(kGlClampToEdge));
  update_composed_filter(mode);
  gl_.TexImage2D(kGlTexture2D, 0, static_cast<GLint>(kGlRgba8), w, h, 0,
                 kGlRgba, kGlUnsignedByte, nullptr);
  if (gl_.GetError() != kGlNoError) {
    gl_.DeleteTextures(1, &tex_composed_);
    tex_composed_ = 0;
    composed_w_ = 0;
    composed_h_ = 0;
    return false;
  }

  gl_.BindFramebuffer(kGlFramebuffer, fbo_composed_);
  gl_.FramebufferTexture2D(kGlFramebuffer, kGlColorAttachment0, kGlTexture2D,
                           tex_composed_, 0);
  const GLenum status = gl_.CheckFramebufferStatus(kGlFramebuffer);
  gl_.BindFramebuffer(kGlFramebuffer, 0u);
  if (status != kGlFramebufferComplete) {
    gl_.DeleteTextures(1, &tex_composed_);
    tex_composed_ = 0;
    composed_w_ = 0;
    composed_h_ = 0;
    return false;
  }

  composed_w_ = w;
  composed_h_ = h;
  composed_mode_ = mode;
  composed_gen_ = 0u;
  return true;
}

bool GlRenderer::ensure_hd_textures(std::size_t num_luts) {
  while (gl_.GetError() != kGlNoError) {
  }

  if (tex_hd_canvas_ == 0u) {
    gl_.GenTextures(1, &tex_hd_canvas_);
    gl_.BindTexture(kGlTexture2D, tex_hd_canvas_);
    gl_.TexParameteri(kGlTexture2D, kGlTextureMinFilter,
                      static_cast<GLint>(kGlNearest));
    gl_.TexParameteri(kGlTexture2D, kGlTextureMagFilter,
                      static_cast<GLint>(kGlNearest));
    gl_.TexParameteri(kGlTexture2D, kGlTextureWrapS,
                      static_cast<GLint>(kGlClampToEdge));
    gl_.TexParameteri(kGlTexture2D, kGlTextureWrapT,
                      static_cast<GLint>(kGlClampToEdge));
    gl_.TexImage2D(kGlTexture2D, 0, static_cast<GLint>(kGlRgba8ui),
                   GpuFrameMailbox::kHdCanvasW, GpuFrameMailbox::kHdCanvasH, 0,
                   kGlRgbaInteger, kGlUnsignedByte, nullptr);
    uploaded_hd_gen_ = 0u;
  }

  if (tex_hd_lut_idx_ == 0u) {
    gl_.GenTextures(1, &tex_hd_lut_idx_);
    gl_.BindTexture(kGlTexture2D, tex_hd_lut_idx_);
    gl_.TexParameteri(kGlTexture2D, kGlTextureMinFilter,
                      static_cast<GLint>(kGlNearest));
    gl_.TexParameteri(kGlTexture2D, kGlTextureMagFilter,
                      static_cast<GLint>(kGlNearest));
    gl_.TexParameteri(kGlTexture2D, kGlTextureWrapS,
                      static_cast<GLint>(kGlClampToEdge));
    gl_.TexParameteri(kGlTexture2D, kGlTextureWrapT,
                      static_cast<GLint>(kGlClampToEdge));
    gl_.TexImage2D(kGlTexture2D, 0, static_cast<GLint>(kGlR16ui),
                   VesaPlanarVideo::kWidth, VesaPlanarVideo::kHeight, 0,
                   kGlRedInteger, kGlUnsignedShort, nullptr);
    uploaded_lut_idx_gen_ = 0u;
  }

  const int needed_rows =
      std::max(1, static_cast<int>((num_luts + kLutCols - 1u) / kLutCols));
  if (tex_luts_ == 0u || needed_rows > lut_atlas_rows_) {
    const int next_rows = std::min(8, std::max(needed_rows, lut_atlas_rows_ * 2));
    if (kLutTileH * next_rows > max_tex_size_) {
      return false;
    }
    if (tex_luts_ == 0u) {
      gl_.GenTextures(1, &tex_luts_);
    }
    gl_.BindTexture(kGlTexture2D, tex_luts_);
    gl_.TexParameteri(kGlTexture2D, kGlTextureMinFilter,
                      static_cast<GLint>(kGlNearest));
    gl_.TexParameteri(kGlTexture2D, kGlTextureMagFilter,
                      static_cast<GLint>(kGlNearest));
    gl_.TexParameteri(kGlTexture2D, kGlTextureWrapS,
                      static_cast<GLint>(kGlClampToEdge));
    gl_.TexParameteri(kGlTexture2D, kGlTextureWrapT,
                      static_cast<GLint>(kGlClampToEdge));
    gl_.TexImage2D(kGlTexture2D, 0, static_cast<GLint>(kGlRgba8ui), kLutAtlasW,
                   kLutTileH * next_rows, 0, kGlRgbaInteger, kGlUnsignedByte,
                   nullptr);
    lut_atlas_rows_ = next_rows;
    uploaded_luts_gen_ = 0u;
  }

  return gl_.GetError() == kGlNoError && tex_hd_canvas_ != 0u &&
         tex_hd_lut_idx_ != 0u && tex_luts_ != 0u;
}

GlRenderer::UploadResult GlRenderer::upload_and_compose(
    const GpuFrameMailbox::Slot& slot) {
  UploadResult res{};
  if (!initialized_) {
    return res;
  }
  if (!ensure_composed_target(slot.out_w, slot.out_h, slot.mode)) {
    return res;
  }

  if (slot.src_w != uploaded_src_w_ || slot.src_h != uploaded_src_h_) {
    uploaded_src_w_ = slot.src_w;
    uploaded_src_h_ = slot.src_h;
    uploaded_vram_gen_ = 0u;
    composed_gen_ = 0u;
  }

  const int tiles_x =
      (slot.src_w + VesaPlanarVideo::kTileSize - 1) / VesaPlanarVideo::kTileSize;
  const int tiles_y =
      (slot.src_h + VesaPlanarVideo::kTileSize - 1) / VesaPlanarVideo::kTileSize;

  // 1) Incremental 800x600 VRAM upload (GL_R8UI, alignment 1).
  const std::uint64_t vram_px = GpuFrameMailbox::changed_tile_rects(
      slot.vram_tile_gen, uploaded_vram_gen_, tiles_x, tiles_y,
      VesaPlanarVideo::kTileSize, slot.src_w, slot.src_h, 0, rect_buf_);
  if (!rect_buf_.empty()) {
    gl_.BindTexture(kGlTexture2D, tex_vram_);
    gl_.PixelStorei(kGlUnpackAlignment, 1);
    gl_.PixelStorei(kGlUnpackRowLength, VesaPlanarVideo::kWidth);
    const std::uint64_t full_vram_px =
        static_cast<std::uint64_t>(slot.src_w) *
        static_cast<std::uint64_t>(slot.src_h);
    if (vram_px * 10u > full_vram_px * 6u) {
      gl_.PixelStorei(kGlUnpackSkipPixels, 0);
      gl_.PixelStorei(kGlUnpackSkipRows, 0);
      gl_.TexSubImage2D(kGlTexture2D, 0, 0, 0, slot.src_w, slot.src_h,
                        kGlRedInteger, kGlUnsignedByte, slot.vram.data());
      res.uploaded_texels += full_vram_px;
    } else {
      for (const UpscaleDirtyRect& r : rect_buf_) {
        gl_.PixelStorei(kGlUnpackSkipPixels, r.x);
        gl_.PixelStorei(kGlUnpackSkipRows, r.y);
        gl_.TexSubImage2D(kGlTexture2D, 0, r.x, r.y, r.w, r.h, kGlRedInteger,
                          kGlUnsignedByte, slot.vram.data());
      }
      res.uploaded_texels += vram_px;
    }
    gl_.PixelStorei(kGlUnpackRowLength, 0);
    gl_.PixelStorei(kGlUnpackSkipPixels, 0);
    gl_.PixelStorei(kGlUnpackSkipRows, 0);
    gl_.PixelStorei(kGlUnpackAlignment, 4);
    uploaded_vram_gen_ = slot.gen;
  }

  // 2) 50x38 Glyph tile mask upload (GL_R8UI, 1,900 bytes when changed).
  if (slot.glyph_gen > uploaded_glyph_gen_) {
    gl_.BindTexture(kGlTexture2D, tex_glyph_);
    gl_.PixelStorei(kGlUnpackAlignment, 1);
    gl_.TexSubImage2D(kGlTexture2D, 0, 0, 0, VesaPlanarVideo::kTilesX,
                      VesaPlanarVideo::kTilesY, kGlRedInteger, kGlUnsignedByte,
                      slot.glyph_tiles.data());
    gl_.PixelStorei(kGlUnpackAlignment, 4);
    res.uploaded_texels += VesaPlanarVideo::kTileCount;
    uploaded_glyph_gen_ = slot.gen;
  }

  // 3) Optional HD canvas + LUT index map + 17x17x17 LUT atlas upload.
  bool use_hd = slot.hd_active;
  if (use_hd) {
    if (!ensure_hd_textures(slot.luts.size())) {
      use_hd = false;
    } else {
      if (slot.hd_epoch != uploaded_hd_epoch_) {
        uploaded_hd_epoch_ = slot.hd_epoch;
        uploaded_hd_gen_ = 0u;
        uploaded_lut_idx_gen_ = 0u;
        uploaded_luts_gen_ = 0u;
      }

      // 3a) Incremental 3200x2400 HD canvas upload (0 bytes during fades!).
      constexpr int kHdTilePx =
          VesaPlanarVideo::kTileSize * GpuFrameMailbox::kHdScale;  // 64
      const std::uint64_t hd_px = GpuFrameMailbox::changed_tile_rects(
          slot.hd_tile_gen, uploaded_hd_gen_, VesaPlanarVideo::kTilesX,
          VesaPlanarVideo::kTilesY, kHdTilePx, GpuFrameMailbox::kHdCanvasW,
          GpuFrameMailbox::kHdCanvasH, 0, rect_buf_);
      if (!rect_buf_.empty() && !slot.hd_canvas_argb.empty()) {
        gl_.BindTexture(kGlTexture2D, tex_hd_canvas_);
        gl_.PixelStorei(kGlUnpackAlignment, 4);
        gl_.PixelStorei(kGlUnpackRowLength, GpuFrameMailbox::kHdCanvasW);
        constexpr std::uint64_t kFullHdPx =
            static_cast<std::uint64_t>(GpuFrameMailbox::kHdCanvasW) *
            GpuFrameMailbox::kHdCanvasH;
        if (hd_px * 10u > kFullHdPx * 6u) {
          gl_.PixelStorei(kGlUnpackSkipPixels, 0);
          gl_.PixelStorei(kGlUnpackSkipRows, 0);
          gl_.TexSubImage2D(kGlTexture2D, 0, 0, 0, GpuFrameMailbox::kHdCanvasW,
                            GpuFrameMailbox::kHdCanvasH, kGlRgbaInteger,
                            kGlUnsignedByte, slot.hd_canvas_argb.data());
          res.uploaded_texels += kFullHdPx;
        } else {
          for (const UpscaleDirtyRect& r : rect_buf_) {
            gl_.PixelStorei(kGlUnpackSkipPixels, r.x);
            gl_.PixelStorei(kGlUnpackSkipRows, r.y);
            gl_.TexSubImage2D(kGlTexture2D, 0, r.x, r.y, r.w, r.h,
                              kGlRgbaInteger, kGlUnsignedByte,
                              slot.hd_canvas_argb.data());
          }
          res.uploaded_texels += hd_px;
        }
        gl_.PixelStorei(kGlUnpackRowLength, 0);
        gl_.PixelStorei(kGlUnpackSkipPixels, 0);
        gl_.PixelStorei(kGlUnpackSkipRows, 0);
        uploaded_hd_gen_ = slot.gen;
      }

      // 3b) Incremental 800x600 LUT slot-index map upload (GL_R16UI).
      const std::uint64_t idx_px = GpuFrameMailbox::changed_tile_rects(
          slot.lut_idx_tile_gen, uploaded_lut_idx_gen_,
          VesaPlanarVideo::kTilesX, VesaPlanarVideo::kTilesY,
          VesaPlanarVideo::kTileSize, VesaPlanarVideo::kWidth,
          VesaPlanarVideo::kHeight, 0, rect_buf_);
      if (!rect_buf_.empty() && !slot.hd_lut_idx.empty()) {
        gl_.BindTexture(kGlTexture2D, tex_hd_lut_idx_);
        gl_.PixelStorei(kGlUnpackAlignment, 2);
        gl_.PixelStorei(kGlUnpackRowLength, VesaPlanarVideo::kWidth);
        constexpr std::uint64_t kFullLogicalPx =
            static_cast<std::uint64_t>(VesaPlanarVideo::kWidth) *
            VesaPlanarVideo::kHeight;
        if (idx_px * 10u > kFullLogicalPx * 6u) {
          gl_.PixelStorei(kGlUnpackSkipPixels, 0);
          gl_.PixelStorei(kGlUnpackSkipRows, 0);
          gl_.TexSubImage2D(kGlTexture2D, 0, 0, 0, VesaPlanarVideo::kWidth,
                            VesaPlanarVideo::kHeight, kGlRedInteger,
                            kGlUnsignedShort, slot.hd_lut_idx.data());
          res.uploaded_texels += kFullLogicalPx;
        } else {
          for (const UpscaleDirtyRect& r : rect_buf_) {
            gl_.PixelStorei(kGlUnpackSkipPixels, r.x);
            gl_.PixelStorei(kGlUnpackSkipRows, r.y);
            gl_.TexSubImage2D(kGlTexture2D, 0, r.x, r.y, r.w, r.h,
                              kGlRedInteger, kGlUnsignedShort,
                              slot.hd_lut_idx.data());
          }
          res.uploaded_texels += idx_px;
        }
        gl_.PixelStorei(kGlUnpackRowLength, 0);
        gl_.PixelStorei(kGlUnpackSkipPixels, 0);
        gl_.PixelStorei(kGlUnpackSkipRows, 0);
        gl_.PixelStorei(kGlUnpackAlignment, 4);
        uploaded_lut_idx_gen_ = slot.gen;
      }

      // 3c) Active 17x17x17 grading LUTs + u_lut_slot_map (updates on fades).
      if (slot.luts_gen > uploaded_luts_gen_) {
        gl_.BindTexture(kGlTexture2D, tex_luts_);
        gl_.PixelStorei(kGlUnpackAlignment, 4);
        gl_.PixelStorei(kGlUnpackRowLength, 0);
        const std::size_t n_luts = std::min<std::size_t>(
            slot.luts.size(),
            static_cast<std::size_t>(kLutCols * lut_atlas_rows_));
        for (std::size_t i = 0; i < n_luts; ++i) {
          const int col = static_cast<int>(i) & (kLutCols - 1);
          const int row = static_cast<int>(i) / kLutCols;
          gl_.TexSubImage2D(kGlTexture2D, 0, col * kLutTileW, row * kLutTileH,
                            kLutTileW, kLutTileH, kGlRgbaInteger,
                            kGlUnsignedByte, slot.luts[i].data());
        }
        res.uploaded_texels +=
            static_cast<std::uint64_t>(n_luts) * GpuFrameMailbox::kLutGridSize;

        gl_.UseProgram(prog_compose_);
        std::array<GLint, GpuFrameMailbox::kMaxKeySlots> gl_map{};
        for (std::size_t i = 0; i < GpuFrameMailbox::kMaxKeySlots; ++i) {
          const int idx = slot.lut_slot_map[i];
          gl_map[i] =
              (idx >= 0 && static_cast<std::size_t>(idx) < n_luts) ? idx : -1;
        }
        gl_.Uniform1iv(loc_comp_lut_slot_map_,
                       static_cast<GLsizei>(gl_map.size()), gl_map.data());
        uploaded_luts_gen_ = slot.gen;
      }
    }
  }

  // 4) Decide Pass 1 recompose region (full vs scissored dirty rects + 2px halo).
  const bool need_full_compose =
      (composed_gen_ == 0u || slot.full_recompose_gen > composed_gen_);
  if (!need_full_compose) {
    const int out_tile_px = VesaPlanarVideo::kTileSize * slot.scale;
    const int halo_px = (slot.mode == UpscaleMode::Nearest ||
                         slot.mode == UpscaleMode::Sharp2x)
                            ? 0
                            : (2 * slot.scale);
    GpuFrameMailbox::changed_tile_rects(
        slot.compose_tile_gen, composed_gen_, tiles_x, tiles_y, out_tile_px,
        slot.out_w, slot.out_h, halo_px, rect_buf_);
    if (rect_buf_.empty()) {
      res.ok = true;
      res.recomposed = false;
      return res;
    }
  }

  // 5) Pass 1: Upscale + HD/LUT Composite into `fbo_composed_`.
  gl_.BindFramebuffer(kGlFramebuffer, fbo_composed_);
  gl_.Viewport(0, 0, slot.out_w, slot.out_h);
  gl_.UseProgram(prog_compose_);

  if (slot.palette_gen > uploaded_palette_gen_ || composed_gen_ == 0u) {
    std::array<GLuint, 16 * 4> pal_uvec4{};
    for (std::size_t i = 0; i < 16u; ++i) {
      const std::uint32_t c = slot.palette_argb[i];
      pal_uvec4[i * 4 + 0] = (c >> 16) & 0xFFu;
      pal_uvec4[i * 4 + 1] = (c >> 8) & 0xFFu;
      pal_uvec4[i * 4 + 2] = c & 0xFFu;
      pal_uvec4[i * 4 + 3] = 255u;
    }
    gl_.Uniform4uiv(loc_comp_palette_, 16, pal_uvec4.data());
    uploaded_palette_gen_ = slot.gen;
  }

  gl_.Uniform2i(loc_comp_src_size_, slot.src_w, slot.src_h);
  gl_.Uniform1i(loc_comp_mode_, static_cast<GLint>(slot.mode));
  gl_.Uniform1i(loc_comp_scale_, slot.scale);
  gl_.Uniform1i(loc_comp_hd_active_, use_hd ? 1 : 0);

  gl_.ActiveTexture(kGlTexture0 + 0u);
  gl_.BindTexture(kGlTexture2D, tex_vram_);
  gl_.ActiveTexture(kGlTexture0 + 1u);
  gl_.BindTexture(kGlTexture2D, tex_glyph_);
  gl_.ActiveTexture(kGlTexture0 + 2u);
  gl_.BindTexture(kGlTexture2D, use_hd ? tex_hd_canvas_ : tex_dummy_rgba8ui_);
  gl_.ActiveTexture(kGlTexture0 + 3u);
  gl_.BindTexture(kGlTexture2D, use_hd ? tex_hd_lut_idx_ : tex_dummy_r16ui_);
  gl_.ActiveTexture(kGlTexture0 + 4u);
  gl_.BindTexture(kGlTexture2D, use_hd ? tex_luts_ : tex_dummy_rgba8ui_);
  gl_.ActiveTexture(kGlTexture0);

  gl_.BindVertexArray(vao_);
  if (need_full_compose) {
    gl_.Disable(kGlScissorTest);
    gl_.DrawArrays(kGlTriangles, 0, 3);
  } else {
    gl_.Enable(kGlScissorTest);
    for (const UpscaleDirtyRect& r : rect_buf_) {
      gl_.Scissor(r.x, r.y, r.w, r.h);
      gl_.DrawArrays(kGlTriangles, 0, 3);
    }
    gl_.Disable(kGlScissorTest);
  }

  gl_.BindVertexArray(0u);
  gl_.UseProgram(0u);
  gl_.BindFramebuffer(kGlFramebuffer, 0u);

  composed_gen_ = slot.gen;
  res.ok = true;
  res.recomposed = true;
  return res;
}

void GlRenderer::present(int drawable_w, int drawable_h, int vp_x, int vp_y,
                         int vp_w, int vp_h) {
  if (!initialized_ || drawable_w <= 0 || drawable_h <= 0) {
    return;
  }
  gl_.BindFramebuffer(kGlFramebuffer, 0u);
  gl_.Disable(kGlScissorTest);
  gl_.Viewport(0, 0, drawable_w, drawable_h);
  gl_.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  gl_.Clear(kGlColorBufferBit);

  if (tex_composed_ != 0u && vp_w > 0 && vp_h > 0) {
    const int gl_y = drawable_h - (vp_y + vp_h);
    gl_.Viewport(vp_x, gl_y, vp_w, vp_h);
    gl_.UseProgram(prog_present_);
    gl_.ActiveTexture(kGlTexture0);
    gl_.BindTexture(kGlTexture2D, tex_composed_);
    gl_.BindVertexArray(vao_);
    gl_.DrawArrays(kGlTriangles, 0, 3);
    gl_.BindVertexArray(0u);
    gl_.UseProgram(0u);
  }
}

bool GlRenderer::readback_composed_argb(
    std::vector<std::uint32_t>& out) const {
  if (!initialized_ || fbo_composed_ == 0u || tex_composed_ == 0u ||
      composed_w_ <= 0 || composed_h_ <= 0) {
    return false;
  }
  out.resize(static_cast<std::size_t>(composed_w_) *
             static_cast<std::size_t>(composed_h_));
  gl_.BindFramebuffer(kGlFramebuffer, fbo_composed_);
  gl_.PixelStorei(kGlPackAlignment, 4);
  gl_.PixelStorei(kGlPackRowLength, 0);
  gl_.ReadPixels(0, 0, composed_w_, composed_h_, kGlBgra, kGlUnsignedByte,
                 out.data());
  gl_.BindFramebuffer(kGlFramebuffer, 0u);
  return gl_.GetError() == kGlNoError;
}

}  // namespace san4::gpu
