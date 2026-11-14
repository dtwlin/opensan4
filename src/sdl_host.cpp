#include "sdl_host.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "util/simd_blend.h"
#include "util/string_util.h"

namespace san4 {
namespace {

MachineConfig make_machine_config(const HostOptions& opts) {
  MachineConfig cfg;
  cfg.game_dir = opts.game_dir;
  cfg.save_dir = opts.save_dir;
  cfg.hd_pack_dir = opts.hd_pack_dir;
  cfg.dump_sprites_dir = opts.dump_sprites_dir;
  cfg.embedded_vfs = opts.embedded_vfs;
  cfg.skip_intro = opts.skip_intro;
  cfg.mouse_warp = opts.mouse_warp;
  cfg.speed_multiplier = opts.speed_multiplier;
  cfg.deterministic = opts.deterministic;
  return cfg;
}

// `GL_RENDERER` of the renderer's current context, or "" for non-GL drivers.
// glGetString is the only raw GL call the host makes.
std::string query_gl_renderer(std::string_view driver_name) {
  if (!driver_name.starts_with("opengl")) {
    return {};
  }
  using GlGetStringFn = const unsigned char* (*)(unsigned int);
  const auto gl_get_string =
      reinterpret_cast<GlGetStringFn>(SDL_GL_GetProcAddress("glGetString"));
  if (gl_get_string == nullptr) {
    return {};
  }
  constexpr unsigned int kGlRenderer = 0x1F01u;
  const unsigned char* name = gl_get_string(kGlRenderer);
  return name != nullptr ? std::string(reinterpret_cast<const char*>(name))
                         : std::string();
}

int find_render_driver_index(std::string_view name) {
  const int n = SDL_GetNumRenderDrivers();
  for (int i = 0; i < n; ++i) {
    SDL_RendererInfo info{};
    if (SDL_GetRenderDriverInfo(i, &info) == 0 && info.name != nullptr &&
        name == info.name) {
      return i;
    }
  }
  return -1;
}

std::vector<std::string> split_tokens(const std::string& s, char delim) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, delim)) {
    if (!item.empty()) {
      out.push_back(item);
    }
  }
  return out;
}

std::uint8_t ascii_to_xt_scancode(char raw_ch) {
  const char ch = static_cast<char>(
      std::tolower(static_cast<unsigned char>(raw_ch)));
  if (ch >= '1' && ch <= '9') {
    return static_cast<std::uint8_t>(0x02u + (ch - '1'));
  }
  switch (ch) {
    case '0': return 0x0Bu;
    case '-': case '_': return 0x0Cu;
    case '=': case '+': return 0x0Du;
    case 'q': return 0x10u;
    case 'w': return 0x11u;
    case 'e': return 0x12u;
    case 'r': return 0x13u;
    case 't': return 0x14u;
    case 'y': return 0x15u;
    case 'u': return 0x16u;
    case 'i': return 0x17u;
    case 'o': return 0x18u;
    case 'p': return 0x19u;
    case '[': case '{': return 0x1Au;
    case ']': case '}': return 0x1Bu;
    case 'a': return 0x1Eu;
    case 's': return 0x1Fu;
    case 'd': return 0x20u;
    case 'f': return 0x21u;
    case 'g': return 0x22u;
    case 'h': return 0x23u;
    case 'j': return 0x24u;
    case 'k': return 0x25u;
    case 'l': return 0x26u;
    case ';': case ':': return 0x27u;
    case '\'': case '"': return 0x28u;
    case '`': case '~': return 0x29u;
    case '\\': case '|': return 0x2Bu;
    case 'z': return 0x2Cu;
    case 'x': return 0x2Du;
    case 'c': return 0x2Eu;
    case 'v': return 0x2Fu;
    case 'b': return 0x30u;
    case 'n': return 0x31u;
    case 'm': return 0x32u;
    case ',': case '<': return 0x33u;
    case '.': case '>': return 0x34u;
    case '/': case '?': return 0x35u;
    case ' ': return 0x39u;
    default: return 0x00u;
  }
}

}  // namespace

SdlHost::SdlHost(const HostOptions& opts)
    : opts_(opts),
      pool_(std::make_unique<ThreadPool>(opts.threads)),
      machine_(make_machine_config(opts)),
      upscaler_(4),
      session_(machine_, upscaler_),
      fullscreen_(opts.fullscreen) {
  upscaler_.set_mode(opts.upscale_mode);
  upscaler_.set_thread_pool(pool_.get());
  upscaler_.set_dirty_policy(UpscaleDirtyPolicy::TileRowSpans);
  machine_.hd_pack().set_thread_pool(pool_.get());
  mailbox_.set_thread_pool(pool_.get());
  cached_cursor_shape_ = machine_.mouse().cursor_shape();
  cached_cursor_visible_ = machine_.mouse().cursor_visible();

  if (!opts_.headless_script.empty()) {
    // Headless runs keep dynamic-CG re-upscales synchronous so scripted
    // screenshots do not depend on background job timing, never warp the
    // host pointer, and apply cursor changes on the spot.
    machine_.mouse().set_cursor_callback(
        [this](const DosCursorShape& shape, bool visible) {
          cached_cursor_shape_ = shape;
          cached_cursor_visible_ = visible;
          rebuild_hardware_cursor();
        });
    return;
  }
  machine_.hd_pack().set_async_pool(pool_.get());
  // These fire on the emulation thread; the main thread applies them.
  machine_.mouse().set_warp_callback([this](int dx, int dy) {
    HostEvent ev;
    ev.type = HostEvent::Type::Warp;
    ev.x = dx;
    ev.y = dy;
    ev.active_w = machine_.video().active_width();
    ev.active_h = machine_.video().active_height();
    event_queue_.push(std::move(ev));
  });
  machine_.mouse().set_cursor_callback(
      [this](const DosCursorShape& shape, bool visible) {
        HostEvent ev;
        ev.type = HostEvent::Type::Cursor;
        ev.shape = shape;
        ev.visible = visible;
        event_queue_.push(std::move(ev));
      });
}

SdlHost::~SdlHost() {
  // The emulation thread must stop before any SDL object goes away.
  emu_thread_.shutdown();
  shutdown_sdl();
}

bool SdlHost::init_sdl() {
  struct InitCleanupGuard {
    SdlHost* self = nullptr;
    bool committed = false;
    ~InitCleanupGuard() {
      if (!committed && self != nullptr) {
        self->shutdown_sdl();
      }
    }
  } guard{this, false};

  SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
  SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
  const bool headless = !opts_.headless_script.empty();
  if (headless) {
    // Scripted runs ignore SDL_QUIT, so SIGINT / SIGTERM must keep their
    // default (terminating) action instead of becoming SDL_QUIT events.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  }
  if (use_dummy_video_driver(headless, std::getenv("SDL_VIDEODRIVER"))) {
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
  }

  SDL_SetMainReady();
  constexpr std::uint32_t kVideoTimerFlags = SDL_INIT_VIDEO | SDL_INIT_TIMER;
  if (SDL_Init(kVideoTimerFlags) != 0) {
    if (headless) {
      // An explicitly requested driver failed; the environment variable
      // outranks normal-priority hints.
      SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "dummy", SDL_HINT_OVERRIDE);
      if (SDL_Init(kVideoTimerFlags) != 0) {
        std::cerr << "[san4] SDL video init failed: " << SDL_GetError() << "\n";
        return false;
      }
    } else {
      std::cerr << "[san4] SDL video init failed: " << SDL_GetError() << "\n";
      return false;
    }
  }
  sdl_init_subsystems_ |= kVideoTimerFlags;

  const int scale = std::clamp(opts_.window_scale, 1, 8);
  int win_w = VesaPlanarVideo::kWidth * scale;
  int win_h = VesaPlanarVideo::kHeight * scale;
  if (!headless && !fullscreen_) {
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0 && usable.w > 0 &&
        usable.h > 0) {
      const Viewport clamped =
          clamp_initial_window_size(win_w, win_h, usable.w, usable.h);
      win_w = clamped.w;
      win_h = clamped.h;
    }
  }
  Uint32 win_flags =
      SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
  if (fullscreen_) {
    win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
  }

#ifdef _WIN32
  constexpr const char* kWindowTitleBase = "SAN4 Native Windows";
#else
  constexpr const char* kWindowTitleBase = "SAN4 Native Linux";
#endif
  window_ = SDL_CreateWindow(kWindowTitleBase, SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED, win_w, win_h, win_flags);
  if (!window_) {
    std::cerr << "[san4] SDL_CreateWindow failed: " << SDL_GetError() << "\n";
    return false;
  }

  if (opts_.headless_script.empty()) {
    if (!create_interactive_renderer(true)) {
      return false;
    }
  } else {
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer_) {
      renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer_) {
      std::cerr << "[san4] SDL_CreateRenderer failed: " << SDL_GetError()
                << "\n";
      return false;
    }
  }

  if (!recreate_frame_texture(upscaler_.output_width(),
                              upscaler_.output_height(), upscaler_.mode())) {
    if (upscaler_.mode() != UpscaleMode::Nearest) {
      upscaler_.set_mode(UpscaleMode::Nearest);
      if (!recreate_frame_texture(upscaler_.output_width(),
                                  upscaler_.output_height(),
                                  upscaler_.mode())) {
        return false;
      }
    } else {
      return false;
    }
  }

  // Open 44.1 kHz stereo 16-bit audio output when available; headless/CI
  // environments without a host sound server fall back cleanly to main-loop
  // ring buffer draining.
  if (opts_.headless_script.empty()) {
    open_audio_device(true);
  }

  rebuild_hardware_cursor();
  update_window_title_inline();
  guard.committed = true;
  return true;
}

void SdlHost::open_audio_device(bool verbose_log) {
  if (audio_dev_ != 0) {
    return;
  }
  const char* env_driver = SDL_getenv("SDL_AUDIODRIVER");
  const bool has_explicit_driver =
      (env_driver != nullptr && *env_driver != '\0');
  if ((sdl_init_subsystems_ & SDL_INIT_AUDIO) == 0u) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
      if (verbose_log) {
        std::cout << "[san4-audio] unavailable: " << SDL_GetError() << "\n";
      }
      return;
    }
    sdl_init_subsystems_ |= SDL_INIT_AUDIO;
  } else if (SDL_GetCurrentAudioDriver() == nullptr) {
    if (SDL_AudioInit(has_explicit_driver ? env_driver : nullptr) != 0) {
      if (verbose_log) {
        std::cout << "[san4-audio] unavailable: " << SDL_GetError() << "\n";
      }
      return;
    }
  }

  SDL_AudioSpec want{};
  SDL_AudioSpec have{};
  want.freq = Opl2Audio::kSampleRate;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 1024;  // ~23.2 ms slice resilient to Win11 background wakeups
  want.callback = &SdlHost::sdl_audio_callback;
  want.userdata = this;
  audio_dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  // Some backends initialize successfully but expose no usable default
  // device (e.g. PipeWire without a session sink, seen with sdl2-compat on
  // SDL3). Unless the user pinned SDL_AUDIODRIVER, try the remaining real
  // backends in SDL's preference order before giving up.
  if (audio_dev_ == 0 && !has_explicit_driver) {
    const std::string first_error = SDL_GetError();
    const char* failed = SDL_GetCurrentAudioDriver();
    const std::string failed_driver = failed != nullptr ? failed : "";
    const int num_drivers = SDL_GetNumAudioDrivers();
    for (int i = 0; i < num_drivers && audio_dev_ == 0; ++i) {
      const char* name = SDL_GetAudioDriver(i);
      if (name == nullptr || failed_driver == name ||
          std::strcmp(name, "disk") == 0 || std::strcmp(name, "dummy") == 0) {
        continue;
      }
      SDL_AudioQuit();
      if (SDL_AudioInit(name) == 0) {
        audio_dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
      }
    }
    if (audio_dev_ == 0) {
      // Restore the primary hotplug-capable backend (e.g. WASAPI on Windows 11
      // or PipeWire/PulseAudio on Linux) so future SDL_AUDIODEVICEADDED events
      // still fire when headphones or an HDMI sink are connected later.
      if (!failed_driver.empty()) {
        const char* cur = SDL_GetCurrentAudioDriver();
        if (cur == nullptr || failed_driver != cur) {
          SDL_AudioQuit();
          SDL_AudioInit(failed_driver.c_str());
        }
      }
      SDL_SetError("%s", first_error.c_str());
    }
  }
  // Discard synchronous initial device-enumeration events queued by
  // SDL_InitSubSystem / SDL_AudioInit; only subsequent hotplug arrivals should
  // trigger recovery in process_sdl_events().
  SDL_FlushEvent(SDL_AUDIODEVICEADDED);

  if (audio_dev_ != 0) {
    SDL_PauseAudioDevice(audio_dev_, 0);
    if (verbose_log) {
      const char* audio_driver = SDL_GetCurrentAudioDriver();
      std::cout << "[san4-audio] driver="
                << (audio_driver != nullptr ? audio_driver : "unknown")
                << " freq=" << have.freq
                << " channels=" << static_cast<int>(have.channels)
                << " samples=" << have.samples << "\n";
    }
  } else if (verbose_log) {
    std::cout << "[san4-audio] unavailable: " << SDL_GetError() << "\n";
  }
}

bool SdlHost::create_interactive_renderer(bool verbose_log) {
  // On Windows, prefer direct3d11 (hardware D3D11 + inbox WARP fallback) over
  // SDL's index-0 legacy direct3d9 driver; on Linux, opengles2 uploads far
  // faster than desktop GL under llvmpipe and is just as fast on real GPUs.
  // Fall back to SDL's default choice, then software. VSync is enabled
  // afterwards because it depends on the detected profile.
#ifdef _WIN32
  constexpr bool kIsWindows = true;
#else
  constexpr bool kIsWindows = false;
#endif
  const int pref_index =
      find_render_driver_index(preferred_render_driver(kIsWindows));
  if (pref_index >= 0) {
    renderer_ =
        SDL_CreateRenderer(window_, pref_index, SDL_RENDERER_ACCELERATED);
  }
  if (!renderer_) {
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
  }
  if (!renderer_) {
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
  }
  if (!renderer_) {
    std::cerr << "[san4] SDL_CreateRenderer failed: " << SDL_GetError() << "\n";
    return false;
  }

  SDL_RendererInfo rinfo{};
  const std::string driver =
      (SDL_GetRendererInfo(renderer_, &rinfo) == 0 && rinfo.name != nullptr)
          ? std::string(rinfo.name)
          : std::string();
  const std::string gl_renderer = query_gl_renderer(driver);
  render_profile_ = select_render_profile(opts_.renderer, driver, gl_renderer);

  SDL_DisplayMode display_mode{};
  const int refresh_hz =
      (SDL_GetWindowDisplayMode(window_, &display_mode) == 0)
          ? display_mode.refresh_rate
          : 0;
#if SDL_VERSION_ATLEAST(2, 0, 18)
  has_vsync_ = select_vsync(render_profile_, refresh_hz) &&
               SDL_RenderSetVSync(renderer_, 1) == 0;
#else
  // VSync cannot be enabled after renderer creation before SDL 2.0.18; the
  // host's present pacing takes over.
  (void)refresh_hz;
  has_vsync_ = false;
#endif

  if (!emu_thread_.running()) {
    session_.set_auto_scale_policy(EmuSession::AutoScalePolicy{
        .profile = render_profile_,
        .hd_pack = !opts_.hd_pack_dir.empty(),
        .explicit_scale = opts_.explicit_scale,
    });
  }
  apply_auto_scale();
  if (verbose_log) {
    const UpscaleMode active_mode =
        emu_thread_.running() ? view_status_.mode : upscaler_.mode();
    std::cout << "[san4-render] driver=" << (driver.empty() ? "unknown" : driver)
              << " gl_renderer=" << (gl_renderer.empty() ? "none" : gl_renderer)
              << " profile=" << render_profile_name(render_profile_)
              << " vsync=" << (has_vsync_ ? 1 : 0)
              << " scale=" << PixelUpscaler::mode_name(active_mode)
              << " simd=" << simd::level_name(simd::active_level()) << "\n";
  }
  return true;
}

bool SdlHost::recreate_renderer_after_reset() {
  if (window_ == nullptr) {
    return false;
  }
  if (texture_ != nullptr) {
    SDL_DestroyTexture(texture_);
    texture_ = nullptr;
  }
  tex_w_ = 0;
  tex_h_ = 0;
  if (renderer_ != nullptr) {
    SDL_DestroyRenderer(renderer_);
    renderer_ = nullptr;
  }
  if (opts_.headless_script.empty()) {
    return create_interactive_renderer(false);
  }
  renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
  if (!renderer_) {
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
  }
  return renderer_ != nullptr;
}

void SdlHost::apply_auto_scale() {
  int out_w = 0;
  int out_h = 0;
  if (!opts_.headless_script.empty() || renderer_ == nullptr ||
      SDL_GetRendererOutputSize(renderer_, &out_w, &out_h) != 0) {
    return;
  }
  send(HostCmd::auto_scale(out_h));
  update_window_title_inline();
}

void SdlHost::shutdown_sdl() {
  if (audio_dev_ != 0) {
    SDL_CloseAudioDevice(audio_dev_);
    audio_dev_ = 0;
  }
  if (color_cursor_) {
    SDL_Cursor* def_cur = SDL_GetDefaultCursor();
    if (def_cur) {
      SDL_SetCursor(def_cur);
    }
    SDL_FreeCursor(color_cursor_);
    color_cursor_ = nullptr;
  }
  if (texture_) {
    SDL_DestroyTexture(texture_);
    texture_ = nullptr;
  }
  tex_w_ = 0;
  tex_h_ = 0;
  if (renderer_) {
    SDL_DestroyRenderer(renderer_);
    renderer_ = nullptr;
  }
  if (window_) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }
  if (sdl_init_subsystems_ != 0u) {
    SDL_QuitSubSystem(sdl_init_subsystems_);
    SDL_Quit();
    sdl_init_subsystems_ = 0u;
  }
}

void SdlHost::sdl_audio_callback(void* userdata, std::uint8_t* stream, int len) {
  if (len <= 0 || stream == nullptr || userdata == nullptr) {
    return;
  }
  auto* self = static_cast<SdlHost*>(userdata);
  constexpr std::size_t kBytesPerFrame = 2u * sizeof(std::int16_t);
  constexpr std::uint32_t kChunkFrames = 1024u;
  alignas(std::int16_t) std::int16_t chunk[kChunkFrames * 2u];

  std::size_t remaining_bytes = static_cast<std::size_t>(len);
  std::uint8_t* dst = stream;
  while (remaining_bytes >= kBytesPerFrame) {
    const auto avail_frames =
        static_cast<std::uint32_t>(remaining_bytes / kBytesPerFrame);
    const std::uint32_t pull_n = std::min(avail_frames, kChunkFrames);
    self->machine_.audio().pull_stereo_s16(chunk, pull_n);
    const std::size_t copy_bytes =
        static_cast<std::size_t>(pull_n) * kBytesPerFrame;
    std::memcpy(dst, chunk, copy_bytes);
    dst += copy_bytes;
    remaining_bytes -= copy_bytes;
  }
  if (remaining_bytes > 0u) {
    std::memset(dst, 0, remaining_bytes);
  }
}

bool SdlHost::recreate_frame_texture(int w, int h, UpscaleMode mode) {
  if (texture_) {
    SDL_DestroyTexture(texture_);
    texture_ = nullptr;
  }
  tex_w_ = 0;
  tex_h_ = 0;
  if (!renderer_ || w <= 0 || h <= 0) {
    return false;
  }
  // Linear filtering on pre-upscaled 2x/4x surfaces gives crisp Sharp-Bilinear
  // / supersampled edges at arbitrary window sizes; Nearest keeps raw 1x crisp.
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,
              (mode == UpscaleMode::Nearest) ? "0" : "1");
  texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
                               SDL_TEXTUREACCESS_STREAMING, w, h);
  if (!texture_) {
    std::cerr << "[san4] SDL_CreateTexture failed: " << SDL_GetError() << "\n";
    return false;
  }
#if SDL_VERSION_ATLEAST(2, 0, 12)
  // Failure is non-fatal: the scale-quality hint above already applies.
  SDL_SetTextureScaleMode(texture_, (mode == UpscaleMode::Nearest)
                                        ? SDL_ScaleModeNearest
                                        : SDL_ScaleModeLinear);
#endif
  tex_w_ = w;
  tex_h_ = h;
  tex_mode_ = mode;
  return true;
}

void SdlHost::update_window_title(const HostStatus& status) {
  view_status_ = status;
  if (!window_) {
    return;
  }
  std::ostringstream oss;
#ifdef _WIN32
  oss << "SAN4 Native Windows — ";
#else
  oss << "SAN4 Native Linux — ";
#endif
  oss << PixelUpscaler::mode_name(status.mode)
      << " | " << status.speed_multiplier << "x";
  if (status.hd_loaded > 0u) {
    oss << " | HD:" << status.hd_loaded;
  }
  if (status.hd_dumping) {
    oss << " | DUMP";
  }
  SDL_SetWindowTitle(window_, oss.str().c_str());
}

void SdlHost::update_window_title_inline() {
  if (!emu_thread_.running()) {
    update_window_title(session_.status());
  }
}

SdlHost::Viewport SdlHost::fit_aspect_4_3(int outer_w, int outer_h) {
  const std::int64_t ow = std::max(1, outer_w);
  const std::int64_t oh = std::max(1, outer_h);
  std::int64_t vw = ow;
  std::int64_t vh = (ow * 3) / 4;
  if (vh > oh) {
    vh = oh;
    vw = (oh * 4) / 3;
  }
  vw = std::max<std::int64_t>(1, vw);
  vh = std::max<std::int64_t>(1, vh);
  return Viewport{
      .x = static_cast<int>((ow - vw) / 2),
      .y = static_cast<int>((oh - vh) / 2),
      .w = static_cast<int>(vw),
      .h = static_cast<int>(vh),
  };
}

SdlHost::Viewport SdlHost::clamp_initial_window_size(int win_w, int win_h,
                                                     int usable_w,
                                                     int usable_h) {
  if (usable_w <= 0 || usable_h <= 0) {
    return Viewport{.x = 0, .y = 0, .w = win_w, .h = win_h};
  }
  const int max_w = std::max(640, usable_w - 32);
  const int max_h = std::max(480, usable_h - 64);
  if (win_w <= max_w && win_h <= max_h) {
    return Viewport{.x = 0, .y = 0, .w = win_w, .h = win_h};
  }
  const Viewport fitted = fit_aspect_4_3(max_w, max_h);
  return Viewport{
      .x = 0,
      .y = 0,
      .w = std::max(640, fitted.w),
      .h = std::max(480, fitted.h),
  };
}

void SdlHost::map_window_to_dos(int wx, int wy, int ww, int wh, int rw, int rh,
                                int active_w, int active_h, int& out_dx,
                                int& out_dy) {
  const int aw = std::max(1, active_w);
  const int ah = std::max(1, active_h);
  if (ww <= 0 || wh <= 0 || rw <= 0 || rh <= 0) {
    const Viewport vp = fit_aspect_4_3(
        ww > 0 ? ww : VesaPlanarVideo::kWidth,
        wh > 0 ? wh : VesaPlanarVideo::kHeight);
    const std::int64_t rel_x = static_cast<std::int64_t>(wx) - vp.x;
    const std::int64_t rel_y = static_cast<std::int64_t>(wy) - vp.y;
    out_dx = static_cast<int>(
        std::clamp<std::int64_t>((rel_x * aw) / vp.w, 0, aw - 1));
    out_dy = static_cast<int>(
        std::clamp<std::int64_t>((rel_y * ah) / vp.h, 0, ah - 1));
    return;
  }

  const Viewport rvp = fit_aspect_4_3(rw, rh);
  const double rx =
      ((static_cast<double>(wx) + 0.5) * static_cast<double>(rw)) /
      static_cast<double>(ww);
  const double ry =
      ((static_cast<double>(wy) + 0.5) * static_cast<double>(rh)) /
      static_cast<double>(wh);
  const double dx_f =
      ((rx - static_cast<double>(rvp.x)) * static_cast<double>(aw)) /
      static_cast<double>(rvp.w);
  const double dy_f =
      ((ry - static_cast<double>(rvp.y)) * static_cast<double>(ah)) /
      static_cast<double>(rvp.h);
  out_dx = std::clamp(static_cast<int>(std::floor(dx_f)), 0, aw - 1);
  out_dy = std::clamp(static_cast<int>(std::floor(dy_f)), 0, ah - 1);
}

void SdlHost::map_dos_to_window(int dx, int dy, int ww, int wh, int rw, int rh,
                                int active_w, int active_h, int& out_wx,
                                int& out_wy) {
  const int aw = std::max(1, active_w);
  const int ah = std::max(1, active_h);
  if (ww <= 0 || wh <= 0 || rw <= 0 || rh <= 0) {
    const Viewport vp = fit_aspect_4_3(
        ww > 0 ? ww : VesaPlanarVideo::kWidth,
        wh > 0 ? wh : VesaPlanarVideo::kHeight);
    out_wx = static_cast<int>(
        vp.x + ((static_cast<std::int64_t>(dx) * vp.w + vp.w / 2) / aw));
    out_wy = static_cast<int>(
        vp.y + ((static_cast<std::int64_t>(dy) * vp.h + vp.h / 2) / ah));
    return;
  }

  const Viewport rvp = fit_aspect_4_3(rw, rh);
  const double rx =
      static_cast<double>(rvp.x) +
      ((static_cast<double>(dx) + 0.5) * static_cast<double>(rvp.w)) /
          static_cast<double>(aw);
  const double ry =
      static_cast<double>(rvp.y) +
      ((static_cast<double>(dy) + 0.5) * static_cast<double>(rvp.h)) /
          static_cast<double>(ah);
  const double wx_f =
      (rx * static_cast<double>(ww)) / static_cast<double>(rw);
  const double wy_f =
      (ry * static_cast<double>(wh)) / static_cast<double>(rh);
  out_wx = std::clamp(static_cast<int>(std::floor(wx_f)), 0, ww - 1);
  out_wy = std::clamp(static_cast<int>(std::floor(wy_f)), 0, wh - 1);
}

SdlHost::Viewport SdlHost::compute_window_viewport() const {
  int ww = VesaPlanarVideo::kWidth;
  int wh = VesaPlanarVideo::kHeight;
  if (window_) {
    SDL_GetWindowSize(window_, &ww, &wh);
  }
  return fit_aspect_4_3(ww, wh);
}

SdlHost::Viewport SdlHost::compute_renderer_viewport() const {
  int rw = VesaPlanarVideo::kWidth;
  int rh = VesaPlanarVideo::kHeight;
  if (renderer_) {
    SDL_GetRendererOutputSize(renderer_, &rw, &rh);
  }
  return fit_aspect_4_3(rw, rh);
}

void SdlHost::window_to_dos_coords(int wx, int wy, int& out_dx,
                                   int& out_dy) const {
  int ww = 0;
  int wh = 0;
  int rw = 0;
  int rh = 0;
  if (window_) {
    SDL_GetWindowSize(window_, &ww, &wh);
  }
  if (renderer_) {
    SDL_GetRendererOutputSize(renderer_, &rw, &rh);
  }
  map_window_to_dos(wx, wy, ww, wh, rw, rh, view_status_.active_w,
                    view_status_.active_h, out_dx, out_dy);
}

void SdlHost::dos_to_window_coords(int dx, int dy, int active_w, int active_h,
                                   int& out_wx, int& out_wy) const {
  int ww = 0;
  int wh = 0;
  int rw = 0;
  int rh = 0;
  if (window_) {
    SDL_GetWindowSize(window_, &ww, &wh);
  }
  if (renderer_) {
    SDL_GetRendererOutputSize(renderer_, &rw, &rh);
  }
  map_dos_to_window(dx, dy, ww, wh, rw, rh, active_w, active_h, out_wx, out_wy);
}

void SdlHost::warp_host_mouse(int dos_x, int dos_y, int active_w,
                              int active_h) {
  if (!window_ || !opts_.mouse_warp || !opts_.headless_script.empty()) {
    return;
  }
  int wx = 0;
  int wy = 0;
  dos_to_window_coords(dos_x, dos_y, active_w, active_h, wx, wy);
  SDL_WarpMouseInWindow(window_, wx, wy);
}

void SdlHost::send(HostCmd cmd) {
  if (emu_thread_.running()) {
    cmd_queue_.push(std::move(cmd));
  } else {
    session_.apply(cmd);
  }
}

void SdlHost::drain_host_events() {
  event_queue_.drain(host_events_);
  bool cursor_changed = false;
  for (const HostEvent& ev : host_events_) {
    if (ev.type == HostEvent::Type::Warp) {
      warp_host_mouse(ev.x, ev.y, ev.active_w, ev.active_h);
    } else {
      cached_cursor_shape_ = ev.shape;
      cached_cursor_visible_ = ev.visible;
      cursor_changed = true;
    }
  }
  // Only the final shape matters; rebuilding creates an SDL cursor.
  if (cursor_changed) {
    rebuild_hardware_cursor();
  }
}

void SdlHost::rebuild_hardware_cursor() {
  if (!window_) {
    return;
  }
  const Viewport vp = compute_window_viewport();
  const int scale = std::clamp(
      (vp.h + VesaPlanarVideo::kHeight / 2) / VesaPlanarVideo::kHeight, 1, 4);
  const int cw = 16 * scale;
  const int ch = 16 * scale;

  SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(
      0, cw, ch, 32, SDL_PIXELFORMAT_ARGB8888);
  if (surf) {
    SDL_LockSurface(surf);
    for (int y = 0; y < ch; ++y) {
      auto* dst_row = reinterpret_cast<std::uint32_t*>(
          static_cast<std::uint8_t*>(surf->pixels) + y * surf->pitch);
      const int sy = y / scale;
      for (int x = 0; x < cw; ++x) {
        const int sx = x / scale;
        dst_row[x] = cached_cursor_shape_.argb[sy * 16 + sx];
      }
    }
    SDL_UnlockSurface(surf);

    SDL_Cursor* next_cursor = SDL_CreateColorCursor(
        surf, std::clamp(cached_cursor_shape_.hot_x * scale, 0, cw - 1),
        std::clamp(cached_cursor_shape_.hot_y * scale, 0, ch - 1));
    SDL_FreeSurface(surf);

    if (next_cursor) {
      SDL_SetCursor(next_cursor);
      if (color_cursor_) {
        SDL_FreeCursor(color_cursor_);
      }
      color_cursor_ = next_cursor;
      cached_cursor_scale_ = scale;
    }
  }
  SDL_ShowCursor(cached_cursor_visible_ ? SDL_ENABLE : SDL_DISABLE);
}

void SdlHost::sync_modifier_flags() {
  const SDL_Keymod mod = SDL_GetModState();
  std::uint8_t flags = 0;
  if (mod & KMOD_RSHIFT) flags |= 0x01u;
  if (mod & KMOD_LSHIFT) flags |= 0x02u;
  if (mod & KMOD_CTRL) flags |= 0x04u;
  if (mod & KMOD_ALT) flags |= 0x08u;
  send(HostCmd::set_shift_flags(flags));
}

void SdlHost::handle_keydown(const SDL_KeyboardEvent& ev) {
  const SDL_Keycode sym = ev.keysym.sym;
  const Uint16 mod = ev.keysym.mod;

  // Engine QoL hotkeys (do not leak into guest DOS keyboard queue)
  if (sym == SDLK_TAB) {
    if (!ev.repeat) {
      send(HostCmd::set_turbo(true));
      update_window_title_inline();
    }
    return;
  }
  if (sym == SDLK_F4 && !ev.repeat) {
    send(HostCmd::simple(HostCmdType::CycleSpeed));
    update_window_title_inline();
    return;
  }
  if (sym == SDLK_F5 && !ev.repeat) {
    send(HostCmd::simple(HostCmdType::CycleScale));
    update_window_title_inline();
    return;
  }
  if (sym == SDLK_F8 && !ev.repeat) {
    if (opts_.hd_pack_dir.empty()) {
      opts_.hd_pack_dir = "hd_pack";
      send(HostCmd::with_path(HostCmdType::SetPackDir, opts_.hd_pack_dir));
      // An HD pack is now configured, which changes the automatic scale.
      apply_auto_scale();
    } else {
      send(HostCmd::simple(HostCmdType::ReloadPack));
    }
    send(HostCmd::simple(HostCmdType::MarkAllDirty));
    update_window_title_inline();
    return;
  }
  if (sym == SDLK_F9 && !ev.repeat) {
    const std::filesystem::path default_dump =
        opts_.dump_sprites_dir.empty() ? std::filesystem::path("dumped_sprites")
                                       : opts_.dump_sprites_dir;
    send(HostCmd::with_path(HostCmdType::ToggleDump, default_dump));
    update_window_title_inline();
    return;
  }
  if ((sym == SDLK_F11 || (sym == SDLK_RETURN && (mod & KMOD_ALT))) &&
      !ev.repeat) {
    fullscreen_ = !fullscreen_;
    SDL_SetWindowFullscreen(window_,
                            fullscreen_ ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    rebuild_hardware_cursor();
    present_.request_redraw();
    return;
  }

  // WASD map panning -> DOS arrow keys when Ctrl/Alt are not held
  if ((mod & (KMOD_CTRL | KMOD_ALT)) == 0) {
    if (sym == SDLK_w) {
      send(HostCmd::push_key(0x00u, 0x48u));
      return;
    }
    if (sym == SDLK_a) {
      send(HostCmd::push_key(0x00u, 0x4Bu));
      return;
    }
    if (sym == SDLK_s) {
      send(HostCmd::push_key(0x00u, 0x50u));
      return;
    }
    if (sym == SDLK_d) {
      send(HostCmd::push_key(0x00u, 0x4Du));
      return;
    }
  }

  switch (sym) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      send(HostCmd::push_key(0x0Du, 0x1Cu));
      return;
    case SDLK_ESCAPE:
      send(HostCmd::push_key(0x1Bu, 0x01u));
      return;
    case SDLK_SPACE:
      send(HostCmd::push_key(0x20u, 0x39u));
      return;
    case SDLK_BACKSPACE:
      send(HostCmd::push_key(0x08u, 0x0Eu));
      return;
    case SDLK_UP:
    case SDLK_KP_8:
      send(HostCmd::push_key(0x00u, 0x48u));
      return;
    case SDLK_DOWN:
    case SDLK_KP_2:
      send(HostCmd::push_key(0x00u, 0x50u));
      return;
    case SDLK_LEFT:
    case SDLK_KP_4:
      send(HostCmd::push_key(0x00u, 0x4Bu));
      return;
    case SDLK_RIGHT:
    case SDLK_KP_6:
      send(HostCmd::push_key(0x00u, 0x4Du));
      return;
    case SDLK_PAGEUP:
    case SDLK_KP_9:
      send(HostCmd::push_key(0x00u, 0x49u));
      return;
    case SDLK_PAGEDOWN:
    case SDLK_KP_3:
      send(HostCmd::push_key(0x00u, 0x51u));
      return;
    case SDLK_HOME:
    case SDLK_KP_7:
      send(HostCmd::push_key(0x00u, 0x47u));
      return;
    case SDLK_END:
    case SDLK_KP_1:
      send(HostCmd::push_key(0x00u, 0x4Fu));
      return;
    case SDLK_F1:
      send(HostCmd::push_key(0x00u, 0x3Bu));
      return;
    case SDLK_F2:
      send(HostCmd::push_key(0x00u, 0x3Cu));
      return;
    case SDLK_F3:
      send(HostCmd::push_key(0x00u, 0x3Du));
      return;
    case SDLK_F6:
      send(HostCmd::push_key(0x00u, 0x40u));
      return;
    case SDLK_F7:
      send(HostCmd::push_key(0x00u, 0x41u));
      return;
    case SDLK_F10:
      send(HostCmd::push_key(0x00u, 0x44u));
      return;
    default:
      break;
  }

  // Printable ASCII digits & letters (0..9, Y/N, etc.)
  if (sym >= 0x20 && sym <= 0x7E) {
    char ch = static_cast<char>(sym);
    if ((mod & KMOD_SHIFT) && ch >= 'a' && ch <= 'z') {
      ch = static_cast<char>(ch - 'a' + 'A');
    }
    send(HostCmd::push_key(static_cast<std::uint8_t>(ch),
                           ascii_to_xt_scancode(ch)));
  }
}

void SdlHost::handle_keyup(const SDL_KeyboardEvent& ev) {
  if (ev.keysym.sym == SDLK_TAB) {
    send(HostCmd::set_turbo(false));
    update_window_title_inline();
  }
}

void SdlHost::handle_render_reset(bool device_reset) {
  int target_w = tex_w_;
  int target_h = tex_h_;
  UpscaleMode target_mode = tex_mode_;
  if (target_w <= 0 || target_h <= 0) {
    if (emu_thread_.running()) {
      if (const auto* cur = mailbox_.current_acquired(); cur != nullptr) {
        target_w = cur->w;
        target_h = cur->h;
        target_mode = cur->mode;
      } else {
        target_w = 0;
        target_h = 0;
      }
    } else {
      target_w = upscaler_.output_width();
      target_h = upscaler_.output_height();
      target_mode = upscaler_.mode();
    }
  }
  if (target_w > 0 && target_h > 0) {
    if (!recreate_frame_texture(target_w, target_h, target_mode) &&
        device_reset) {
      if (recreate_renderer_after_reset()) {
        recreate_frame_texture(target_w, target_h, target_mode);
      }
    }
  } else if (device_reset) {
    recreate_renderer_after_reset();
  }
  present_.texture_replaced();
}

void SdlHost::process_sdl_events(bool& running) {
  if (!emu_thread_.running()) {
    view_status_ = session_.status();
  }
  const bool headless = !opts_.headless_script.empty();
  SDL_Event ev;
  while (SDL_PollEvent(&ev)) {
    if (ev.type == SDL_RENDER_TARGETS_RESET ||
        ev.type == SDL_RENDER_DEVICE_RESET) {
      handle_render_reset(ev.type == SDL_RENDER_DEVICE_RESET);
      continue;
    }
    if (headless) {
      // Scripted runs take input only from the script: host input and
      // SDL_QUIT would make replays depend on the desktop.
      if (ev.type == SDL_WINDOWEVENT) {
        present_.request_redraw();
      }
      continue;
    }
    switch (ev.type) {
      case SDL_QUIT:
        running = false;
        break;

      case SDL_WINDOWEVENT:
        present_.request_redraw();
        if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
            ev.window.event == SDL_WINDOWEVENT_RESIZED ||
            ev.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED) {
          const Viewport vp = compute_window_viewport();
          const int next_scale = std::clamp(
              (vp.h + VesaPlanarVideo::kHeight / 2) / VesaPlanarVideo::kHeight,
              1, 4);
          if (next_scale != cached_cursor_scale_) {
            rebuild_hardware_cursor();
          }
          apply_auto_scale();
        } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
          send(HostCmd::set_turbo(false));
          send(HostCmd::set_shift_flags(0u));
          for (int btn = 0; btn < 3; ++btn) {
            send(HostCmd::mouse_button(btn, false));
          }
          update_window_title_inline();
        }
        break;

      case SDL_AUDIODEVICEREMOVED:
        if (ev.adevice.iscapture == 0 && audio_dev_ != 0 &&
            ev.adevice.which == audio_dev_) {
          SDL_CloseAudioDevice(audio_dev_);
          audio_dev_ = 0;
          send(HostCmd::set_drain_audio(true));
        }
        break;

      case SDL_AUDIODEVICEADDED:
        if (ev.adevice.iscapture == 0 && audio_dev_ == 0 &&
            opts_.headless_script.empty()) {
          open_audio_device(false);
          if (audio_dev_ != 0) {
            send(HostCmd::set_drain_audio(false));
          }
        }
        break;

      case SDL_MOUSEMOTION: {
        int dx = 0;
        int dy = 0;
        window_to_dos_coords(ev.motion.x, ev.motion.y, dx, dy);
        // Middle-drag panning compares against the guest-side position, so
        // it is evaluated where the command is applied.
        send(HostCmd::mouse_motion(dx, dy));
        break;
      }

      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP: {
        const bool pressed = (ev.type == SDL_MOUSEBUTTONDOWN);
        int dx = 0;
        int dy = 0;
        window_to_dos_coords(ev.button.x, ev.button.y, dx, dy);
        send(HostCmd::mouse_pos(dx, dy));

        if (ev.button.button == SDL_BUTTON_LEFT) {
          send(HostCmd::mouse_button(0, pressed));
        } else if (ev.button.button == SDL_BUTTON_RIGHT) {
          send(HostCmd::mouse_button(1, pressed));
        } else if (ev.button.button == SDL_BUTTON_MIDDLE) {
          send(HostCmd::mouse_button(2, pressed));
        }
        break;
      }

      case SDL_MOUSEWHEEL: {
        int wy = std::clamp<int>(ev.wheel.y, -10, 10);
        if (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
          wy = -wy;
        }
        const bool shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
        const int steps = std::min(std::abs(wy), 6);
        for (int i = 0; i < steps; ++i) {
          if (wy > 0) {
            send(HostCmd::push_key(0x00u, shift ? 0x49u : 0x48u));
          } else if (wy < 0) {
            send(HostCmd::push_key(0x00u, shift ? 0x51u : 0x50u));
          }
        }
        break;
      }

      case SDL_KEYDOWN:
        sync_modifier_flags();
        handle_keydown(ev.key);
        break;

      case SDL_KEYUP:
        sync_modifier_flags();
        handle_keyup(ev.key);
        break;

      default:
        break;
    }
  }
}

std::uint64_t SdlHost::upload_rects(const std::uint32_t* argb,
                                    std::span<const UpscaleDirtyRect> rects) {
  if (texture_ == nullptr || rects.empty() || tex_w_ <= 0 || tex_h_ <= 0) {
    return 0;
  }
  const int pitch = tex_w_ * static_cast<int>(sizeof(std::uint32_t));
  std::uint64_t covered = 0;
  for (const UpscaleDirtyRect& r : rects) {
    covered += static_cast<std::uint64_t>(r.w) * static_cast<std::uint64_t>(r.h);
  }
  const std::uint64_t frame_px =
      static_cast<std::uint64_t>(tex_w_) * static_cast<std::uint64_t>(tex_h_);
  // One full-frame upload beats many small ones once most pixels changed.
  if (covered * 10u > frame_px * 6u) {
    SDL_UpdateTexture(texture_, nullptr, argb, pitch);
    return frame_px;
  }
  for (const UpscaleDirtyRect& r : rects) {
    const SDL_Rect rect{r.x, r.y, r.w, r.h};
    SDL_UpdateTexture(texture_, &rect,
                      argb + static_cast<std::size_t>(r.y) *
                                 static_cast<std::size_t>(tex_w_) +
                          static_cast<std::size_t>(r.x),
                      pitch);
  }
  return covered;
}

void SdlHost::present_texture() {
  const Viewport rvp = compute_renderer_viewport();
  SDL_Rect dst{rvp.x, rvp.y, rvp.w, rvp.h};
  SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
  SDL_RenderClear(renderer_);
  if (texture_) {
    SDL_RenderCopy(renderer_, texture_, nullptr, &dst);
  }
  SDL_RenderPresent(renderer_);
}

void SdlHost::render_and_present(bool& force_full_upscale) {
  const bool profiling = profiler_.enabled();
  session_.produce_frame(force_full_upscale,
                         profiling ? &profile_row_ : nullptr);

  FrameProfiler::Clock::time_point t_upload0;
  FrameProfiler::Clock::time_point t_present0;
  if (profiling) {
    t_upload0 = FrameProfiler::Clock::now();
  }
  if (PresentTracker::needs_new_texture(
          texture_ != nullptr, tex_w_, tex_h_, tex_mode_,
          upscaler_.output_width(), upscaler_.output_height(),
          upscaler_.mode())) {
    if (!recreate_frame_texture(upscaler_.output_width(),
                                upscaler_.output_height(), upscaler_.mode()) &&
        upscaler_.mode() != UpscaleMode::Nearest) {
      upscaler_.set_mode(UpscaleMode::Nearest);
      session_.produce_frame(force_full_upscale, nullptr);
      recreate_frame_texture(upscaler_.output_width(),
                             upscaler_.output_height(), upscaler_.mode());
      update_window_title_inline();
    }
    present_.texture_replaced();
  }
  // A new or reset texture holds nothing yet: upload the whole frame.
  const UpscaleDirtyRect whole{
      .has_changes = true, .x = 0, .y = 0, .w = tex_w_, .h = tex_h_};
  const std::uint64_t uploaded_px = upload_rects(
      upscaler_.output_argb().data(),
      present_.uploaded_gen() == 0u
          ? std::span<const UpscaleDirtyRect>(&whole, 1u)
          : session_.frame_rects());
  present_.finish_upload(presented_frames_ + 1u, uploaded_px);

  if (profiling) {
    t_present0 = FrameProfiler::Clock::now();
  }
  present_texture();
  present_.presented();

  if (profiling) {
    const auto t_end = FrameProfiler::Clock::now();
    profile_row_.frame = presented_frames_;
    profile_row_.upload_ms = FrameProfiler::ms_between(t_upload0, t_present0);
    profile_row_.present_ms = FrameProfiler::ms_between(t_present0, t_end);
    profile_row_.dirty_px = uploaded_px;
    profiler_.write(profile_row_);
    profile_row_ = FrameProfileRow{};
  }
  ++presented_frames_;
}

bool SdlHost::present_slot(const FrameMailbox::Slot& slot) {
  const bool profiling = profiler_.enabled();
  FrameProfiler::Clock::time_point t_upload0;
  FrameProfiler::Clock::time_point t_present0;
  if (profiling) {
    t_upload0 = FrameProfiler::Clock::now();
  }
  if (PresentTracker::needs_new_texture(texture_ != nullptr, tex_w_, tex_h_,
                                        tex_mode_, slot.w, slot.h,
                                        slot.mode)) {
    if (!recreate_frame_texture(slot.w, slot.h, slot.mode)) {
      if (recreate_renderer_after_reset() &&
          recreate_frame_texture(slot.w, slot.h, slot.mode)) {
        present_.texture_replaced();
      } else {
        // Pins Nearest once; until the emulation thread applies it, later
        // slots in this mode just retry the texture.
        if (present_.take_scale_fallback(slot.mode)) {
          send(HostCmd::set_scale(UpscaleMode::Nearest));
        }
        return false;
      }
    } else {
      present_.texture_replaced();
    }
  }
  FrameMailbox::changed_rects(slot, present_.uploaded_gen(), upload_rects_);
  const std::uint64_t uploaded_px = upload_rects(slot.argb.data(), upload_rects_);
  // Nothing changed on screen: skip the (on software GL, expensive) present.
  const bool present = present_.finish_upload(slot.gen, uploaded_px);
  if (slot.status != view_status_) {
    update_window_title(slot.status);
  }

  if (profiling) {
    t_present0 = FrameProfiler::Clock::now();
  }
  if (present) {
    present_texture();
    present_.presented();
    ++presented_frames_;
  }

  if (profiling) {
    FrameProfileRow row = slot.profile;
    row.frame = slot.gen - 1u;
    row.upload_ms = FrameProfiler::ms_between(t_upload0, t_present0);
    row.present_ms =
        present ? FrameProfiler::ms_between(t_present0,
                                            FrameProfiler::Clock::now())
                : 0.0;
    row.dirty_px = uploaded_px;
    profiler_.write(row);
  }
  return present;
}

void SdlHost::step_headless_frames(int count, bool& running, bool& force_full) {
  FrameProfileRow* const row = profiler_.enabled() ? &profile_row_ : nullptr;
  for (int i = 0; i < count && running && !machine_.exited(); ++i) {
    process_sdl_events(running);
    session_.run_guest_frame(1.0 / 60.0, 0.015, row);
    render_and_present(force_full);
    if (audio_dev_ == 0) {
      discard_buffered_audio(machine_.audio());
    }
  }
}

bool SdlHost::execute_headless_command(const std::vector<std::string>& parts,
                                       bool& running, bool& force_full) {
  const std::string& op = parts[0];
  if (op == "frames") {
    int count = 0;
    if (parts.size() != 2u ||
        !util::parse_int_strict(parts[1], 1, 1000000, count)) {
      return false;
    }
    step_headless_frames(count, running, force_full);
    return true;
  }
  if (op == "wait_idle") {
    int max_frames = 300;
    if (parts.size() > 2u ||
        (parts.size() == 2u &&
         !util::parse_int_strict(parts[1], 1, 1000000, max_frames))) {
      return false;
    }
    int idle_streak = 0;
    for (int i = 0; i < max_frames && running && !machine_.exited(); ++i) {
      step_headless_frames(1, running, force_full);
      if (machine_.is_waiting_for_input()) {
        if (++idle_streak >= 3) {
          break;
        }
      } else {
        idle_streak = 0;
      }
    }
    return true;
  }
  if (op == "wait_ms") {
    int ms = 0;
    if (parts.size() != 2u ||
        !util::parse_int_strict(parts[1], 0, 3600000, ms)) {
      return false;
    }
    const std::int64_t frames64 =
        std::max<std::int64_t>(1, (static_cast<std::int64_t>(ms) * 60 + 999) / 1000);
    step_headless_frames(static_cast<int>(frames64), running, force_full);
    return true;
  }
  if (op == "move") {
    int mx = 0;
    int my = 0;
    if (parts.size() != 3u ||
        !util::parse_int_strict(parts[1], -10000, 10000, mx) ||
        !util::parse_int_strict(parts[2], -10000, 10000, my)) {
      return false;
    }
    machine_.mouse().set_position(mx, my);
    step_headless_frames(1, running, force_full);
    return true;
  }
  if (op == "click" || op == "rclick") {
    int mx = 0;
    int my = 0;
    if (parts.size() != 3u ||
        !util::parse_int_strict(parts[1], -10000, 10000, mx) ||
        !util::parse_int_strict(parts[2], -10000, 10000, my)) {
      return false;
    }
    const int btn = (op == "rclick") ? 1 : 0;
    machine_.mouse().set_position(mx, my);
    machine_.mouse().set_button(btn, true);
    step_headless_frames(3, running, force_full);
    machine_.mouse().set_button(btn, false);
    step_headless_frames(3, running, force_full);
    return true;
  }
  if (op == "tap") {
    int mx = 0;
    int my = 0;
    if (parts.size() != 3u ||
        !util::parse_int_strict(parts[1], -10000, 10000, mx) ||
        !util::parse_int_strict(parts[2], -10000, 10000, my)) {
      return false;
    }
    // Sub-frame trackpad tap: press + release before next guest poll!
    machine_.mouse().set_position(mx, my);
    machine_.mouse().set_button(0, true);
    machine_.mouse().set_button(0, false);
    step_headless_frames(4, running, force_full);
    return true;
  }
  if (op == "wheel" || op == "shift_wheel") {
    int raw_wy = 0;
    if (parts.size() != 2u ||
        !util::parse_int_strict(parts[1], -1000000, 1000000, raw_wy)) {
      return false;
    }
    const int wy = std::clamp(raw_wy, -10, 10);
    const bool shift = (op == "shift_wheel");
    const int steps = std::abs(wy);
    for (int i = 0; i < steps; ++i) {
      if (wy > 0) {
        machine_.dos().push_key(0x00u, shift ? 0x49u : 0x48u);
      } else if (wy < 0) {
        machine_.dos().push_key(0x00u, shift ? 0x51u : 0x50u);
      }
    }
    step_headless_frames(4, running, force_full);
    return true;
  }
  if (op == "key") {
    if (parts.size() != 2u || parts[1].empty()) {
      return false;
    }
    const std::string& k = parts[1];
    if (k == "enter") {
      machine_.dos().push_key(0x0Du, 0x1Cu);
    } else if (k == "esc") {
      machine_.dos().push_key(0x1Bu, 0x01u);
    } else if (k == "space") {
      machine_.dos().push_key(0x20u, 0x39u);
    } else if (k == "up") {
      machine_.dos().push_key(0x00u, 0x48u);
    } else if (k == "down") {
      machine_.dos().push_key(0x00u, 0x50u);
    } else if (k == "left") {
      machine_.dos().push_key(0x00u, 0x4Bu);
    } else if (k == "right") {
      machine_.dos().push_key(0x00u, 0x4Du);
    } else if (k == "pgup") {
      machine_.dos().push_key(0x00u, 0x49u);
    } else if (k == "pgdn") {
      machine_.dos().push_key(0x00u, 0x51u);
    } else if (k.size() == 1u) {
      machine_.dos().push_key(static_cast<std::uint8_t>(k[0]),
                              ascii_to_xt_scancode(k[0]));
    } else {
      return false;
    }
    step_headless_frames(3, running, force_full);
    return true;
  }
  if (op == "speed") {
    int mult = 1;
    if (parts.size() != 2u ||
        !util::parse_int_strict(parts[1], 1, 4, mult) ||
        (mult != 1 && mult != 2 && mult != 4)) {
      return false;
    }
    machine_.set_base_speed_multiplier(mult);
    update_window_title_inline();
    return true;
  }
  if (op == "scale") {
    UpscaleMode m = upscaler_.mode();
    if (parts.size() != 2u || !PixelUpscaler::parse_mode(parts[1], m)) {
      return false;
    }
    upscaler_.set_mode(m);
    force_full = true;
    step_headless_frames(1, running, force_full);
    update_window_title_inline();
    return true;
  }
  if (op == "reload_hd") {
    if (parts.size() >= 2u) {
      std::string path = parts[1];
      for (std::size_t i = 2; i < parts.size(); ++i) {
        path += ":" + parts[i];
      }
      opts_.hd_pack_dir = util::utf8_to_path(path);
      machine_.hd_pack().set_pack_dir(opts_.hd_pack_dir);
    } else {
      machine_.hd_pack().reload_pack();
    }
    machine_.video().mark_all_dirty();
    force_full = true;
    step_headless_frames(1, running, force_full);
    return true;
  }
  if (op == "screenshot") {
    if (parts.size() < 2u) {
      return false;
    }
    // Reconstruct path if it contained colons (e.g. C:\path on Windows)
    std::string path = parts[1];
    for (std::size_t i = 2; i < parts.size(); ++i) {
      path += ":" + parts[i];
    }
    HdPackManager::save_argb_png(util::utf8_to_path(path),
                                 upscaler_.output_width(),
                                 upscaler_.output_height(),
                                 upscaler_.output_argb().data());
    return true;
  }
  if (op == "reset_audio_peak") {
    if (parts.size() != 1u) {
      return false;
    }
    machine_.audio().reset_peak_abs_sample();
    return true;
  }
  if (op == "stats") {
    if (parts.size() != 1u) {
      return false;
    }
    std::cout << "[san4-stats] frames=" << presented_frames_
              << " mode=0x" << std::hex << machine_.video().current_mode()
              << std::dec << " out=" << upscaler_.output_width() << "x"
              << upscaler_.output_height()
              << " scale=" << PixelUpscaler::mode_name(upscaler_.mode())
              << " speed=" << machine_.effective_speed_multiplier() << "x"
              << " audio_frames=" << machine_.audio().total_rendered_frames()
              << " audio_peak=" << machine_.audio().peak_abs_sample()
              << " hd_loaded=" << machine_.hd_pack().loaded_texture_count()
              << " hd_active=" << machine_.hd_pack().active_overlay_count()
              << " hd_cache_kb=" << (machine_.hd_pack().cache_resident_bytes() / 1024u)
              << " hd_hits=" << machine_.hd_pack().cache_hits()
              << " hd_misses=" << machine_.hd_pack().cache_misses()
              << " hd_reload_ms=" << machine_.hd_pack().last_reload_ms()
              << " grp_cs=0x" << std::hex << machine_.dos().grp600c_cs()
              << " cs:ip=" << machine_.emu()->x86.R_CS << ":"
              << machine_.emu()->x86.R_IP << std::dec
              << " mouse=" << machine_.mouse().x() << ","
              << machine_.mouse().y() << "\n";
    return true;
  }
  if (op == "quit") {
    if (parts.size() != 1u) {
      return false;
    }
    running = false;
    return true;
  }
  return false;
}

int SdlHost::run_headless_script() {
  bool running = true;
  bool force_full = true;

  for (const std::string& raw_cmd : split_tokens(opts_.headless_script, ';')) {
    if (!running || machine_.exited()) {
      break;
    }
    const auto parts = split_tokens(raw_cmd, ':');
    if (parts.empty()) {
      continue;
    }
    if (!execute_headless_command(parts, running, force_full)) {
      std::cerr << "[san4] Invalid headless command: " << raw_cmd << "\n";
      return 2;
    }
  }
  return 0;
}

int SdlHost::run() {
  if (!opts_.hd_pack_dir.empty() &&
      machine_.hd_pack().loaded_texture_count() > 0u) {
    std::cout << "[san4-hd] Indexed "
              << machine_.hd_pack().loaded_texture_count()
              << " HD assets in " << machine_.hd_pack().last_reload_ms()
              << " ms\n";
  }
  if (!opts_.profile_frames_path.empty()) {
    if (!profiler_.open(opts_.profile_frames_path)) {
      std::cerr << "[san4] Cannot write --profile-frames file: "
                << util::path_to_utf8(opts_.profile_frames_path) << "\n";
      return 1;
    }
    machine_.set_grp_census_enabled(true);
  }
  if (!init_sdl()) {
    return 1;
  }
  if (!machine_.boot("SAN486.COM")) {
    const std::string src_desc =
        !opts_.game_dir.empty()
            ? util::path_to_utf8(opts_.game_dir)
            : (opts_.embedded_vfs ? std::string("<embedded-vfs>")
                                  : std::string("<empty>"));
    std::cerr << "[san4] Failed to boot SAN486.COM from " << src_desc << "\n";
    return 1;
  }

  int rc = 0;
  try {
    rc = opts_.headless_script.empty() ? run_interactive()
                                       : run_headless_script();
  } catch (...) {
    // Keep the frames profiled up to the failure.
    report_profile();
    throw;
  }
  report_profile();
  return rc;
}

void SdlHost::report_profile() {
  if (!profiler_.enabled()) {
    return;
  }
  profiler_.flush();
  if (const GrpCensus* census = machine_.grp_census()) {
    census->print(std::cout);
  }
}

int SdlHost::run_interactive() {
  using Clock = std::chrono::steady_clock;
  // Without VSync, presents are spaced at least this far apart (~66 Hz cap),
  // which keeps every 60 Hz emulator frame while bounding turbo output.
  constexpr auto kMinPresentInterval = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double>(0.9 / 60.0));
  // Longest wait for a new frame before input is polled again.
  constexpr auto kEventPollInterval = std::chrono::milliseconds(8);

  emu_thread_.start(session_, mailbox_, cmd_queue_,
                    EmuSession::LoopOptions{
                        .guest_budget_sec = 0.012,
                        .drain_audio = (audio_dev_ == 0),
                        .profiling = profiler_.enabled(),
                    });
  bool running = true;
  while (running) {
    process_sdl_events(running);
    if (!running || emu_thread_.finished()) {
      break;
    }
    drain_host_events();

    const auto present_start = Clock::now();
    bool presented = false;
    if (const FrameMailbox::Slot* slot = mailbox_.acquire()) {
      presented = present_slot(*slot);
    } else if (present_.needs_redraw()) {
      if (present_.uploaded_gen() == 0u || texture_ == nullptr) {
        if (const FrameMailbox::Slot* cur = mailbox_.current_acquired()) {
          presented = present_slot(*cur);
        }
      } else if (present_.uploaded_gen() > 0u && texture_ != nullptr) {
        present_texture();
        present_.presented();
        presented = true;
      }
    }
    if (presented && !has_vsync_) {
      std::this_thread::sleep_until(present_start + kMinPresentInterval);
    }
    mailbox_.wait_for_fresh_until(Clock::now() + kEventPollInterval);
  }

  // Joins before any SDL teardown; rethrows a failure of the emulation loop.
  emu_thread_.stop();
  return 0;
}

}  // namespace san4
