#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "emu_session.h"
#include "frame_mailbox.h"
#include "frame_profiler.h"
#include "gpu/gl_renderer.h"
#include "gpu/gpu_frame_mailbox.h"
#include "host_channel.h"
#include "machine.h"
#include "present_tracker.h"
#include "render_policy.h"
#include "upscaler.h"
#include "util/thread_pool.h"

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Cursor;
struct SDL_KeyboardEvent;
using SDL_AudioDeviceID = std::uint32_t;

namespace san4 {

struct HostOptions {
  std::filesystem::path game_dir;
  std::filesystem::path save_dir;
  std::filesystem::path hd_pack_dir;
  std::filesystem::path dump_sprites_dir;
  std::shared_ptr<const EmbeddedGameVfs> embedded_vfs;
  UpscaleMode upscale_mode = UpscaleMode::Sharp2x;
  // `upscale_mode` came from `--scale`; disables the automatic HD-pack scale.
  bool explicit_scale = false;
  // Interactive present-profile override (`--renderer`).
  RendererPreference renderer = RendererPreference::Auto;
  int window_scale = 2;
  bool fullscreen = false;
  bool skip_intro = false;
  bool mouse_warp = true;
  int speed_multiplier = 1;
  std::string headless_script;
  // When non-empty, write per-frame timings here (TSV) and print a GRP600C
  // call census at exit.
  std::filesystem::path profile_frames_path;
  // Shared worker-pool size for upscaling, compositing and background CG
  // re-upscales (0 = clamp(hardware threads, 4, 16)).
  unsigned threads = 0;
  // Headless runs budget guest bursts by instruction count and derive DOS
  // time from guest ticks, so identical scripts give identical screenshots.
  bool deterministic = false;
};

class SdlHost {
 public:
  explicit SdlHost(const HostOptions& opts);
  ~SdlHost();

  SdlHost(const SdlHost&) = delete;
  SdlHost& operator=(const SdlHost&) = delete;

  int run();

 private:
  struct Viewport {
    int x = 0;
    int y = 0;
    int w = VesaPlanarVideo::kWidth;
    int h = VesaPlanarVideo::kHeight;
  };

  bool init_sdl();
  // Probes an OpenGL 3.3 Core hardware GPU context on a hidden window first;
  // destroys the probe window cleanly and returns false on software rasterizers
  // (under `--renderer=auto`) or unsupported drivers so `init_sdl()` falls back
  // to `SDL_Renderer`.
  bool try_init_opengl_backend(const char* title, int win_w, int win_h,
                               std::uint32_t base_win_flags, bool verbose_log);
  // Interactive renderer per the present policy (opengles2 first, profile,
  // VSync); logs one `[san4-render]` line when `verbose_log` is true.
  bool create_interactive_renderer(bool verbose_log = true);
  bool recreate_renderer_after_reset();
  bool get_drawable_size(int& out_w, int& out_h) const;
  void open_audio_device(bool verbose_log);
  // Re-applies the automatic output scale for the current drawable size.
  void apply_auto_scale();
  void shutdown_sdl();
  bool recreate_frame_texture(int w, int h, UpscaleMode mode);
  void update_window_title(const HostStatus& status);
  // Refreshes the title from the session while no emulation thread runs
  // (headless, start-up); afterwards titles follow published frames.
  void update_window_title_inline();
  void rebuild_hardware_cursor();

  static Viewport fit_aspect_4_3(int outer_w, int outer_h);
  static Viewport clamp_initial_window_size(int win_w, int win_h, int usable_w,
                                            int usable_h);
  static void map_window_to_dos(int wx, int wy, int ww, int wh, int rw, int rh,
                                int active_w, int active_h, int& out_dx,
                                int& out_dy);
  static void map_dos_to_window(int dx, int dy, int ww, int wh, int rw, int rh,
                                int active_w, int active_h, int& out_wx,
                                int& out_wy);
  Viewport compute_window_viewport() const;
  Viewport compute_renderer_viewport() const;
  void window_to_dos_coords(int wx, int wy, int& out_dx, int& out_dy) const;
  void dos_to_window_coords(int dx, int dy, int active_w, int active_h,
                            int& out_wx, int& out_wy) const;
  void warp_host_mouse(int dos_x, int dos_y, int active_w, int active_h);

  // Hands a command to the emulation thread, or applies it immediately when
  // none runs (headless and start-up), so both paths share one handler.
  void send(HostCmd cmd);
  // Applies MouseHle warp / cursor events queued by the emulation thread.
  void drain_host_events();

  // Polls SDL events. Headless runs ignore input and SDL_QUIT (their input
  // comes only from the script) and only note redraws and render resets.
  void process_sdl_events(bool& running);
  // SDL_RENDER_TARGETS_RESET / SDL_RENDER_DEVICE_RESET: the texture's
  // contents are lost, so it is recreated and fully re-uploaded.
  void handle_render_reset(bool device_reset = false);
  void handle_keydown(const SDL_KeyboardEvent& ev);
  void handle_keyup(const SDL_KeyboardEvent& ev);
  void sync_modifier_flags();

  // Uploads `rects` of the `tex_w_ x tex_h_` frame `argb` (one full upload
  // when they cover more than 60% of it); returns the pixels uploaded.
  std::uint64_t upload_rects(const std::uint32_t* argb,
                             std::span<const UpscaleDirtyRect> rects);
  void present_texture();
  void present_gpu_texture();
  // Headless: produce, upload and present one frame on the calling thread.
  void render_and_present(bool& force_full_upscale);
  // Interactive: uploads what changed in `slot` since the last upload and
  // presents; returns whether a frame was presented.
  bool present_slot(const FrameMailbox::Slot& slot);
  bool present_gpu_slot(const GpuFrameMailbox::Slot& slot);
  void step_headless_frames(int count, bool& running, bool& force_full);
  bool execute_headless_command(const std::vector<std::string>& parts,
                                bool& running, bool& force_full);
  int run_headless_script();
  int run_interactive();
  void report_profile();

  static void sdl_audio_callback(void* userdata, std::uint8_t* stream, int len);

  HostOptions opts_;
  // Declared before every user so it is destroyed last (after emu_thread_
  // has been joined).
  std::unique_ptr<ThreadPool> pool_;
  // Emulation state: owned by emu_thread_ while it runs, otherwise by the
  // calling thread.
  X86Machine machine_;
  PixelUpscaler upscaler_;
  EmuSession session_;

  // Main <-> emulation thread channels.
  FrameMailbox mailbox_;
  GpuFrameMailbox gpu_mailbox_;
  HostCommandQueue cmd_queue_;
  HostEventQueue event_queue_;
  std::vector<HostEvent> host_events_;
  // Declared after everything its loop touches so it is joined first.
  EmuThread emu_thread_;

  std::uint32_t sdl_init_subsystems_ = 0;
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* texture_ = nullptr;
  void* gl_context_ = nullptr;
  std::unique_ptr<gpu::GlRenderer> gl_renderer_;
  RenderBackendKind render_backend_ = RenderBackendKind::SdlRenderer;
  SDL_Cursor* color_cursor_ = nullptr;
  SDL_AudioDeviceID audio_dev_ = 0;

  int tex_w_ = 0;
  int tex_h_ = 0;
  UpscaleMode tex_mode_ = UpscaleMode::Sharp2x;
  // Uploaded generation, pending redraws (expose, resize) and the scale
  // fallback for texture_.
  PresentTracker present_;
  std::vector<UpscaleDirtyRect> upload_rects_;
  bool fullscreen_ = false;
  bool has_vsync_ = false;
  RenderProfile render_profile_ = RenderProfile::Hardware;
  // Status of the last presented frame: window title and the active
  // resolution for mouse coordinate mapping.
  HostStatus view_status_{};

  DosCursorShape cached_cursor_shape_{};
  bool cached_cursor_visible_ = false;
  int cached_cursor_scale_ = 0;

  std::uint64_t presented_frames_ = 0;

  FrameProfiler profiler_;
  FrameProfileRow profile_row_{};

  friend struct SdlHostTestPeer;
};

}  // namespace san4
