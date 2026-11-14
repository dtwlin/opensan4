#include "machine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "grp600c_defs.h"

namespace san4 {
namespace {

constexpr std::uint16_t kBiosRomSeg = 0xF000u;
constexpr std::uint16_t kRomInt08Off = 0x0080u;
constexpr std::uint16_t kRomInt1COff = 0x01C0u;
constexpr std::uint16_t kRomInt33Off = 0x0330u;
constexpr std::uint16_t kRomDefaultIretOff = 0x0400u;
constexpr std::uint16_t kRomVbeModesOff = 0x0500u;
constexpr std::uint16_t kRomVbeOemOff = 0x0510u;
constexpr std::uint16_t kRomFadeWaitOff = 0x0F30u;
constexpr std::uint16_t kRomFont8x16Off = 0xB000u;

}  // namespace

X86Machine::X86Machine(const MachineConfig& cfg)
    : guest_ram_(kGuestRamBytes, 0u),
      vfs_(cfg.game_dir, cfg.save_dir, cfg.embedded_vfs),
      dos_(vfs_, ems_) {
  set_base_speed_multiplier(cfg.speed_multiplier);
  set_deterministic(cfg.deterministic);
  dos_.set_skip_intro(cfg.skip_intro);
  mouse_.set_warp_enabled(cfg.mouse_warp);
  if (!cfg.hd_pack_dir.empty()) {
    hd_pack_.set_pack_dir(cfg.hd_pack_dir);
  }
  if (!cfg.dump_sprites_dir.empty()) {
    hd_pack_.set_dump_dir(cfg.dump_sprites_dir);
  }

  emu_ = x86emu_new(X86EMU_PERM_RWX, X86EMU_PERM_RW);
  emu_->_private = this;
  default_memio_ = x86emu_set_memio_handler(emu_, &X86Machine::memio_thunk);
  x86emu_set_intr_handler(emu_, &X86Machine::intr_thunk);

  // Map guest RAM across 4 KB pages (including 64 KB HMA above 1 MB).
  // Leave page 0 (0x00000..0x00FFF, IVT + BDA) routed through `on_memio` so
  // direct BDA keyboard polls (`0040:001A`/`0040:001C` in MAIN.EXE) stage
  // queued keys and detect quiescent idle loops cleanly!
  // Note: libx86emu's `x86emu_set_page(emu, page_off, ptr)` takes a 4 KB-aligned
  // byte address (`pg * 4096u`), not a page index!
  const unsigned total_pages = static_cast<unsigned>(kGuestRamBytes / 4096u);
  for (unsigned pg = 1u; pg < total_pages; ++pg) {
    x86emu_set_page(emu_, pg * 4096u, guest_ram_.data() + pg * 4096u);
  }
  x86emu_set_perm(emu_, 0u, static_cast<unsigned>(kGuestRamBytes - 1u),
                  X86EMU_PERM_RWX | X86EMU_PERM_VALID);

  ems_.attach(emu_, guest_ram_.data());
  dos_.attach(emu_, guest_ram_.data(), guest_ram_.size());
  install_bios_rom();
  x86emu_set_perm(emu_, static_cast<unsigned>(kBiosRomSeg) << 4, 0xFFFFFu,
                  X86EMU_PERM_RX | X86EMU_PERM_VALID);
}

X86Machine::~X86Machine() {
  if (emu_ != nullptr) {
    x86emu_done(emu_);
    emu_ = nullptr;
  }
}

void X86Machine::install_bios_rom() {
  const std::uint32_t rom_base = static_cast<std::uint32_t>(kBiosRomSeg) << 4;

  // Default IRET stub at F000:0400
  guest_ram_[rom_base + kRomDefaultIretOff] = 0xCFu;
  for (unsigned vec = 0; vec < 256u; ++vec) {
    const std::uint32_t addr = vec * 4u;
    guest_ram_[addr + 0] = static_cast<std::uint8_t>(kRomDefaultIretOff & 0xFFu);
    guest_ram_[addr + 1] = static_cast<std::uint8_t>(kRomDefaultIretOff >> 8);
    guest_ram_[addr + 2] = static_cast<std::uint8_t>(kBiosRomSeg & 0xFFu);
    guest_ram_[addr + 3] = static_cast<std::uint8_t>(kBiosRomSeg >> 8);
  }

  auto set_ivt = [&](std::uint8_t vec, std::uint16_t seg, std::uint16_t off) {
    const std::uint32_t addr = static_cast<std::uint32_t>(vec) * 4u;
    guest_ram_[addr + 0] = static_cast<std::uint8_t>(off & 0xFFu);
    guest_ram_[addr + 1] = static_cast<std::uint8_t>(off >> 8);
    guest_ram_[addr + 2] = static_cast<std::uint8_t>(seg & 0xFFu);
    guest_ram_[addr + 3] = static_cast<std::uint8_t>(seg >> 8);
  };

  // BIOS INT 08h at F000:0080:
  //   push ax; push ds; xor ax, ax; mov ds, ax;
  //   add word [0x046C], 1; adc word [0x046E], 0;
  //   int 0x1C; mov al, 0x20; out 0x20, al; pop ds; pop ax; iret
  static constexpr std::uint8_t kInt08Stub[] = {
      0x50, 0x1E, 0x31, 0xC0, 0x8E, 0xD8, 0x83, 0x06, 0x6C, 0x04, 0x01,
      0x83, 0x16, 0x6E, 0x04, 0x00, 0xCD, 0x1C, 0xB0, 0x20, 0xE6, 0x20,
      0x1F, 0x58, 0xCF,
  };
  std::memcpy(guest_ram_.data() + rom_base + kRomInt08Off, kInt08Stub,
              sizeof(kInt08Stub));
  set_ivt(0x08u, kBiosRomSeg, kRomInt08Off);

  // BIOS INT 1Ch at F000:01C0: iret
  guest_ram_[rom_base + kRomInt1COff] = 0xCFu;
  set_ivt(0x1Cu, kBiosRomSeg, kRomInt1COff);

  // Boot INT 33h at F000:0330 (`int 0x33; iret`) so GRP600C's far-call into
  // saved IVT[0x33] (`pushf; lcall *%cs:0x89`) traps cleanly into MouseHle!
  guest_ram_[rom_base + kRomInt33Off + 0] = 0xCDu;
  guest_ram_[rom_base + kRomInt33Off + 1] = 0x33u;
  guest_ram_[rom_base + kRomInt33Off + 2] = 0xCFu;
  set_ivt(0x33u, kBiosRomSeg, kRomInt33Off);

  // EMS INT 67h vector -> E000:0000 (where +0x0A holds "EMMXXXX0")
  set_ivt(0x67u, EmsManager::kDeviceHeaderSeg, 0x0000u);

  // Smooth fade mainline-hold loop at F000:0F30 (`sti; jmp -2`)
  guest_ram_[rom_base + kRomFadeWaitOff + 0] = 0xFBu;
  guest_ram_[rom_base + kRomFadeWaitOff + 1] = 0xEBu;
  guest_ram_[rom_base + kRomFadeWaitOff + 2] = 0xFEu;

  // VESA VBE 1.2 mode list `{0x0102, 0x006A, 0xFFFF}` at F000:0500 and OEM
  // string at F000:0510 for GRP600C's AX=4F00h/4F01h probe.
  static constexpr std::uint8_t kVbeModes[] = {
      0x02, 0x01, 0x6A, 0x00, 0xFF, 0xFF,
  };
  std::memcpy(guest_ram_.data() + rom_base + kRomVbeModesOff, kVbeModes,
              sizeof(kVbeModes));
  static constexpr char kVbeOem[] = "SAN4 CleanRoom VESA VBE 1.2";
  std::memcpy(guest_ram_.data() + rom_base + kRomVbeOemOff, kVbeOem,
              sizeof(kVbeOem));

  // Copy 4096-byte 8x16 VGA ROM font into F000:B000
  const auto font = VesaPlanarVideo::builtin_font_8x16();
  std::memcpy(guest_ram_.data() + rom_base + kRomFont8x16Off, font.data(),
              font.size());

  // Populate minimal BIOS Data Area (BDA 0x0400): 640 KB conventional memory
  // at 0x0413 (`0x0280`), keyboard circular buffer pointers at 0x041A..0x0482,
  // 80 columns at 0x044A, CRT port 0x03D4 at 0x0463.
  guest_ram_[0x0413] = 0x80u;
  guest_ram_[0x0414] = 0x02u;
  guest_ram_[0x041A] = 0x1Eu;
  guest_ram_[0x041B] = 0x00u;
  guest_ram_[0x041C] = 0x1Eu;
  guest_ram_[0x041D] = 0x00u;
  guest_ram_[0x044A] = 80u;
  guest_ram_[0x0463] = 0xD4u;
  guest_ram_[0x0464] = 0x03u;
  guest_ram_[0x0480] = 0x1Eu;
  guest_ram_[0x0481] = 0x00u;
  guest_ram_[0x0482] = 0x3Eu;
  guest_ram_[0x0483] = 0x00u;
}

bool X86Machine::boot(std::string_view program, std::string_view cmd_tail) {
  return dos_.boot_program(program, cmd_tail);
}

void X86Machine::load_synthetic_code(std::uint16_t seg, std::uint16_t off,
                                     std::span<const std::uint8_t> code,
                                     std::uint16_t stack_seg,
                                     std::uint16_t stack_sp) {
  const std::uint32_t lin = (static_cast<std::uint32_t>(seg) << 4) + off;
  if (lin + code.size() <= guest_ram_.size()) {
    std::memcpy(guest_ram_.data() + lin, code.data(), code.size());
  }
  x86emu_set_seg_register(emu_, emu_->x86.R_CS_SEL, seg);
  x86emu_set_seg_register(emu_, emu_->x86.R_DS_SEL, seg);
  x86emu_set_seg_register(emu_, emu_->x86.R_ES_SEL, seg);
  x86emu_set_seg_register(emu_, emu_->x86.R_SS_SEL, stack_seg);
  emu_->x86.R_EIP = off;
  emu_->x86.R_ESP = stack_sp;
  emu_->x86.R_EFLG = F_ALWAYS_ON | FB_IF;
}

void X86Machine::set_base_speed_multiplier(int mult) {
  if (mult <= 1) {
    base_speed_mult_ = 1;
  } else if (mult <= 2) {
    base_speed_mult_ = 2;
  } else {
    base_speed_mult_ = 4;
  }
}

void X86Machine::set_deterministic(bool enabled) {
  deterministic_ = enabled;
  dos_.set_deterministic_clock(enabled);
}

int X86Machine::cycle_base_speed_multiplier() {
  if (base_speed_mult_ == 1) {
    base_speed_mult_ = 2;
  } else if (base_speed_mult_ == 2) {
    base_speed_mult_ = 4;
  } else {
    base_speed_mult_ = 1;
  }
  return base_speed_mult_;
}

double X86Machine::current_pit_period_sec() const {
  const double div =
      (pit_ch0_divisor_ == 0u) ? 65536.0 : static_cast<double>(pit_ch0_divisor_);
  return div / kPitBaseHz;
}

std::uint16_t X86Machine::current_pit_ch0_count() const {
  const std::uint32_t eff_div =
      (pit_ch0_divisor_ == 0u) ? 65536u
                               : static_cast<std::uint32_t>(pit_ch0_divisor_);
  const std::uint64_t tsc_delta =
      (emu_ != nullptr && emu_->x86.R_TSC >= pit_tsc_anchor_)
          ? (emu_->x86.R_TSC - pit_tsc_anchor_)
          : 0u;
  const double elapsed_raw =
      std::fmod(pit_accum_sec_ * kPitBaseHz + static_cast<double>(tsc_delta),
                static_cast<double>(eff_div));
  const auto elapsed = static_cast<std::uint32_t>(
      std::clamp(elapsed_raw, 0.0, static_cast<double>(eff_div - 1u)));
  return static_cast<std::uint16_t>((eff_div - elapsed) & 0xFFFFu);
}

bool X86Machine::is_at_fade_rom() const {
  return emu_ != nullptr && emu_->x86.R_CS == kBiosRomSeg &&
         emu_->x86.R_IP >= kRomFadeWaitOff &&
         emu_->x86.R_IP <= kRomFadeWaitOff + 2u;
}

void X86Machine::release_fade_rom_hold_if_needed() {
  if (!holding_in_fade_rom_ || video_.is_fading()) {
    return;
  }
  if (is_at_fade_rom()) {
    holding_in_fade_rom_ = false;
    pic_irq0_in_service_ = false;
    x86emu_set_seg_register(emu_, emu_->x86.R_CS_SEL, fade_resume_cs_);
    emu_->x86.R_IP = fade_resume_ip_;
  }
}

void X86Machine::step_instructions(std::uint64_t max_instructions) {
  if (max_instructions == 0u || dos_.program_exited()) {
    return;
  }
  emu_->max_instr = emu_->x86.R_TSC + max_instructions;
  x86emu_run(emu_, X86EMU_RUN_MAX_INSTR);
  if (!pic_irq0_in_service_ && dos_.grp600c_cs() != 0u &&
      emu_->x86.R_CS != dos_.grp600c_cs()) {
    flush_pending_plane_blit();
    hd_pack_.sync_vram_occlusions(video_);
  }
}

void X86Machine::run_frame(double dt_sec, double max_cpu_budget_sec) {
  last_frame_stats_ = GuestFrameStats{};
  if (dos_.program_exited()) {
    return;
  }
  const std::uint64_t frame_start_tsc = emu_->x86.R_TSC;
  GuestYieldReason reason = GuestYieldReason::Exited;
  dt_sec = std::clamp(dt_sec, 0.0, 0.050);

  // 1. Advance speed-scaled 1/60s gameplay timers (F4 1x/2x/4x, Tab 16x).
  //    Above 1x the advance becomes a budget that polls draw on mid-burst.
  const double scaled_dt =
      dt_sec * static_cast<double>(effective_speed_multiplier());
  if (effective_speed_multiplier() > 1) {
    timer_f0_budget_sec_ = scaled_dt;
    timer_f1_budget_sec_ = scaled_dt;
  } else {
    timer_f0_sec_ = std::max(0.0, timer_f0_sec_ - scaled_dt);
    timer_f1_sec_ = std::max(0.0, timer_f1_sec_ - scaled_dt);
  }

  // 2. Advance smooth 24-bit host palette fade if active.
  //    Never overwrite CS:IP while FMDRV's IRQ 0 ISR is mid-execution (FMDRV
  //    sends early EOI `OUT 0x20, 0x20` before running its OPL2 sequencer!).
  if (holding_in_fade_rom_) {
    if (!video_.advance_fade(dt_sec)) {
      release_fade_rom_hold_if_needed();
    }
  }

  // 3. Interleave real-time 291.304 Hz PIT IRQ 0 ticks + write-exact OPL2 audio
  //    with unthrottled libx86emu bursts up to max_cpu_budget_sec.
  pit_accum_sec_ += dt_sec;
  pit_tsc_anchor_ = emu_->x86.R_TSC;
  consecutive_idle_polls_ = 0;
  yield_burst_now_ = false;
  video_.consume_vram_activity();
  dos_.consume_file_activity();

  const auto start_tp = std::chrono::steady_clock::now();
  static constexpr std::uint64_t kSliceInstrs = 6000u;
  static constexpr std::uint64_t kFadeIrqSliceInstrs = 4000u;
  std::uint64_t fade_isr_guard = 0;

  while (!dos_.program_exited()) {
    if (holding_in_fade_rom_ && !video_.is_fading()) {
      release_fade_rom_hold_if_needed();
    }

    const double pit_period = current_pit_period_sec();
    bool fired_irq0 = false;

    if (pit_accum_sec_ >= pit_period) {
      const bool can_fire_now =
          (emu_->x86.R_EFLG & FB_IF) != 0u && !pic_irq0_in_service_ &&
          (pic_imr_ & 0x01u) == 0u &&
          (!holding_in_fade_rom_ || is_at_fade_rom());
      if (can_fire_now) {
        pit_accum_sec_ -= pit_period;
        pit_tsc_anchor_ = emu_->x86.R_TSC;
        audio_.advance_seconds(pit_period);
        pic_irq0_in_service_ = true;
        irq0_service_instrs_ = 0;
        x86emu_intr_raise(emu_, 0x08u, INTR_TYPE_SOFT, 0u);
        fired_irq0 = true;
      } else if (pit_accum_sec_ > pit_period * 8.0) {
        // Guest held CLI for a long disk/init stretch: keep audio flowing!
        pit_accum_sec_ -= pit_period;
        pit_tsc_anchor_ = emu_->x86.R_TSC;
        audio_.advance_seconds(pit_period);
      }
    }

    if (holding_in_fade_rom_) {
      if (fired_irq0 || !is_at_fade_rom()) {
        const auto prev_fade_tsc = emu_->x86.R_TSC;
        emu_->max_instr = prev_fade_tsc + kFadeIrqSliceInstrs;
        x86emu_run(emu_, X86EMU_RUN_MAX_INSTR);
        note_census_slice(prev_fade_tsc);
        fade_isr_guard += kFadeIrqSliceInstrs;
        if (is_at_fade_rom()) {
          pic_irq0_in_service_ = false;
          if (!video_.is_fading()) {
            release_fade_rom_hold_if_needed();
            continue;
          }
        } else if (fade_isr_guard < 250000u &&
                   emu_->x86.R_TSC != prev_fade_tsc) {
          continue;
        } else {
          if (!video_.is_fading()) {
            holding_in_fade_rom_ = false;
          }
          reason = GuestYieldReason::FadeHold;
          break;
        }
        continue;
      }
      if (!video_.is_fading()) {
        release_fade_rom_hold_if_needed();
        continue;
      }
      reason = GuestYieldReason::FadeHold;
      break;
    }

    const bool irq0_deliverable =
        (emu_->x86.R_EFLG & FB_IF) != 0u && !pic_irq0_in_service_ &&
        (pic_imr_ & 0x01u) == 0u;

    if (yield_burst_now_ && (pit_accum_sec_ < pit_period || !irq0_deliverable) &&
        !pic_irq0_in_service_) {
      reason = GuestYieldReason::Idle;
      break;
    }

    const auto prev_tsc = emu_->x86.R_TSC;
    emu_->max_instr = prev_tsc + kSliceInstrs;
    x86emu_run(emu_, X86EMU_RUN_MAX_INSTR);
    note_census_slice(prev_tsc);
    const auto ran = emu_->x86.R_TSC - prev_tsc;
    if (ran == 0u) {
      reason = GuestYieldReason::Stalled;
      break;
    }

    if (pic_irq0_in_service_) {
      irq0_service_instrs_ += ran;
      if (irq0_service_instrs_ > 64000u) {
        pic_irq0_in_service_ = false;
      }
    }

    const bool budget_spent =
        deterministic_
            ? static_cast<double>(emu_->x86.R_TSC - frame_start_tsc) >=
                  max_cpu_budget_sec * kDeterministicInstrsPerSec
            : std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                            start_tp)
                      .count() >= max_cpu_budget_sec;
    if (budget_spent && (pit_accum_sec_ < pit_period || !irq0_deliverable)) {
      reason = GuestYieldReason::Budget;
      break;
    }
  }
  last_frame_stats_.guest_instrs = emu_->x86.R_TSC - frame_start_tsc;
  last_frame_stats_.yield_reason = reason;
  if (grp_census_) {
    grp_census_->add_guest_instrs(last_frame_stats_.guest_instrs);
  }

  // Timer advance the guest did not poll for this frame still elapses.
  timer_f0_sec_ = std::max(0.0, timer_f0_sec_ - timer_f0_budget_sec_);
  timer_f1_sec_ = std::max(0.0, timer_f1_sec_ - timer_f1_budget_sec_);
  timer_f0_budget_sec_ = 0.0;
  timer_f1_budget_sec_ = 0.0;

  // Drain any remaining audio time (clamped to frame) so SDL ring stays full.
  if (pit_accum_sec_ > 0.0) {
    const double flush_sec = std::min(pit_accum_sec_, 0.050);
    audio_.advance_seconds(flush_sec);
    pit_accum_sec_ -= flush_sec;
    pit_tsc_anchor_ = emu_->x86.R_TSC;
  }

  // 4. Flush any completed GRP600C HLE operation (once guest has returned
  //    from GRP600C), sync raw VRAM occlusions, and latch frame.
  if (!pic_irq0_in_service_ &&
      (dos_.grp600c_cs() == 0u || emu_->x86.R_CS != dos_.grp600c_cs())) {
    flush_pending_plane_blit();
    hd_pack_.sync_vram_occlusions(video_);
  }
  video_.latch_frame();
}

void X86Machine::flush_pending_plane_blit() {
  if (pending_plane_rect_.kind == PendingGrpKind::None) {
    return;
  }
  PendingPlaneRect r = std::move(pending_plane_rect_);
  pending_plane_rect_ = PendingPlaneRect{};

  switch (r.kind) {
    case PendingGrpKind::LegacyMultiPlane:
      if (r.passes >= 1) {
        hd_pack_.on_planar_blit_complete(r.dst_x, r.dst_y, r.w, r.h, video_);
      }
      break;
    case PendingGrpKind::PlanarBlit0F:
      video_.clear_hd_occlude_bits();
      hd_pack_.on_planar_blit_source(
          r.dst_x, r.dst_y, r.w, r.h, r.active_planes, r.p3_fill_mode,
          r.p3_fill_byte, r.rop_mode, r.plane_disable, r.clip, r.src_bytes,
          video_);
      break;
    case PendingGrpKind::MonoBlit13:
      video_.clear_hd_occlude_bits();
      hd_pack_.on_mono_blit_1bit(r.dst_x, r.dst_y, r.w, r.h, r.rop_mode,
                                 r.fg_slot, r.bg_slot, r.plane_disable, r.clip,
                                 r.src_bytes, video_);
      break;
    case PendingGrpKind::CopyRect17:
      video_.clear_hd_occlude_bits();
      hd_pack_.on_vram_copy_rect(r.src_x, r.src_y, r.w, r.h, r.dst_x, r.dst_y);
      break;
    case PendingGrpKind::SwapRect18:
      video_.clear_hd_occlude_bits();
      hd_pack_.on_vram_swap_complete(r.dst_x, r.dst_y, r.w, r.h, r.clip,
                                     std::move(r.pre_swap_patch), video_);
      break;
    case PendingGrpKind::BlitNpk19:
      video_.clear_hd_occlude_bits();
      hd_pack_.on_npk_blit_complete(r.dst_x, r.dst_y, r.w, r.h, r.clip,
                                    r.src_bytes, video_);
      break;
    case PendingGrpKind::None:
      break;
  }
}

unsigned X86Machine::memio_thunk(x86emu_t* emu, std::uint32_t addr,
                                 std::uint32_t* val, unsigned type) {
  auto* self = static_cast<X86Machine*>(emu->_private);
  return self->on_memio(addr, val, type);
}

int X86Machine::intr_thunk(x86emu_t* emu, std::uint8_t num, unsigned type) {
  auto* self = static_cast<X86Machine*>(emu->_private);
  return self->on_interrupt(num, type);
}

void X86Machine::record_idle_poll(bool stop_emu_immediately) {
  if (video_.consume_vram_activity() == 0u &&
      dos_.consume_file_activity() == 0u) {
    if (++consecutive_idle_polls_ >= 2) {
      yield_burst_now_ = true;
      if (stop_emu_immediately) {
        x86emu_stop(emu_);
      } else {
        // Allow guest's `sti; pop; ret` (~8 instrs) to finish before stop!
        emu_->max_instr = std::min(emu_->max_instr, emu_->x86.R_TSC + 16u);
      }
    }
  } else {
    consecutive_idle_polls_ = 0;
  }
}

void X86Machine::on_bda_byte_read(std::uint32_t addr) {
  if (addr == 0x041Au) {
    dos_.pump_bda_keyboard();
  } else if (addr == 0x041Cu) {
    dos_.pump_bda_keyboard();
    if (!dos_.bda_has_key()) {
      record_idle_poll(false);
    } else {
      consecutive_idle_polls_ = 0;
    }
  }
}

unsigned X86Machine::on_memio(std::uint32_t addr, std::uint32_t* val,
                              unsigned type) {
  const unsigned op = type & 0xFF00u;
  const unsigned sz = type & 0xFFu;
  const unsigned nbytes =
      (sz == X86EMU_MEMIO_32) ? 4u : ((sz == X86EMU_MEMIO_16) ? 2u : 1u);
  const bool is_single_byte =
      (sz == X86EMU_MEMIO_8 || sz == X86EMU_MEMIO_8_NOPERM);
  constexpr std::uint32_t kBiosRomBase =
      static_cast<std::uint32_t>(kBiosRomSeg) << 4;

  // Hot path: memory accesses wholly inside mapped conventional RAM
  // [0x01000, 0xA0000). Page 0 (IVT + BDA poll detection) and planar VRAM
  // (0xA0000+) lie outside this window and keep their routing below.
  if (op <= X86EMU_MEMIO_X && addr - 0x01000u < 0x9F000u &&
      addr + nbytes <= 0xA0000u) {
    return default_memio_(emu_, addr, val, type);
  }

  if (op == X86EMU_MEMIO_R || op == X86EMU_MEMIO_W || op == X86EMU_MEMIO_X) {
    // Fast single-byte paths (including X86EMU_MEMIO_8_NOPERM) for Page 0 and
    // Planar VRAM.
    if (is_single_byte) {
      if (addr < 0x01000u) {
        if (op == X86EMU_MEMIO_W) {
          guest_ram_[addr] = static_cast<std::uint8_t>(*val & 0xFFu);
        } else {
          if (op == X86EMU_MEMIO_R) {
            on_bda_byte_read(addr);
          }
          *val = guest_ram_[addr];
        }
        return 0u;
      }
      if (addr >= 0xA0000u && addr < 0xB0000u) {
        if (op == X86EMU_MEMIO_W) {
          video_.vram_write8(addr - 0xA0000u,
                             static_cast<std::uint8_t>(*val & 0xFFu));
        } else {
          *val = video_.vram_read8(addr - 0xA0000u);
        }
        return 0u;
      }
    }

    // Fast path ONLY when the entire [addr, addr + nbytes) access lies strictly
    // inside mapped UMB/EMS/HMA pages (or read-only BIOS ROM) and never
    // straddles into write-protected BIOS ROM [0xF0000, 0xFFFFF].
    const std::uint64_t end_addr = static_cast<std::uint64_t>(addr) + nbytes;
    const bool touches_rom_write =
        (op == X86EMU_MEMIO_W && addr < 0x100000u && end_addr > kBiosRomBase);
    const bool in_upper_mem =
        (!touches_rom_write && addr >= 0xB0000u && end_addr <= kBiosRomBase);
    const bool in_hma = (addr >= 0x100000u && end_addr <= kGuestRamBytes);
    const bool in_rom_ro =
        (op != X86EMU_MEMIO_W && addr >= kBiosRomBase && end_addr <= 0x100000u);
    if (in_upper_mem || in_hma || in_rom_ro) {
      return default_memio_(emu_, addr, val, type);
    }

    // Unified per-byte routing for multi-byte Page 0 R/W, Planar VRAM R/W,
    // BIOS ROM writes, out-of-range >= kGuestRamBytes, and ALL cross-boundary
    // 16/32-bit straddles across 0x01000, 0xA0000, 0xB0000, 0xF0000, 0x100000.
    const unsigned byte_sz =
        (sz == X86EMU_MEMIO_8_NOPERM) ? X86EMU_MEMIO_8_NOPERM : X86EMU_MEMIO_8;
    if (op == X86EMU_MEMIO_W) {
      const std::uint32_t v = *val;
      for (unsigned i = 0; i < nbytes; ++i) {
        const std::uint64_t a64 = static_cast<std::uint64_t>(addr) + i;
        if (a64 >= kGuestRamBytes) {
          continue;
        }
        const auto a = static_cast<std::uint32_t>(a64);
        const auto b = static_cast<std::uint8_t>((v >> (i * 8u)) & 0xFFu);
        if (a < 0x01000u) {
          guest_ram_[a] = b;
        } else if (a >= 0xA0000u && a < 0xB0000u) {
          video_.vram_write8(a - 0xA0000u, b);
        } else if (a >= kBiosRomBase && a < 0x100000u) {
          // BIOS ROM 0xF0000..0xFFFFF is write-protected.
        } else {
          std::uint32_t tmp = b;
          default_memio_(emu_, a, &tmp, X86EMU_MEMIO_W | byte_sz);
        }
      }
      return 0u;
    }

    std::uint32_t out = 0;
    for (unsigned i = 0; i < nbytes; ++i) {
      const std::uint64_t a64 = static_cast<std::uint64_t>(addr) + i;
      std::uint8_t b = 0xFFu;
      if (a64 < kGuestRamBytes) {
        const auto a = static_cast<std::uint32_t>(a64);
        if (a < 0x01000u) {
          if (op == X86EMU_MEMIO_R) {
            on_bda_byte_read(a);
          }
          b = guest_ram_[a];
        } else if (a >= 0xA0000u && a < 0xB0000u) {
          b = video_.vram_read8(a - 0xA0000u);
        } else if (a >= kBiosRomBase && a < 0x100000u) {
          b = guest_ram_[a];
        } else {
          std::uint32_t tmp = 0;
          default_memio_(emu_, a, &tmp, op | byte_sz);
          b = static_cast<std::uint8_t>(tmp & 0xFFu);
        }
      }
      out |= (static_cast<std::uint32_t>(b) << (i * 8u));
    }
    *val = out;
    return 0u;
  }

  // Port I/O: split 16/32-bit IN/OUT (`OUT DX, AX` on 0x3C4/0x3CE!) into
  // sequential 8-bit port calls (`port + 0`, `port + 1`, ...).
  if (op == X86EMU_MEMIO_O) {
    const std::uint32_t v = *val;
    for (unsigned i = 0; i < nbytes; ++i) {
      port_out8(static_cast<std::uint16_t>(addr + i),
                static_cast<std::uint8_t>((v >> (i * 8u)) & 0xFFu));
    }
    return 0u;
  }

  if (op == X86EMU_MEMIO_I) {
    std::uint32_t out = 0;
    for (unsigned i = 0; i < nbytes; ++i) {
      const std::uint8_t b = port_in8(static_cast<std::uint16_t>(addr + i));
      out |= (static_cast<std::uint32_t>(b) << (i * 8u));
    }
    *val = out;
    return 0u;
  }

  return default_memio_(emu_, addr, val, type);
}

std::uint8_t X86Machine::port_in8(std::uint16_t port) {
  if (video_.handles_port(port)) {
    return video_.port_in8(port);
  }
  if (audio_.handles_port(port)) {
    return audio_.port_in8(port);
  }
  switch (port) {
    case 0x0021u:
      return pic_imr_;
    case 0x0040u: {
      if (pit_ch0_access_mode_ == 1u) {
        const std::uint16_t v =
            pit_ch0_latched_ ? pit_ch0_latched_count_ : current_pit_ch0_count();
        pit_ch0_latched_ = false;
        return static_cast<std::uint8_t>(v & 0xFFu);
      }
      if (pit_ch0_access_mode_ == 2u) {
        const std::uint16_t v =
            pit_ch0_latched_ ? pit_ch0_latched_count_ : current_pit_ch0_count();
        pit_ch0_latched_ = false;
        return static_cast<std::uint8_t>((v >> 8) & 0xFFu);
      }
      if (!pit_ch0_read_msb_) {
        if (!pit_ch0_latched_) {
          pit_ch0_unlatched_snap_ = current_pit_ch0_count();
        }
        pit_ch0_read_msb_ = true;
        const std::uint16_t v =
            pit_ch0_latched_ ? pit_ch0_latched_count_ : pit_ch0_unlatched_snap_;
        return static_cast<std::uint8_t>(v & 0xFFu);
      }
      const std::uint16_t v =
          pit_ch0_latched_ ? pit_ch0_latched_count_ : pit_ch0_unlatched_snap_;
      pit_ch0_read_msb_ = false;
      pit_ch0_latched_ = false;
      return static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    }
    case 0x0060u:
      return 0x00u;
    // Stubbed dead PC-98 / delay ports
    case 0x005Fu:
    case 0x007Cu:
    case 0x007Eu:
    default:
      return 0xFFu;
  }
}

void X86Machine::port_out8(std::uint16_t port, std::uint8_t val) {
  if (video_.handles_port(port)) {
    video_.port_out8(port, val);
    return;
  }
  if (audio_.handles_port(port)) {
    audio_.port_out8(port, val);
    return;
  }
  switch (port) {
    case 0x0020u:
      if ((val & 0x20u) != 0u) {
        pic_irq0_in_service_ = false;
      }
      break;
    case 0x0021u:
      pic_imr_ = val;
      break;
    case 0x0040u:
      if (pit_ch0_access_mode_ == 1u) {
        pit_ch0_divisor_ = val;
        pit_tsc_anchor_ = emu_ ? emu_->x86.R_TSC : 0u;
      } else if (pit_ch0_access_mode_ == 2u) {
        pit_ch0_divisor_ = static_cast<std::uint16_t>(val) << 8;
        pit_tsc_anchor_ = emu_ ? emu_->x86.R_TSC : 0u;
      } else if (!pit_ch0_write_msb_) {
        pit_ch0_lo_ = val;
        pit_ch0_write_msb_ = true;
      } else {
        pit_ch0_divisor_ = static_cast<std::uint16_t>(pit_ch0_lo_) |
                           (static_cast<std::uint16_t>(val) << 8);
        pit_ch0_write_msb_ = false;
        pit_tsc_anchor_ = emu_ ? emu_->x86.R_TSC : 0u;
      }
      break;
    case 0x0043u: {
      const std::uint8_t sc = static_cast<std::uint8_t>((val >> 6) & 3u);
      const std::uint8_t rl = static_cast<std::uint8_t>((val >> 4) & 3u);
      if (sc == 0u) {
        if (rl == 0u) {
          if (!pit_ch0_latched_) {
            pit_ch0_latched_ = true;
            pit_ch0_latched_count_ = current_pit_ch0_count();
            pit_ch0_read_msb_ = false;
          }
        } else {
          pit_ch0_access_mode_ = rl;
          pit_ch0_write_msb_ = false;
          pit_ch0_read_msb_ = false;
          pit_ch0_latched_ = false;
        }
      } else if (sc == 2u) {
        audio_.on_pit_control(val);
      }
      break;
    }
    default:
      break;
  }
}

void X86Machine::set_grp_census_enabled(bool enabled) {
  if (!enabled) {
    grp_census_.reset();
  } else if (!grp_census_) {
    grp_census_ = std::make_unique<GrpCensus>();
  }
}

void X86Machine::record_grp_census_call(std::uint8_t ah, std::uint32_t grp_lin,
                                        bool has_real_grp) {
  std::uint16_t rop = 0;
  if (has_real_grp) {
    const std::uint32_t off = grp_lin + ((ah == kGrpBlitMono)
                                             ? kGrpCsMonoRopOffset
                                             : kGrpCsPlanarRopOffset);
    rop = static_cast<std::uint16_t>(
        guest_ram_[off] | (static_cast<std::uint16_t>(guest_ram_[off + 1u]) << 8));
  }
  grp_census_->record_call(ah, emu_->x86.R_AL, rop);
}

std::uint16_t X86Machine::poll_fmdrv_timer(double& timer_sec,
                                            double& budget_sec) {
  if (timer_sec > 0.0 && budget_sec > 0.0) {
    // Tolerance absorbs the rounding of repeated BX/60 set + subtract cycles.
    constexpr double kTimerEpsSec = 1e-9;
    if (timer_sec <= budget_sec + kTimerEpsSec) {
      budget_sec = std::max(0.0, budget_sec - timer_sec);
      timer_sec = 0.0;
    } else {
      timer_sec -= budget_sec;
      budget_sec = 0.0;
    }
  }
  const auto rem = static_cast<std::uint16_t>(std::ceil(timer_sec * 60.0));
  if (rem > 0u) {
    record_idle_poll(true);
  }
  return rem;
}

int X86Machine::handle_fmdrv_timers() {
  // Intercept ONLY AH=F0h & AH=F1h (1/60s gameplay timers) so F4/Tab speed
  // scaling accelerates marches/animations without altering FMDRV BGM!
  const std::uint8_t ah = emu_->x86.R_AH;
  const std::uint8_t al = emu_->x86.R_AL;
  if (ah == 0xF0u) {
    if (al != 0u) {
      timer_f0_sec_ = static_cast<double>(emu_->x86.R_BX) / 60.0;
    } else {
      emu_->x86.R_BX = poll_fmdrv_timer(timer_f0_sec_, timer_f0_budget_sec_);
    }
    return 1;
  }
  if (ah == 0xF1u) {
    if (al != 0u) {
      timer_f1_sec_ = static_cast<double>(emu_->x86.R_BX) / 60.0;
    } else {
      emu_->x86.R_BX = poll_fmdrv_timer(timer_f1_sec_, timer_f1_budget_sec_);
    }
    return 1;
  }
  // Vector all music/SFX/TSR functions (AH=00h..EFh, F2h..FFh) to guest IVT[0x66]!
  return 0;
}

int X86Machine::handle_grp600c_dispatch() {
  const std::uint8_t ah = emu_->x86.R_AH;
  const std::uint16_t grp_cs = emu_->x86.R_CS;
  const std::uint32_t grp_lin = static_cast<std::uint32_t>(grp_cs) << 4;
  const bool has_real_grp =
      (dos_.grp600c_cs() != 0u && grp_cs == dos_.grp600c_cs() &&
       grp_lin + kGrpMinSegSpan <= guest_ram_.size());
  const std::uint32_t ss_sp =
      (static_cast<std::uint32_t>(emu_->x86.R_SS) << 4) + emu_->x86.R_SP;

  auto read_grp_u16 = [&]( std::uint16_t off) -> std::uint16_t {
    const std::uint32_t p = grp_lin + off;
    return static_cast<std::uint16_t>(guest_ram_[p]) |
           (static_cast<std::uint16_t>(guest_ram_[p + 1u]) << 8);
  };
  if (grp_census_) {
    record_grp_census_call(ah, grp_lin, has_real_grp);
  }
  auto read_grp_clip = [&]() -> GrpClipRect {
    if (!has_real_grp) {
      return GrpClipRect{};
    }
    return GrpClipRect{
        .x0 = static_cast<std::int16_t>(read_grp_u16(kGrpCsClipX0Offset)),
        .y0 = static_cast<std::int16_t>(read_grp_u16(kGrpCsClipY0Offset)),
        .x1 = static_cast<std::int16_t>(read_grp_u16(kGrpCsClipX1Offset)),
        .y1 = static_cast<std::int16_t>(read_grp_u16(kGrpCsClipY1Offset)),
    };
  };

  // 1. Flush any completed previous GRP600C HLE operation before starting the
  //    next call (preserving MS C 7.0 MOVE overlay stack frames untouched!).
  if (pending_plane_rect_.kind != PendingGrpKind::None) {
    if (pending_plane_rect_.kind == PendingGrpKind::LegacyMultiPlane) {
      const bool is_same_plane_rect =
          ((ah == kGrpBlitPlanar || ah == kGrpBlitMono) &&
           static_cast<std::int16_t>(emu_->x86.R_BX) ==
               pending_plane_rect_.dst_x &&
           static_cast<std::int16_t>(emu_->x86.R_CX) ==
               pending_plane_rect_.dst_y &&
           static_cast<std::int16_t>(emu_->x86.R_SI) == pending_plane_rect_.w &&
           static_cast<std::int16_t>(emu_->x86.R_DI) == pending_plane_rect_.h);
      const bool is_plane_attr_setup =
          (ah == kGrpSetPlaneDisable || ah == kGrpSetActivePlanes ||
           ah == kGrpSetPlanarRop || ah == kGrpSetMonoRop || ah == 0x22u);
      if (!is_same_plane_rect && !is_plane_attr_setup) {
        flush_pending_plane_blit();
      }
    } else {
      flush_pending_plane_blit();
    }
  }

  // Apply any non-HLE VRAM occlusions (AH=0x0A..0x0E lines/boxes or raw guest
  // VRAM writes) accumulated since the last GRP600C operation!
  hd_pack_.sync_vram_occlusions(video_);

  if (ah == kGrpInit || ah == kGrpReset || ah == kGrpClearScreen) {
    if (video_.is_fading()) {
      video_.finish_fade_immediately();
      release_fade_rom_hold_if_needed();
    }
    video_.clear_all_glyphs();
    hd_pack_.clear_overlays();
    return 1;
  }
  if (ah == kGrpSetPaletteSingle || ah == kGrpSetPaletteAll) {
    if (video_.is_fading()) {
      video_.finish_fade_immediately();
      release_fade_rom_hold_if_needed();
    }
    return 1;
  }
  // AH=0x0D (AL&3)==2 tile-pattern box fill (GRP600C CS:0x383F): with the
  // opaque PSET ROP and every plane enabled, each pixel of the clipped box is
  // overwritten, so any glyph marks there are stale. Degenerate boxes are
  // lines (CS:0x3252), and other ROPs or plane masks let existing text show
  // through, so those keep their marks.
  if (ah == kGrpDrawBox && (emu_->x86.R_AL & 0x03u) == 0x02u &&
      has_real_grp && read_grp_u16(kGrpCsPlanarRopOffset) == 0u &&
      (guest_ram_[grp_lin + kGrpCsPlaneDisableOffset] & 0x0Fu) == 0u) {
    const int x0 = static_cast<std::int16_t>(emu_->x86.R_BX);
    const int y0 = static_cast<std::int16_t>(emu_->x86.R_CX);
    const int x1 = static_cast<std::int16_t>(emu_->x86.R_SI);
    const int y1 = static_cast<std::int16_t>(emu_->x86.R_DI);
    if (x0 != x1 && y0 != y1) {
      const GrpClipRect clip = read_grp_clip();
      const int rx0 = std::max<int>(std::min(x0, x1), clip.x0);
      const int ry0 = std::max<int>(std::min(y0, y1), clip.y0);
      const int rx1 = std::min<int>(std::max(x0, x1), clip.x1);
      const int ry1 = std::min<int>(std::max(y0, y1), clip.y1);
      video_.clear_glyph_rect(rx0, ry0, rx1 - rx0 + 1, ry1 - ry0 + 1);
    }
    return 1;
  }

  // AH=0x10 (`ReadPlanar`, VRAM -> ES:BP): snapshot active HD background slice
  // BEFORE GRP600C reads VRAM so popup menus & sprites restore 4x HD seamlessly!
  if (ah == kGrpReadPlanar && has_real_grp) {
    const int sx = static_cast<std::int16_t>(emu_->x86.R_BX);
    const int sy = static_cast<std::int16_t>(emu_->x86.R_CX);
    const int w = static_cast<std::int16_t>(emu_->x86.R_SI);
    const int h = static_cast<std::int16_t>(emu_->x86.R_DI);
    if (w > 0 && h > 0) {
      hd_pack_.on_vram_read_rect(sx, sy, w, h, read_grp_clip(), video_);
    }
    return 1;
  }

  // AH=0x0F (`BlitPlanar`, 3/4-plane byte-interleaved bitmap from ES:BP):
  if (ah == kGrpBlitPlanar && has_real_grp) {
    const int dx = static_cast<std::int16_t>(emu_->x86.R_BX);
    const int dy = static_cast<std::int16_t>(emu_->x86.R_CX);
    const int w = static_cast<std::int16_t>(emu_->x86.R_SI);
    const int h = static_cast<std::int16_t>(emu_->x86.R_DI);
    if (w > 0 && h > 0 && w <= VesaPlanarVideo::kWidth &&
        h <= VesaPlanarVideo::kHeight) {
      video_.clear_glyph_rect(dx, dy, w, h);
      if (hd_pack_.loaded_texture_count() > 0u ||
          hd_pack_.active_overlay_count() > 0u || hd_pack_.dumping_enabled()) {
        const int planes = std::clamp<int>(
            read_grp_u16(kGrpCsActivePlanesOffset), 3, 4);
        const std::uint16_t rop = read_grp_u16(kGrpCsPlanarRopOffset);
        const std::uint8_t pdis = guest_ram_[grp_lin + kGrpCsPlaneDisableOffset];
        const std::size_t row_bytes =
            static_cast<std::size_t>((w + 7) / 8) *
            static_cast<std::size_t>(planes);
        const std::size_t total_bytes = row_bytes * static_cast<std::size_t>(h);
        const std::uint32_t src_lin =
            (static_cast<std::uint32_t>(emu_->x86.R_ES) << 4) + emu_->x86.R_BP;

        PendingPlaneRect op{};
        op.kind = PendingGrpKind::PlanarBlit0F;
        op.dst_x = dx;
        op.dst_y = dy;
        op.w = w;
        op.h = h;
        op.active_planes = planes;
        op.p3_fill_mode = emu_->x86.R_DL;
        op.p3_fill_byte = emu_->x86.R_DH;
        op.rop_mode = rop;
        op.plane_disable = pdis;
        op.clip = read_grp_clip();
        if (src_lin < guest_ram_.size() &&
            total_bytes <= guest_ram_.size() - src_lin) {
          op.src_bytes.assign(guest_ram_.data() + src_lin,
                              guest_ram_.data() + src_lin + total_bytes);
        }
        pending_plane_rect_ = std::move(op);
      }
    }
    return 1;
  }

  // AH=0x13 (`BlitMono`, 1-bit monochrome glyph / Gaiji / UI pattern from ES:BP):
  if (ah == kGrpBlitMono && has_real_grp) {
    const int dx = static_cast<std::int16_t>(emu_->x86.R_BX);
    const int dy = static_cast<std::int16_t>(emu_->x86.R_CX);
    const int w = static_cast<std::int16_t>(emu_->x86.R_SI);
    const int h = static_cast<std::int16_t>(emu_->x86.R_DI);
    if (w > 0 && h > 0 && w <= VesaPlanarVideo::kWidth &&
        h <= VesaPlanarVideo::kHeight) {
      if (w <= 16 && h <= 16) {
        video_.mark_glyph_rect(dx, dy, w, h);
      } else {
        video_.clear_glyph_rect(dx, dy, w, h);
      }
      if (hd_pack_.loaded_texture_count() > 0u ||
          hd_pack_.active_overlay_count() > 0u) {
        const std::uint16_t mrop = read_grp_u16(kGrpCsMonoRopOffset);
        const std::uint8_t fg =
            static_cast<std::uint8_t>(read_grp_u16(kGrpCsMonoFgSlotOffset) & 0x0Fu);
        const std::uint8_t bg =
            static_cast<std::uint8_t>(read_grp_u16(kGrpCsMonoBgSlotOffset) & 0x0Fu);
        const std::uint8_t pdis = guest_ram_[grp_lin + kGrpCsPlaneDisableOffset];
        const std::size_t total_bytes =
            static_cast<std::size_t>((w + 7) / 8) * static_cast<std::size_t>(h);
        const std::uint32_t src_lin =
            (static_cast<std::uint32_t>(emu_->x86.R_ES) << 4) + emu_->x86.R_BP;

        PendingPlaneRect op{};
        op.kind = PendingGrpKind::MonoBlit13;
        op.dst_x = dx;
        op.dst_y = dy;
        op.w = w;
        op.h = h;
        op.rop_mode = mrop;
        op.fg_slot = fg;
        op.bg_slot = bg;
        op.plane_disable = pdis;
        op.clip = read_grp_clip();
        if (src_lin < guest_ram_.size() &&
            total_bytes <= guest_ram_.size() - src_lin) {
          op.src_bytes.assign(guest_ram_.data() + src_lin,
                              guest_ram_.data() + src_lin + total_bytes);
        }
        pending_plane_rect_ = std::move(op);
      }
    }
    return 1;
  }

  // Synthetic fallback when no GRP600C TSR is loaded (`dos_.grp600c_cs() == 0`):
  if ((ah == kGrpBlitPlanar || ah == kGrpBlitMono) && !has_real_grp) {
    const int dx = static_cast<std::int16_t>(emu_->x86.R_BX);
    const int dy = static_cast<std::int16_t>(emu_->x86.R_CX);
    const int w = static_cast<std::int16_t>(emu_->x86.R_SI);
    const int h = static_cast<std::int16_t>(emu_->x86.R_DI);
    if (w > 0 && w <= 16 && h > 0 && h <= 16) {
      video_.mark_glyph_rect(dx, dy, w, h);
    } else if (w > 0 && h > 0) {
      video_.clear_glyph_rect(dx, dy, w, h);
      if (hd_pack_.loaded_texture_count() > 0u || hd_pack_.dumping_enabled()) {
        if (pending_plane_rect_.kind == PendingGrpKind::LegacyMultiPlane &&
            pending_plane_rect_.dst_x == dx &&
            pending_plane_rect_.dst_y == dy && pending_plane_rect_.w == w &&
            pending_plane_rect_.h == h) {
          ++pending_plane_rect_.passes;
        } else {
          flush_pending_plane_blit();
          PendingPlaneRect op{};
          op.kind = PendingGrpKind::LegacyMultiPlane;
          op.dst_x = dx;
          op.dst_y = dy;
          op.w = w;
          op.h = h;
          op.passes = 1;
          pending_plane_rect_ = std::move(op);
        }
      }
    }
    return 1;
  }

  // AH=0x17 (`CopyRect`, VRAM -> VRAM):
  if (ah == kGrpCopyRect) {
    const int src_x = static_cast<std::int16_t>(emu_->x86.R_BX);
    const int src_y = static_cast<std::int16_t>(emu_->x86.R_CX);
    const int w = static_cast<std::int16_t>(emu_->x86.R_DX);
    const int h = static_cast<std::int16_t>(emu_->x86.R_SI);
    const int dst_x = static_cast<std::int16_t>(emu_->x86.R_DI);
    const int dst_y = static_cast<std::int16_t>(emu_->x86.R_BP);
    video_.clear_glyph_rect(dst_x, dst_y, w, h);
    if (hd_pack_.active_overlay_count() > 0u) {
      PendingPlaneRect op{};
      op.kind = PendingGrpKind::CopyRect17;
      op.src_x = src_x;
      op.src_y = src_y;
      op.w = w;
      op.h = h;
      op.dst_x = dst_x;
      op.dst_y = dst_y;
      pending_plane_rect_ = std::move(op);
    }
    return 1;
  }

  // AH=0x18 (`SwapPlanarRect`, VRAM <-> ES:DI):
  if (ah == kGrpSwapPlanarRect) {
    const int dx = static_cast<std::int16_t>(emu_->x86.R_BX);
    const int dy = static_cast<std::int16_t>(emu_->x86.R_CX);
    const int w = static_cast<std::int16_t>(emu_->x86.R_DX);
    const int h = static_cast<std::int16_t>(emu_->x86.R_SI);
    video_.clear_glyph_rect(dx, dy, w, h);
    if (w > 0 && h > 0 &&
        (hd_pack_.loaded_texture_count() > 0u ||
         hd_pack_.active_overlay_count() > 0u)) {
      const GrpClipRect clip = read_grp_clip();
      PendingPlaneRect op{};
      op.kind = PendingGrpKind::SwapRect18;
      op.dst_x = dx;
      op.dst_y = dy;
      op.w = w;
      op.h = h;
      op.clip = clip;
      op.pre_swap_patch =
          hd_pack_.capture_patch_snapshot(dx, dy, w, h, clip, video_);
      pending_plane_rect_ = std::move(op);
    }
    return 1;
  }

  // AH=0x19 (`BlitNpk`, NPK016 compressed 4-plane CG blit, w=DX, h=SI, ES:DI):
  if (ah == kGrpBlitNpk) {
    const int dx = static_cast<std::int16_t>(emu_->x86.R_BX);
    const int dy = static_cast<std::int16_t>(emu_->x86.R_CX);
    const int w = static_cast<std::int16_t>(emu_->x86.R_DX);
    const int h = static_cast<std::int16_t>(emu_->x86.R_SI);
    video_.clear_glyph_rect(dx, dy, w, h);
    if (w > 0 && h > 0 && w <= VesaPlanarVideo::kWidth &&
        h <= VesaPlanarVideo::kHeight &&
        (hd_pack_.loaded_texture_count() > 0u ||
         hd_pack_.active_overlay_count() > 0u || hd_pack_.dumping_enabled())) {
      const GrpClipRect clip = read_grp_clip();
      PendingPlaneRect op{};
      op.kind = PendingGrpKind::BlitNpk19;
      op.dst_x = dx;
      op.dst_y = dy;
      op.w = w;
      op.h = h;
      op.clip = clip;

      const bool unclipped =
          (dx >= 0 && dy >= 0 && dx >= clip.x0 && dy >= clip.y0 &&
           dx + w <= VesaPlanarVideo::kWidth &&
           dy + h <= VesaPlanarVideo::kHeight && dx + w - 1 <= clip.x1 &&
           dy + h - 1 <= clip.y1);
      if (!unclipped) {
        const std::uint32_t es_di =
            (static_cast<std::uint32_t>(emu_->x86.R_ES) << 4) + emu_->x86.R_DI;
        if (es_di < guest_ram_.size()) {
          const std::size_t max_copy =
              std::min<std::size_t>(65536u, guest_ram_.size() - es_di);
          op.src_bytes.assign(guest_ram_.data() + es_di,
                              guest_ram_.data() + es_di + max_copy);
        }
      }
      pending_plane_rect_ = std::move(op);
    }
    return 1;
  }

  if (ah == kGrpSmoothFade && has_real_grp) {
    // GRP600C CS:0x2B35 palette_fade:
    // AL=start_idx, CL=count, CH=steps, DX=vsyncs_per_step, ES:BX=dst_rgb4
    const std::uint8_t start_idx = emu_->x86.R_AL;
    const std::uint8_t count = emu_->x86.R_CL;
    const std::uint8_t steps = std::max<std::uint8_t>(1u, emu_->x86.R_CH);
    const std::uint16_t vsyncs = emu_->x86.R_DX;
    const std::uint32_t grp_lin = static_cast<std::uint32_t>(grp_cs) << 4;
    const std::uint32_t dst_lin =
        (static_cast<std::uint32_t>(emu_->x86.R_ES) << 4) + emu_->x86.R_BX;

    if (start_idx < 16u && count > 0u && start_idx + count <= 16u &&
        grp_lin + kGrpMinSegSpan <= guest_ram_.size() &&
        dst_lin + count * 3u <= guest_ram_.size() &&
        ss_sp + 4u <= guest_ram_.size()) {
      std::array<std::uint8_t, 48> src_cache{};
      std::memcpy(src_cache.data(),
                  guest_ram_.data() + grp_lin + kGrpPalCacheAOff, 48u);
      const std::uint8_t* dst_rgb = guest_ram_.data() + dst_lin;
      const std::uint8_t* slot_map =
          guest_ram_.data() + grp_lin + kGrpSlotMapOff;

      // Update GRP600C's RAM palette cache (`CS:0x198D` and `CS:0x19CF`)
      std::memcpy(
          guest_ram_.data() + grp_lin + kGrpPalCacheAOff + start_idx * 3u,
          dst_rgb, count * 3u);
      std::memcpy(
          guest_ram_.data() + grp_lin + kGrpPalCacheBOff + start_idx * 3u,
          dst_rgb, count * 3u);

      const double duration =
          (static_cast<double>(steps) * static_cast<double>(vsyncs)) /
          (60.0 * static_cast<double>(effective_speed_multiplier()));
      video_.begin_smooth_fade(start_idx, count, src_cache.data(), dst_rgb,
                               slot_map, duration);

      // Pop caller's far return address and hold mainline in F000:0F30
      // (`sti; jmp $`) while 24-bit fade & 291.3 Hz FM music run live!
      fade_resume_ip_ =
          static_cast<std::uint16_t>(x86emu_read_word(emu_, ss_sp + 0u));
      fade_resume_cs_ =
          static_cast<std::uint16_t>(x86emu_read_word(emu_, ss_sp + 2u));
      emu_->x86.R_SP = static_cast<std::uint16_t>(emu_->x86.R_SP + 4u);

      if (video_.is_fading()) {
        holding_in_fade_rom_ = true;
        x86emu_set_seg_register(emu_, emu_->x86.R_CS_SEL, kBiosRomSeg);
        emu_->x86.R_IP = kRomFadeWaitOff;
        yield_burst_now_ = true;
        x86emu_stop(emu_);
      } else {
        x86emu_set_seg_register(emu_, emu_->x86.R_CS_SEL, fade_resume_cs_);
        emu_->x86.R_IP = fade_resume_ip_;
      }
      return 1;
    }
  }
  return 1;
}

int X86Machine::on_interrupt(std::uint8_t num, unsigned type) {
  if ((type & 0xFFu) == INTR_TYPE_FAULT && num != 0x00u) {
    if (std::getenv("SAN4_TRACE") != nullptr) {
      std::cerr << "[x86-fault] num=0x" << std::hex << static_cast<int>(num)
                << " type=0x" << type << " CS:IP=" << emu_->x86.R_CS << ":"
                << emu_->x86.R_IP << " SS:SP=" << emu_->x86.R_SS << ":"
                << emu_->x86.R_SP << std::dec << "\n";
    }
    return 1;
  }

  switch (num) {
    case 0x10u: {
      std::uint16_t ax = emu_->x86.R_AX;
      std::uint16_t bx = emu_->x86.R_BX;
      std::uint16_t cx = emu_->x86.R_CX;
      std::uint16_t dx = emu_->x86.R_DX;
      std::uint16_t es = emu_->x86.R_ES;
      std::uint16_t bp = emu_->x86.R_BP;
      const std::uint16_t di = emu_->x86.R_DI;
      video_.handle_int10(ax, bx, cx, dx, es, bp, di, guest_ram_, kBiosRomSeg,
                          kRomFont8x16Off, kRomVbeModesOff, kRomVbeOemOff,
                          emu_);
      if (!video_.is_fading()) {
        release_fade_rom_hold_if_needed();
      }
      emu_->x86.R_AX = ax;
      emu_->x86.R_BX = bx;
      emu_->x86.R_CX = cx;
      emu_->x86.R_DX = dx;
      if (es != emu_->x86.R_ES) {
        x86emu_set_seg_register(emu_, emu_->x86.R_ES_SEL, es);
      }
      emu_->x86.R_BP = bp;
      return 1;
    }

    case 0x11u:  // BIOS Equipment List (VGA + Math coprocessor + Mouse)
      emu_->x86.R_AX = 0x0026u;
      return 1;

    case 0x12u:  // Conventional Memory Size (640 KB)
      emu_->x86.R_AX = 640u;
      return 1;

    case 0x13u:  // Stubbed diskette check (CF=0, AH=0)
      emu_->x86.R_AH = 0x00u;
      emu_->x86.R_EFLG &= ~static_cast<unsigned>(FB_CF);
      return 1;

    case 0x15u:  // Reject DOS/V AX=5000h & system services cleanly (CF=1, AH=86h)
      emu_->x86.R_AH = 0x86u;
      emu_->x86.R_EFLG |= FB_CF;
      return 1;

    case 0x16u: {
      bool was_poll = false;
      dos_.handle_int16(emu_, was_poll);
      if (was_poll) {
        record_idle_poll(true);
      } else {
        consecutive_idle_polls_ = 0;
      }
      return 1;
    }

    case 0x1Au: {  // BIOS Time-of-Day
      if (emu_->x86.R_AH == 0x00u) {
        const std::uint16_t lo =
            static_cast<std::uint16_t>(guest_ram_[0x046C]) |
            (static_cast<std::uint16_t>(guest_ram_[0x046D]) << 8);
        const std::uint16_t hi =
            static_cast<std::uint16_t>(guest_ram_[0x046E]) |
            (static_cast<std::uint16_t>(guest_ram_[0x046F]) << 8);
        emu_->x86.R_DX = lo;
        emu_->x86.R_CX = hi;
        emu_->x86.R_AL = 0u;
      }
      emu_->x86.R_EFLG &= ~static_cast<unsigned>(FB_CF);
      return 1;
    }

    case 0x20u:
      dos_.handle_int20(emu_);
      return 1;

    case 0x21u:
      consecutive_idle_polls_ = 0;
      dos_.handle_int21(emu_);
      return 1;

    case 0x33u: {
      bool was_poll = false;
      mouse_.handle_int33(emu_, was_poll);
      if (was_poll) {
        record_idle_poll(true);
      }
      return 1;
    }

    case 0x66u:
      return handle_fmdrv_timers();

    case 0x67u:
      ems_.handle_int67(emu_);
      return 1;

    case 0xF1u:
      return handle_grp600c_dispatch();

    // Vector guest-hooked interrupts through guest IVT (0000:num*4):
    // 08h (FMDRV PIT), 1Ch (MAIN timer), 18h, 23h, 24h, 3Fh (MSC MOVE), 65h (SAN486 IPC)
    default:
      return 0;
  }
}

}  // namespace san4
