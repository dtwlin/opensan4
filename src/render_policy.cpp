#include "render_policy.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace san4 {
namespace {

bool contains_ci(std::string_view haystack, std::string_view needle) {
  const auto it = std::search(
      haystack.begin(), haystack.end(), needle.begin(), needle.end(),
      [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
      });
  return it != haystack.end();
}

}  // namespace

bool parse_renderer_preference(std::string_view text, RendererPreference& out) {
  if (text == "auto") {
    out = RendererPreference::Auto;
  } else if (text == "gpu") {
    out = RendererPreference::Gpu;
  } else if (text == "cpu") {
    out = RendererPreference::Cpu;
  } else {
    return false;
  }
  return true;
}

const char* render_profile_name(RenderProfile profile) {
  return profile == RenderProfile::Software ? "sw" : "hw";
}

bool is_software_gl_renderer(std::string_view gl_renderer) {
  static constexpr std::array<std::string_view, 4> kCpuRasterizers = {
      "llvmpipe", "softpipe", "swrast", "SVGA3D"};
  return std::any_of(
      kCpuRasterizers.begin(), kCpuRasterizers.end(),
      [&](std::string_view name) { return contains_ci(gl_renderer, name); });
}

RenderProfile select_render_profile(RendererPreference pref,
                                    std::string_view driver_name,
                                    std::string_view gl_renderer) {
  if (pref == RendererPreference::Gpu) {
    return RenderProfile::Hardware;
  }
  if (pref == RendererPreference::Cpu) {
    return RenderProfile::Software;
  }
  if (driver_name == "software" || is_software_gl_renderer(gl_renderer)) {
    return RenderProfile::Software;
  }
  return RenderProfile::Hardware;
}

bool select_vsync(RenderProfile profile, int refresh_hz) {
  return profile == RenderProfile::Hardware && refresh_hz >= kMinVsyncRefreshHz;
}

UpscaleMode select_auto_upscale_mode(const AutoScaleInputs& in) {
  if (in.explicit_scale || in.user_cycled || !in.hd_pack ||
      in.drawable_h <= 0) {
    return in.current;
  }
  if (in.profile == RenderProfile::Software &&
      in.drawable_h <= kSoftwareXbr2xMaxDrawableH) {
    return UpscaleMode::Xbr2x;
  }
  return UpscaleMode::Xbr4x;
}

bool use_dummy_video_driver(bool headless, const char* sdl_videodriver_env) {
  return headless &&
         (sdl_videodriver_env == nullptr || sdl_videodriver_env[0] == '\0');
}

const char* preferred_render_driver(bool is_windows) noexcept {
  return is_windows ? "direct3d11" : "opengles2";
}

}  // namespace san4
