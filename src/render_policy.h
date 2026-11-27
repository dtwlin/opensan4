#pragma once

#include <cstdint>
#include <string_view>

#include "upscaler.h"

namespace san4 {

// Pure present-path decisions (no SDL calls), so they are unit-testable.

// `--renderer=auto|gpu|cpu`.
enum class RendererPreference : std::uint8_t {
  Auto = 0,  // classify the created renderer
  Gpu,       // force the Hardware profile
  Cpu,       // force the Software profile
};

// Whether presenting runs on a real GPU or on the CPU (SDL `software`, or a
// GL driver such as llvmpipe that rasterizes on the CPU).
enum class RenderProfile : std::uint8_t {
  Hardware = 0,
  Software,
};

// Interactive host rendering backend.
enum class RenderBackendKind : std::uint8_t {
  SdlRenderer = 0,  // CPU upscaler + compositor -> SDL_Renderer
  OpenGL33 = 1,     // OpenGL 3.3 Core GPU upscaler + HD/LUT compositor
};

struct GlCapabilities {
  bool context_ok = false;
  bool procs_ok = false;
  int major_version = 0;
  int minor_version = 0;
  int max_texture_size = 0;
  std::string_view gl_renderer;
};

inline constexpr int kMinOpenGlTextureSize = 4096;

bool parse_renderer_preference(std::string_view text, RendererPreference& out);
const char* render_profile_name(RenderProfile profile);  // "hw" or "sw"
const char* render_backend_name(RenderBackendKind backend) noexcept;

// True when `gl_renderer` (the `GL_RENDERER` string) names a CPU rasterizer:
// llvmpipe, softpipe, swrast, lavapipe, VMware SVGA3D, or Windows software
// drivers (GDI Generic, Microsoft Basic Render Driver, WARP).
bool is_software_gl_renderer(std::string_view gl_renderer);

// Whether interactive startup should probe an OpenGL 3.3 Core GPU context.
bool should_probe_opengl(RendererPreference pref, bool headless) noexcept;

// Selects OpenGL 3.3 GPU shader backend when capabilities meet requirements
// (and `gl_renderer` is hardware under `Auto`), else falls back to SdlRenderer.
RenderBackendKind select_render_backend(RendererPreference pref,
                                        const GlCapabilities& caps) noexcept;

// `driver_name` is `SDL_RendererInfo::name`; `gl_renderer` is empty for non-GL
// drivers.
RenderProfile select_render_profile(RendererPreference pref,
                                    std::string_view driver_name,
                                    std::string_view gl_renderer);

// Displays slower than this are paced by the host instead of by VSync.
inline constexpr int kMinVsyncRefreshHz = 55;

// VSync only for the Hardware profile on a display of at least 55 Hz;
// `refresh_hz <= 0` means unknown and disables VSync.
bool select_vsync(RenderProfile profile, int refresh_hz);

struct AutoScaleInputs {
  RenderProfile profile = RenderProfile::Hardware;
  int drawable_h = 0;           // renderer output height in pixels
  bool hd_pack = false;         // an HD pack directory is configured
  bool explicit_scale = false;  // `--scale` was given
  bool user_cycled = false;     // the user picked a mode with F5
  UpscaleMode current = UpscaleMode::Sharp2x;
};

// Software drawables up to this height get `xbr2x` with an HD pack.
inline constexpr int kSoftwareXbr2xMaxDrawableH = 1440;

// The output scale to use: `current` when the user chose one (`--scale` or
// F5), no HD pack is configured, or the drawable size is unknown
// (`drawable_h <= 0`, e.g. while minimized); otherwise `xbr4x` for Hardware,
// and for Software `xbr2x` up to a 1440-pixel drawable height, else `xbr4x`.
UpscaleMode select_auto_upscale_mode(const AutoScaleInputs& in);

// Whether a run must force SDL's `dummy` video driver: headless runs do,
// unless the user picked a driver through the `SDL_VIDEODRIVER` environment
// variable (`sdl_videodriver_env`, nullptr when unset). Headless screenshots
// come from the upscaler, so the window is never needed, and a real display
// would leak host input into scripted runs.
bool use_dummy_video_driver(bool headless, const char* sdl_videodriver_env);

// Preferred interactive SDL renderer driver name by OS: "direct3d11" on
// Windows (hardware Direct3D 11 + inbox SIMD WARP fallback, avoiding legacy
// index-0 direct3d9) and "opengles2" on Linux.
const char* preferred_render_driver(bool is_windows) noexcept;

}  // namespace san4
