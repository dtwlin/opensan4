#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "audio.h"
#include "dos/dos_kernel.h"
#include "dos/ems.h"
#include "dos/mouse.h"
#include "dos/vfs.h"
#include "grp_census.h"
#include "hd_pack.h"
#include "video.h"
#include "x86emu_clean.h"

namespace san4 {

struct MachineConfig {
  std::filesystem::path game_dir;
  std::filesystem::path save_dir;
  std::filesystem::path hd_pack_dir;
  std::filesystem::path dump_sprites_dir;
  bool skip_intro = false;
  bool mouse_warp = true;
  int speed_multiplier = 1;  // 1, 2, or 4 (Tab overrides to 16 while held)
  // Replays bit-identically for identical inputs; see set_deterministic().
  bool deterministic = false;
};

// Why the last run_frame burst stopped.
enum class GuestYieldReason : std::uint8_t {
  None = 0,  // no burst ran (program already exited)
  Budget,    // the CPU budget (wall clock, or instructions if deterministic) ran out
  Idle,      // the guest was detected polling for input or a timer
  FadeHold,  // parked in the smooth-fade ROM wait loop
  Stalled,   // the emulator retired no instructions
  Exited,    // the guest program terminated
};

constexpr const char* guest_yield_reason_name(GuestYieldReason r) {
  switch (r) {
    case GuestYieldReason::None:
      return "none";
    case GuestYieldReason::Budget:
      return "budget";
    case GuestYieldReason::Idle:
      return "idle";
    case GuestYieldReason::FadeHold:
      return "fade_hold";
    case GuestYieldReason::Stalled:
      return "stalled";
    case GuestYieldReason::Exited:
      return "exited";
  }
  return "none";
}

struct GuestFrameStats {
  std::uint64_t guest_instrs = 0;
  GuestYieldReason yield_reason = GuestYieldReason::None;
};

class X86Machine {
 public:
  static constexpr std::size_t kGuestRamBytes = 1048576u + 65536u;
  static constexpr double kPitBaseHz = 1193182.0;
  // Guest instructions per second of `max_cpu_budget_sec` in deterministic
  // mode: the measured libx86emu rate of the slice loop (~198k instructions
  // per 15 ms headless burst on the reference host).
  static constexpr double kDeterministicInstrsPerSec = 13.2e6;

  explicit X86Machine(const MachineConfig& cfg = {});
  ~X86Machine();

  X86Machine(const X86Machine&) = delete;
  X86Machine& operator=(const X86Machine&) = delete;

  // Boots a DOS program inside the configured VFS (e.g. "SAN486.COM").
  bool boot(std::string_view program = "SAN486.COM",
            std::string_view cmd_tail = "");

  // Loads raw machine-code bytes at `seg:off` for synthetic unit tests (no game
  // files required).
  void load_synthetic_code(std::uint16_t seg, std::uint16_t off,
                           std::span<const std::uint8_t> code,
                           std::uint16_t stack_seg = 0x1000u,
                           std::uint16_t stack_sp = 0xFFFEu);

  // Runs one host frame (`dt_sec`, typically ~1/60s): advances smooth fades,
  // speed-scaled gameplay timers, interleaved 291.304 Hz PIT IRQ 0 + OPL2 FM
  // synthesis, and quiescent-detected libx86emu bursts, then latches VRAM.
  // The burst stops after `max_cpu_budget_sec` of wall-clock time, or after
  // `max_cpu_budget_sec * kDeterministicInstrsPerSec` guest instructions in
  // deterministic mode.
  void run_frame(double dt_sec, double max_cpu_budget_sec = 0.006);

  // Deterministic mode replaces every host-clock input (run_frame budget and
  // DOS time of day) with guest-derived values, so identical inputs and `dt`
  // sequences produce identical guest state.
  void set_deterministic(bool enabled);
  bool deterministic() const { return deterministic_; }

  // Executes an exact maximum instruction count for deterministic unit tests.
  void step_instructions(std::uint64_t max_instructions);

  // Instruction count and stop reason of the most recent run_frame.
  const GuestFrameStats& last_frame_stats() const { return last_frame_stats_; }

  // Enables the GRP600C call census (statistical; profiling only). Disabling
  // discards collected data.
  void set_grp_census_enabled(bool enabled);
  const GrpCensus* grp_census() const { return grp_census_.get(); }

  bool exited() const { return dos_.program_exited(); }
  std::uint8_t exit_code() const { return dos_.exit_code(); }
  bool is_waiting_for_input() const {
    return !holding_in_fade_rom_ && !video_.is_fading() &&
           timer_f0_sec_ <= 0.0 && timer_f1_sec_ <= 0.0 && yield_burst_now_;
  }

  // Speed multiplier controls (`F4` cycles 1x/2x/4x; Hold-`Tab` = 16x Turbo).
  void set_base_speed_multiplier(int mult);
  int base_speed_multiplier() const { return base_speed_mult_; }
  int cycle_base_speed_multiplier();
  void set_turbo_held(bool held) { turbo_held_ = held; }
  int effective_speed_multiplier() const {
    return turbo_held_ ? 16 : base_speed_mult_;
  }

  x86emu_t* emu() { return emu_; }
  const x86emu_t* emu() const { return emu_; }
  std::uint8_t* guest_ram() { return guest_ram_.data(); }
  const std::uint8_t* guest_ram() const { return guest_ram_.data(); }

  VesaPlanarVideo& video() { return video_; }
  const VesaPlanarVideo& video() const { return video_; }
  Opl2Audio& audio() { return audio_; }
  const Opl2Audio& audio() const { return audio_; }
  MouseHle& mouse() { return mouse_; }
  const MouseHle& mouse() const { return mouse_; }
  DosKernel& dos() { return dos_; }
  const DosKernel& dos() const { return dos_; }
  EmsManager& ems() { return ems_; }
  const EmsManager& ems() const { return ems_; }
  HdPackManager& hd_pack() { return hd_pack_; }
  const HdPackManager& hd_pack() const { return hd_pack_; }

 private:
  enum class PendingGrpKind : std::uint8_t {
    None = 0,
    LegacyMultiPlane,
    PlanarBlit0F,
    MonoBlit13,
    CopyRect17,
    SwapRect18,
    BlitNpk19,
  };

  struct PendingPlaneRect {
    PendingGrpKind kind = PendingGrpKind::None;
    int src_x = 0;
    int src_y = 0;
    int dst_x = 0;
    int dst_y = 0;
    int w = 0;
    int h = 0;
    int passes = 0;
    int active_planes = 4;
    std::uint8_t p3_fill_mode = 0;
    std::uint8_t p3_fill_byte = 0;
    std::uint16_t rop_mode = 0;
    std::uint8_t fg_slot = 15;
    std::uint8_t bg_slot = 0;
    std::uint8_t plane_disable = 0;
    GrpClipRect clip{};
    std::vector<std::uint8_t> src_bytes;
    HdPackManager::SavedHdPatch pre_swap_patch;
  };

  void install_bios_rom();
  void flush_pending_plane_blit();
  double current_pit_period_sec() const;
  std::uint16_t current_pit_ch0_count() const;
  void on_bda_byte_read(std::uint32_t addr);
  void record_idle_poll(bool stop_emu_immediately);
  bool is_at_fade_rom() const;
  void release_fade_rom_hold_if_needed();
  int handle_fmdrv_timers();
  // AH=F0h/F1h AL=0 poll: returns the remaining 1/60 s ticks of `timer_sec`
  // after drawing on `budget_sec`.
  std::uint16_t poll_fmdrv_timer(double& timer_sec, double& budget_sec);
  int handle_grp600c_dispatch();
  void record_grp_census_call(std::uint8_t ah, std::uint32_t grp_lin,
                              bool has_real_grp);
  // Feeds one finished emulation slice that started at `slice_begin_tsc` to
  // the census (no-op when disabled).
  void note_census_slice(std::uint64_t slice_begin_tsc) {
    if (grp_census_ && dos_.grp600c_cs() != 0u &&
        emu_->x86.R_CS == dos_.grp600c_cs()) {
      grp_census_->credit_grp_slice(slice_begin_tsc, emu_->x86.R_TSC);
    }
  }

  static unsigned memio_thunk(x86emu_t* emu, std::uint32_t addr,
                              std::uint32_t* val, unsigned type);
  static int intr_thunk(x86emu_t* emu, std::uint8_t num, unsigned type);

  unsigned on_memio(std::uint32_t addr, std::uint32_t* val, unsigned type);
  int on_interrupt(std::uint8_t num, unsigned type);
  std::uint8_t port_in8(std::uint16_t port);
  void port_out8(std::uint16_t port, std::uint8_t val);

  alignas(4096) std::vector<std::uint8_t> guest_ram_;
  x86emu_t* emu_ = nullptr;
  x86emu_memio_handler_t default_memio_ = nullptr;

  CaseInsensitiveVfs vfs_;
  EmsManager ems_;
  VesaPlanarVideo video_;
  Opl2Audio audio_;
  MouseHle mouse_;
  HdPackManager hd_pack_;
  DosKernel dos_;

  // 8253 PIT Channel 0 & 8259 PIC IRQ 0 state
  std::uint16_t pit_ch0_divisor_ = 0;  // 0 = 65536 (~18.2065 Hz) until FMDRV sets 0x1000
  std::uint8_t pit_ch0_access_mode_ = 3u;  // 1=LSB, 2=MSB, 3=LSB+MSB
  bool pit_ch0_write_msb_ = false;
  bool pit_ch0_read_msb_ = false;
  bool pit_ch0_latched_ = false;
  std::uint16_t pit_ch0_latched_count_ = 0;
  std::uint16_t pit_ch0_unlatched_snap_ = 0;
  std::uint8_t pit_ch0_lo_ = 0;
  double pit_accum_sec_ = 0.0;
  std::uint64_t pit_tsc_anchor_ = 0;
  bool pic_irq0_in_service_ = false;
  std::uint64_t irq0_service_instrs_ = 0;
  std::uint8_t pic_imr_ = 0x00u;

  // Speed-scalable FMDRV INT 66h AH=F0h/F1h 1/60s gameplay timers
  int base_speed_mult_ = 1;
  bool turbo_held_ = false;
  double timer_f0_sec_ = 0.0;
  double timer_f1_sec_ = 0.0;
  // Above 1x, the frame's scaled timer advance is handed out lazily to polls
  // so the guest can observe several expiries within one burst.
  double timer_f0_budget_sec_ = 0.0;
  double timer_f1_budget_sec_ = 0.0;

  bool deterministic_ = false;

  // Quiescent input-wait detector for < 2% idle CPU
  int consecutive_idle_polls_ = 0;
  bool yield_burst_now_ = false;

  // GRP600C CS:0x0018 (`INT F1h`) sprite/tile tracking & smooth fade hold state
  PendingPlaneRect pending_plane_rect_{};
  bool holding_in_fade_rom_ = false;
  std::uint16_t fade_resume_cs_ = 0;
  std::uint16_t fade_resume_ip_ = 0;

  GuestFrameStats last_frame_stats_{};
  std::unique_ptr<GrpCensus> grp_census_;
};

}  // namespace san4
