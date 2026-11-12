#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <SDL2/SDL.h>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "audio.h"
#include "dos/dos_kernel.h"
#include "dos/ems.h"
#include "dos/mouse.h"
#include "dos/vfs.h"
#include "emu_session.h"
#include "frame_mailbox.h"
#include "frame_profiler.h"
#include "game_manifest.h"
#include "grp600c_defs.h"
#include "grp_census.h"
#include "hd_gen/hd_art_upscaler.h"
#include "hd_gen/hd_pack_generator.h"
#include "hd_gen/san4_codecs.h"
#include "hd_pack.h"
#include "host_channel.h"
#include "machine.h"
#include "reference/hd_art_upscaler_ref.h"
#include "reference/hd_composite_ref.h"
#include "present_tracker.h"
#include "render_policy.h"
#include "runtime_cli.h"
#include "sdl_host.h"
#include "upscaler.h"
#include "util/simd_blend.h"
#include "util/string_util.h"
#include "util/thread_pool.h"
#include "util/win32_platform.h"
#include "video.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " (" << #cond \
                << ")\n";                                                    \
      ++g_failures;                                                          \
      return;                                                                \
    }                                                                        \
  } while (0)

class ScopedTempDir {
 public:
  ScopedTempDir() {
    static std::atomic<std::uint64_t> seq{0};
    auto base = std::filesystem::temp_directory_path();
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
#ifdef _WIN32
    const auto pid = static_cast<unsigned long>(::_getpid());
#else
    const auto pid = static_cast<unsigned long>(::getpid());
#endif
    const auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
    const auto idx = seq.fetch_add(1u, std::memory_order_relaxed);
    path_ = base / ("san4_unit_" + std::to_string(pid) + "_" +
                    std::to_string(tid) + "_" + std::to_string(stamp) + "_" +
                    std::to_string(idx));
    std::filesystem::create_directories(path_);
  }
  ~ScopedTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

void test_sha256_and_grp600c_signature() {
  const std::array<std::uint8_t, 0> empty{};
  CHECK(san4::sha256_hex(empty) ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  const std::array<std::uint8_t, 3> abc{'a', 'b', 'c'};
  CHECK(san4::sha256_hex(abc) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  CHECK(san4::immutable_game_manifest().size() == 43u);

  std::vector<std::uint8_t> fake_grp(64, 0u);
  fake_grp[0x18] = 0xE9u;
  fake_grp[0x19] = 0x14u;
  fake_grp[0x1A] = 0x23u;
  fake_grp[0x1B] = 0x90u;
  fake_grp[0x1C] = 0x90u;
  CHECK(san4::has_expected_grp600c_dispatch_signature(fake_grp));
  fake_grp[0x1A] = 0x24u;
  CHECK(!san4::has_expected_grp600c_dispatch_signature(fake_grp));

  ScopedTempDir tmp;
  const auto res =
      san4::resolve_and_verify_game_dir(tmp.path().string(), tmp.path() / "bin");
  CHECK(!res.ok);
  CHECK(!res.errors.empty());

  // NBDATA.S4 is mutable custom-officer save data: exact 17,496-byte size is
  // accepted regardless of contents, while wrong byte size is rejected.
  {
    std::ofstream nb(tmp.path() / "NBDATA.S4", std::ios::binary);
    std::vector<char> custom_nb(17496u, '\x5A');
    nb.write(custom_nb.data(), static_cast<std::streamsize>(custom_nb.size()));
  }
  const auto res_nb_ok =
      san4::resolve_and_verify_game_dir(tmp.path().string(), tmp.path() / "bin");
  for (const auto& err : res_nb_ok.errors) {
    CHECK(err.find("NBDATA.S4") == std::string::npos);
  }
  {
    std::ofstream nb_bad(tmp.path() / "NBDATA.S4",
                         std::ios::binary | std::ios::trunc);
    nb_bad.write("short", 5);
  }
  const auto res_nb_bad =
      san4::resolve_and_verify_game_dir(tmp.path().string(), tmp.path() / "bin");
  bool saw_nb_size_err = false;
  for (const auto& err : res_nb_bad.errors) {
    if (err.find("Size mismatch on NBDATA.S4") != std::string::npos) {
      saw_nb_size_err = true;
    }
  }
  CHECK(saw_nb_size_err);
}

void test_vga_planar_16bit_out_and_write_modes() {
  san4::X86Machine m;

  // Synthetic 16-bit x86 snippet exercising:
  // 1) INT 10h AX=4F02h BX=0102h (800x600 4-plane mode)
  // 2) 16-bit OUT DX, AX on 0x3C4 (Map Mask = 0x05 -> planes 0 & 2 -> color 5)
  //    write 0xFF to A000:0000 -> pixels 0..7 = 5
  // 3) 16-bit OUT DX, AX on 0x3CE Write Mode 2 (reg 5 = 0x02), Bit Mask (reg 8 = 0xAA),
  //    Map Mask = 0x0F: RMW read AL=[A000:0000], write [A000:0000]=0x0C ->
  //    even pixels (0,2,4,6 where mask bit 0x80>>x is 1) become 12, odd stay 5!
  // 4) INT 10h AX=1130h BH=06h -> ES:BP points to 8x16 ROM font in 0xF000.
  static constexpr std::uint8_t kCode[] = {
      0xB8, 0x02, 0x4F,              // mov ax, 0x4F02
      0xBB, 0x02, 0x01,              // mov bx, 0x0102
      0xCD, 0x10,                    // int 0x10
      0xB8, 0x00, 0xA0,              // mov ax, 0xA000
      0x8E, 0xC0,                    // mov es, ax
      0xBA, 0xC4, 0x03,              // mov dx, 0x03C4
      0xB8, 0x02, 0x05,              // mov ax, 0x0502 (idx=2 MapMask, val=0x05)
      0xEF,                          // out dx, ax (16-bit port write!)
      0x26, 0xC6, 0x06, 0x00, 0x00, 0xFF,  // mov byte ptr es:[0x0000], 0xFF
      0xB8, 0x02, 0x0F,              // mov ax, 0x0F02 (MapMask = 0x0F)
      0xEF,                          // out dx, ax
      0xBA, 0xCE, 0x03,              // mov dx, 0x03CE
      0xB8, 0x05, 0x02,              // mov ax, 0x0205 (GC Mode = Write Mode 2)
      0xEF,                          // out dx, ax
      0xB8, 0x08, 0xAA,              // mov ax, 0xAA08 (BitMask = 10101010b)
      0xEF,                          // out dx, ax
      0x26, 0xA0, 0x00, 0x00,        // mov al, es:[0x0000] (fill latches!)
      0x26, 0xC6, 0x06, 0x00, 0x00, 0x0C,  // mov byte ptr es:[0x0000], 12
      // Also test Write Mode 3: GC Mode = 3, Set/Reset = 9, BitMask = 0xFF, CPU byte = 0x80 (pixel 0 -> 9)
      0xB8, 0x00, 0x09,              // mov ax, 0x0900 (Set/Reset = 9)
      0xEF,                          // out dx, ax
      0xB8, 0x05, 0x03,              // mov ax, 0x0305 (GC Mode = Write Mode 3)
      0xEF,                          // out dx, ax
      0xB8, 0x08, 0xFF,              // mov ax, 0xFF08 (BitMask = 0xFF)
      0xEF,                          // out dx, ax
      0x26, 0xA0, 0x01, 0x00,        // mov al, es:[0x0001] (latch byte 1)
      0x26, 0xC6, 0x06, 0x01, 0x00, 0x80,  // mov byte ptr es:[0x0001], 0x80 (pixel 8 = 9)
      // Query 8x16 ROM font pointer
      0xB8, 0x30, 0x11,              // mov ax, 0x1130
      0xBB, 0x00, 0x06,              // mov bx, 0x0600
      0xCD, 0x10,                    // int 0x10
      0xEB, 0xFE,                    // jmp $
  };

  m.load_synthetic_code(0x2000u, 0x0100u, kCode);
  m.step_instructions(64);
  m.video().latch_frame();

  CHECK(m.video().current_mode() == 0x0102u);
  const auto& vram = m.video().latched_vram();
  for (int x = 0; x < 8; ++x) {
    const std::uint8_t expected = ((x % 2) == 0) ? 12u : 5u;
    CHECK(vram[x] == expected);
  }
  CHECK(vram[8] == 9u);
  CHECK(vram[9] == 0u);
  CHECK(m.emu()->x86.R_ES == 0xF000u);
  CHECK(m.emu()->x86.R_CX == 16u);
}

void test_ems_zero_copy_int67() {
  san4::X86Machine m;

  // Verify "EMMXXXX0" signature at IVT[0x67].seg : 0x000A
  const std::uint16_t ems_seg =
      static_cast<std::uint16_t>(m.guest_ram()[0x67 * 4 + 2]) |
      (static_cast<std::uint16_t>(m.guest_ram()[0x67 * 4 + 3]) << 8);
  CHECK(ems_seg == 0xE000u);
  CHECK(std::memcmp(m.guest_ram() + (0xE000u << 4) + 0x0Au, "EMMXXXX0", 8) == 0);

  // Synthetic x86 test: allocate 2 EMS pages, map page 0 -> phys 0, write 0xBEEF,
  // map page 1 -> phys 0, write 0xCAFE, remap page 0 -> phys 0, read AX into CX!
  static constexpr std::uint8_t kCode[] = {
      0xB4, 0x41,                    // mov ah, 0x41 (Get Page Frame)
      0xCD, 0x67,                    // int 0x67 -> BX = 0xD000
      0x8E, 0xC3,                    // mov es, bx
      0xB4, 0x43,                    // mov ah, 0x43 (Alloc Pages)
      0xBB, 0x02, 0x00,              // mov bx, 2
      0xCD, 0x67,                    // int 0x67 -> DX = handle
      0xB8, 0x00, 0x44,              // mov ax, 0x4400 (phys slot 0)
      0xBB, 0x00, 0x00,              // mov bx, 0 (logical page 0)
      0xCD, 0x67,                    // int 0x67
      0x26, 0xC7, 0x06, 0x00, 0x00, 0xEF, 0xBE,  // mov word es:[0], 0xBEEF
      0xB8, 0x00, 0x44,              // mov ax, 0x4400 (phys slot 0)
      0xBB, 0x01, 0x00,              // mov bx, 1 (logical page 1)
      0xCD, 0x67,                    // int 0x67
      0x26, 0xC7, 0x06, 0x00, 0x00, 0xFE, 0xCA,  // mov word es:[0], 0xCAFE
      0xB8, 0x00, 0x44,              // mov ax, 0x4400 (phys slot 0)
      0xBB, 0x00, 0x00,              // mov bx, 0 (logical page 0 back!)
      0xCD, 0x67,                    // int 0x67
      0x26, 0x8B, 0x0E, 0x00, 0x00,  // mov cx, es:[0]
      0xEB, 0xFE,                    // jmp $
  };

  m.load_synthetic_code(0x2000u, 0x0100u, kCode);
  m.step_instructions(48);
  CHECK(m.emu()->x86.R_CX == 0xBEEFu);
}

void test_mouse_hle_thunk_coords_and_tap_latch() {
  san4::X86Machine m;
  int warped_x = -1;
  int warped_y = -1;
  m.mouse().set_warp_callback([&](int x, int y) {
    warped_x = x;
    warped_y = y;
  });

  // Synthetic code that:
  // 1) Resets mouse (AX=0), attempts legacy 640x480 clamp (AX=7 CX=0 DX=639, AX=8 CX=0 DX=479)
  // 2) Warps to (512, 384) via AX=4, then host moves to (799, 599) and performs a sub-frame tap!
  // 3) Calls saved IVT[0x33] via `pushf; lcall [0000:0xCC]` (just like GRP600C!) for AX=3!
  static constexpr std::uint8_t kCode[] = {
      0x31, 0xC0,                    // xor ax, ax
      0xCD, 0x33,                    // int 0x33 (Reset)
      0xB8, 0x07, 0x00,              // mov ax, 7
      0x31, 0xC9,                    // xor cx, cx
      0xBA, 0x7F, 0x02,              // mov dx, 639 (legacy clamp ignored!)
      0xCD, 0x33,
      0xB8, 0x04, 0x00,              // mov ax, 4 (Set pos)
      0xB9, 0x00, 0x02,              // mov cx, 512
      0xBA, 0x80, 0x01,              // mov dx, 384
      0xCD, 0x33,
      0x90,                          // nop (breakpoint 1)
      // Far-call boot IVT[0x33] (`pushf; lcall 0:[0x00CC]`) three times
      0x31, 0xC0,                    // xor ax, ax
      0x8E, 0xD8,                    // mov ds, ax
      0xB8, 0x03, 0x00,              // mov ax, 3
      0x9C,                          // pushf
      0xFF, 0x1E, 0xCC, 0x00,        // lcall [0x00CC] -> poll 1
      0x89, 0xDE,                    // mov si, bx
      0xB8, 0x03, 0x00,              // mov ax, 3
      0x9C,                          // pushf
      0xFF, 0x1E, 0xCC, 0x00,        // lcall [0x00CC] -> poll 2
      0x89, 0xDF,                    // mov di, bx
      0xB8, 0x03, 0x00,              // mov ax, 3
      0x9C,                          // pushf
      0xFF, 0x1E, 0xCC, 0x00,        // lcall [0x00CC] -> poll 3 (latch cleared!)
      0xEB, 0xFE,                    // jmp $
  };

  m.load_synthetic_code(0x2000u, 0x0100u, kCode);
  m.step_instructions(11);  // Up through AX=4 warp + nop
  CHECK(warped_x == 512 && warped_y == 384);

  // Move to bottom-right of 800x600 (beyond 640x480!) and tap (press + release)
  m.mouse().set_position(799, 599);
  m.mouse().set_button(0, true);
  m.mouse().set_button(0, false);

  m.step_instructions(64);
  CHECK(m.emu()->x86.R_CX == 799u);
  CHECK(m.emu()->x86.R_DX == 599u);
  CHECK(m.emu()->x86.R_SI == 1u);  // Poll 1 saw tap!
  CHECK(m.emu()->x86.R_DI == 1u);  // Poll 2 saw tap!
  CHECK(m.emu()->x86.R_BX == 0u);  // Poll 3 auto-released!
}

void test_pit_irq0_cli_gating_and_speed_timers() {
  san4::X86Machine m;

  // Install a custom INT 1Ch handler at 3000:0000 that increments word 0:[0x0500]
  // `push ds; push ax; xor ax, ax; mov ds, ax; inc word [0x0500]; pop ax; pop ds; iret`
  static constexpr std::uint8_t kInt1C[] = {
      0x1E, 0x50, 0x31, 0xC0, 0x8E, 0xD8, 0xFF, 0x06, 0x00, 0x05, 0x58, 0x1F, 0xCF,
  };
  std::memcpy(m.guest_ram() + (0x3000u << 4), kInt1C, sizeof(kInt1C));
  m.guest_ram()[0x1C * 4 + 0] = 0x00u;
  m.guest_ram()[0x1C * 4 + 1] = 0x00u;
  m.guest_ram()[0x1C * 4 + 2] = 0x00u;
  m.guest_ram()[0x1C * 4 + 3] = 0x30u;

  // Main code programs PIT ch0 to 0x1000 (291.304 Hz), sets INT 66h AH=F0h BX=60,
  // and spins in STI idle poll loop (`mov ax, 0xF000; int 0x66; jmp -7`).
  static constexpr std::uint8_t kMain[] = {
      0xB0, 0x36,        // mov al, 0x36
      0xE6, 0x43,        // out 0x43, al
      0xB0, 0x00,        // mov al, 0x00
      0xE6, 0x40,        // out 0x40, al
      0xB0, 0x10,        // mov al, 0x10 (divisor = 0x1000 -> 291.304 Hz)
      0xE6, 0x40,        // out 0x40, al
      0xB8, 0x01, 0xF0,  // mov ax, 0xF001 (set 1/60s timer F0)
      0xBB, 0x3C, 0x00,  // mov bx, 60 (1.0 sec)
      0xCD, 0x66,        // int 0x66
      // Poll loop at offset 0x11:
      0xFB,              // sti
      0xB8, 0x00, 0xF0,  // mov ax, 0xF000 (read timer F0)
      0xCD, 0x66,        // int 0x66 -> BX = remaining ticks
      0xEB, 0xF8,        // jmp -8
  };

  m.load_synthetic_code(0x2000u, 0x0100u, kMain);
  m.step_instructions(8);  // Run init up to poll loop (offset 0x14)

  // First test CLI deferral: patch `sti` at 2000:0114 -> `cli` (0xFA), clear IF
  const std::uint32_t sti_lin = (0x2000u << 4) + 0x0100u + 20u;
  m.guest_ram()[sti_lin] = 0xFAu;
  m.emu()->x86.R_EFLG &= ~static_cast<unsigned>(FB_IF);
  m.run_frame(1.0 / 60.0);
  const std::uint16_t ticks_under_cli =
      static_cast<std::uint16_t>(m.guest_ram()[0x0500]) |
      (static_cast<std::uint16_t>(m.guest_ram()[0x0501]) << 8);
  CHECK(ticks_under_cli == 0u);

  // Restore `sti` and run one 1x frame: ~4-5 PIT ticks delivered, timer F0 drops by 1!
  m.guest_ram()[sti_lin] = 0xFBu;
  m.emu()->x86.R_EFLG |= FB_IF;
  const std::uint16_t rem_before = m.emu()->x86.R_BX;
  m.run_frame(1.0 / 60.0);
  const std::uint16_t ticks_after_sti =
      static_cast<std::uint16_t>(m.guest_ram()[0x0500]) |
      (static_cast<std::uint16_t>(m.guest_ram()[0x0501]) << 8);
  CHECK(ticks_after_sti >= 4u);
  const std::uint16_t rem_after_1x = m.emu()->x86.R_BX;
  CHECK(rem_before >= rem_after_1x + 1u);

  // Switch to 16x Turbo (`Tab` held): one 1/60s frame drops timer F0 by 16 ticks,
  // while PIT IRQ 0 still advances by only ~4-5 real-time ticks (music stays 1.0x!).
  m.set_turbo_held(true);
  m.run_frame(1.0 / 60.0);
  const std::uint16_t ticks_after_turbo =
      static_cast<std::uint16_t>(m.guest_ram()[0x0500]) |
      (static_cast<std::uint16_t>(m.guest_ram()[0x0501]) << 8);
  const std::uint16_t pit_delta = ticks_after_turbo - ticks_after_sti;
  CHECK(pit_delta >= 4u && pit_delta <= 6u);
  CHECK(m.emu()->x86.R_BX + 16u <= rem_after_1x);
}

void test_dos_kernel_mcb_exec_and_cjk_env() {
  ScopedTempDir tmp;

  // Write synthetic CHILD.COM:
  // Checks INT 21h AX=3800h (BX==1), checks MS$KANJI open fails (CF=1),
  // checks environment at PSP:[0x2C] contains "C:\CHILD.COM", and exits AL=0x42.
  {
    const std::vector<std::uint8_t> child_com = {
        0x83, 0xEC, 0x40,                    // 0x00: sub sp, 64
        0x89, 0xE2,                          // 0x03: mov dx, sp
        0xB8, 0x00, 0x38,                    // 0x05: mov ax, 0x3800
        0xCD, 0x21,                          // 0x08: int 0x21
        0x83, 0xFB, 0x01,                    // 0x0A: cmp bx, 1
        0x75, 0x1B,                          // 0x0D: jne fail (0x2A)
        0xBA, 0x2F, 0x01,                    // 0x0F: mov dx, 0x012F ("MS$KANJI")
        0xB8, 0x00, 0x3D,                    // 0x12: mov ax, 0x3D00
        0xCD, 0x21,                          // 0x15: int 0x21
        0x73, 0x11,                          // 0x17: jnc fail (0x2A)
        0x8E, 0x06, 0x2C, 0x00,              // 0x19: mov es, [0x002C]
        0x26, 0x80, 0x3E, 0x23, 0x00, 'C',   // 0x1D: cmp byte es:[0x23], 'C' ("C:\CHILD.COM")
        0x75, 0x05,                          // 0x23: jne fail (0x2A)
        0xB8, 0x42, 0x4C,                    // 0x25: mov ax, 0x4C42
        0xCD, 0x21,                          // 0x28: int 0x21
        // 0x2A fail:
        0xB8, 0x99, 0x4C,                    // 0x2A: mov ax, 0x4C99
        0xCD, 0x21,                          // 0x2D: int 0x21
        // 0x2F -> CS:0x012F:
        'M',  'S',  '$',  'K',  'A',  'N',  'J',  'I',  0x00,
    };
    std::ofstream f(tmp.path() / "CHILD.COM", std::ios::binary);
    f.write(reinterpret_cast<const char*>(child_com.data()),
            static_cast<std::streamsize>(child_com.size()));
  }

  // Write synthetic PARENT.COM:
  // Shrinks own MCB to 0x100 paras (`AH=4Ah`), EXECs `CHILD.COM` (`AX=4B00h`),
  // queries child return code (`AH=4Dh`), and exits returning that code (`AH=4Ch`).
  {
    const std::vector<std::uint8_t> parent_com = {
        0xBB, 0x00, 0x01,                    // 0x00: mov bx, 0x0100
        0xB4, 0x4A,                          // 0x03: mov ah, 0x4A (Resize MCB)
        0xCD, 0x21,                          // 0x05: int 0x21
        0x72, 0x15,                          // 0x07: jc err (0x1E)
        0xBA, 0x28, 0x01,                    // 0x09: mov dx, 0x0128 ("CHILD.COM")
        0xBB, 0x38, 0x01,                    // 0x0C: mov bx, 0x0138 (param block)
        0xB8, 0x00, 0x4B,                    // 0x0F: mov ax, 0x4B00 (EXEC)
        0xCD, 0x21,                          // 0x12: int 0x21
        0x72, 0x08,                          // 0x14: jc err (0x1E)
        0xB4, 0x4D,                          // 0x16: mov ah, 0x4D (Get child rc)
        0xCD, 0x21,                          // 0x18: int 0x21 -> AL = 0x42
        0xB4, 0x4C,                          // 0x1A: mov ah, 0x4C
        0xCD, 0x21,                          // 0x1C: int 0x21
        // 0x1E err:
        0xB8, 0xEE, 0x4C,                    // 0x1E: mov ax, 0x4CEE
        0xCD, 0x21,                          // 0x21: int 0x21
        0x90, 0x90, 0x90, 0x90, 0x90,        // 0x23..0x27: pad to 0x28
        // 0x28 -> CS:0x0128: "CHILD.COM\0" (16 bytes -> 0x38)
        'C',  'H',  'I',  'L',  'D',  '.',  'C',  'O',  'M',  0, 0, 0, 0, 0, 0, 0,
        // 0x38 -> CS:0x0138: 14-byte EXEC param block
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    std::ofstream f(tmp.path() / "PARENT.COM", std::ios::binary);
    f.write(reinterpret_cast<const char*>(parent_com.data()),
            static_cast<std::streamsize>(parent_com.size()));
  }

  san4::MachineConfig cfg;
  cfg.game_dir = tmp.path();
  san4::X86Machine m(cfg);
  CHECK(m.boot("parent.com"));  // Case-insensitive lookup!
  m.step_instructions(500);
  CHECK(m.exited());
  CHECK(m.exit_code() == 0x42u);
}

void test_opl2_adlib_timer_and_fm_audio() {
  san4::Opl2Audio opl;

  auto write_reg = [&](std::uint8_t reg, std::uint8_t val) {
    opl.port_out8(0x388u, reg);
    opl.port_out8(0x389u, val);
  };

  // Standard AdLib timer presence sequence used by FMDRV.COM
  write_reg(0x04u, 0x60u);
  write_reg(0x04u, 0x80u);
  CHECK((opl.port_in8(0x388u) & 0xE0u) == 0x00u);

  write_reg(0x02u, 0xFFu);  // 1 tick = 80 us
  write_reg(0x04u, 0x21u);  // Start Timer 1
  for (int i = 0; i < 64; ++i) {
    (void)opl.port_in8(0x388u);  // 64 * 1.5 us = 96 us > 80 us
  }
  CHECK((opl.port_in8(0x388u) & 0xE0u) == 0xC0u);
  write_reg(0x04u, 0x60u);
  write_reg(0x04u, 0x80u);

  // Program a clean 2-operator sine/FM voice on Channel 0
  write_reg(0x01u, 0x20u);
  write_reg(0x20u, 0x01u);  // Modulator mult=1
  write_reg(0x40u, 0x10u);  // Modulator TL
  write_reg(0x60u, 0xF0u);  // Fast attack
  write_reg(0x80u, 0x77u);
  write_reg(0x23u, 0x01u);  // Carrier mult=1
  write_reg(0x43u, 0x00u);  // Carrier max volume (TL=0)
  write_reg(0x63u, 0xF0u);  // Fast attack
  write_reg(0x83u, 0x77u);
  write_reg(0xC0u, 0x00u);  // FM feedback/conn
  write_reg(0xA0u, 0x98u);  // F-Number low
  write_reg(0xB0u, 0x31u);  // Key-On + Block 4 + F-Num high

  opl.reset_peak_abs_sample();
  opl.advance_seconds(0.050);  // 50 ms (~2205 frames)

  CHECK(opl.total_rendered_frames() >= 2200u);
  CHECK(opl.peak_abs_sample() > 500);

  std::vector<std::int16_t> pcm(1024 * 2, 0);
  const std::uint32_t pulled = opl.pull_stereo_s16(pcm.data(), 1024u);
  CHECK(pulled == 1024u);
  bool any_nonzero = false;
  for (std::int16_t s : pcm) {
    if (s != 0) {
      any_nonzero = true;
      break;
    }
  }
  CHECK(any_nonzero);
}

void test_frozen_vram_smooth_crossfade() {
  san4::VesaPlanarVideo vid;
  vid.reset_mode(0x0102u);

  // Write pixel 0 = color 1 in live VRAM and latch initial frame
  vid.port_out8(0x3C4u, 0x02u);
  vid.port_out8(0x3C5u, 0x01u);
  vid.vram_write8(0u, 0x80u);
  vid.latch_frame();
  CHECK(vid.latched_vram()[0] == 1u);

  std::array<std::uint8_t, 48> src_black{};
  std::array<std::uint8_t, 48> dst_white{};
  dst_white.fill(0x0Fu);
  std::array<std::uint8_t, 16> slots{};
  for (std::uint8_t i = 0; i < 16u; ++i) slots[i] = i;

  vid.begin_smooth_fade(0u, 16u, src_black.data(), dst_white.data(),
                        slots.data(), 0.20);
  CHECK(vid.is_fading());

  // Mutate live VRAM mid-fade -> latched_vram must stay frozen on old scene!
  vid.port_out8(0x3C4u, 0x02u);
  vid.port_out8(0x3C5u, 0x02u);
  vid.vram_write8(0u, 0x80u);  // Live pixel 0 becomes color 3

  CHECK(vid.advance_fade(0.10));  // Halfway (t = 0.5)
  vid.latch_frame();
  CHECK(vid.latched_vram()[0] == 1u);  // Still frozen!
  const std::uint32_t mid_rgb = vid.latched_palette_argb()[1] & 0xFFu;
  CHECK(mid_rgb >= 110u && mid_rgb <= 145u);  // Smooth 24-bit midpoint!

  CHECK(!vid.advance_fade(0.15));  // Complete (t = 1.0)
  vid.latch_frame();
  CHECK(vid.latched_vram()[0] == 3u);  // Unfrozen once fade finishes!
  CHECK((vid.latched_palette_argb()[1] & 0xFFFFFFu) == 0xFFFFFFu);
}

void test_upscaler_dither_glyph_guards_and_perf() {
  std::vector<std::uint8_t> src(
      san4::VesaPlanarVideo::kWidth * san4::VesaPlanarVideo::kHeight, 0u);
  std::array<std::uint32_t, 16> pal{};
  pal[0] = 0xFF000000u;
  pal[1] = 0xFFFFFFFFu;
  pal[2] = 0xFFE04020u;
  std::array<std::uint8_t, san4::VesaPlanarVideo::kTileCount> glyphs{};

  // Region A (tile 0,0 -> x=0..15, y=0..15): 2x2 checkerboard dither of 0 and 1
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      src[y * 800 + x] = static_cast<std::uint8_t>((x + y) & 1);
    }
  }

  // Region B (tile 2,0 -> x=32..47, y=0..15): glyph-marked diagonal stroke
  glyphs[0 * san4::VesaPlanarVideo::kTilesX + 2] = 1u;
  for (int i = 0; i < 16; ++i) {
    src[i * 800 + (32 + i)] = 1u;
  }

  // Region C (tile 4,0 -> x=64..79, y=0..15): non-glyph solid diagonal triangle
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      if (x >= y) {
        src[y * 800 + (64 + x)] = 2u;
      }
    }
  }

  san4::PixelUpscaler up(4);
  up.set_mode(san4::UpscaleMode::Xbr4x);

  const auto t0 = std::chrono::steady_clock::now();
  up.upscale_full_buffer(src.data(), pal, &glyphs);
  const auto t1 = std::chrono::steady_clock::now();
  const double full_ms =
      std::chrono::duration<double, std::milli>(t1 - t0).count();
  std::cout << "[san4-bench] full 800x600 -> 3200x2400 Xbr4x: " << full_ms
            << " ms\n";

  const auto& out = up.output_argb();
  const int ow = up.output_width();
  CHECK(ow == 3200 && up.output_height() == 2400);

  // Verify Region A (checkerboard dither at (4,4)=0 and (5,4)=1): all 4x4 subpixels crisp!
  for (int dy = 0; dy < 4; ++dy) {
    for (int dx = 0; dx < 4; ++dx) {
      CHECK(out[(4 * 4 + dy) * ow + (4 * 4 + dx)] == pal[0]);
      CHECK(out[(4 * 4 + dy) * ow + (5 * 4 + dx)] == pal[1]);
    }
  }

  // Verify Region B (glyph diagonal at x=36, y=4 -> white): all 4x4 subpixels crisp!
  for (int dy = 0; dy < 4; ++dy) {
    for (int dx = 0; dx < 4; ++dx) {
      CHECK(out[(4 * 4 + dy) * ow + (36 * 4 + dx)] == pal[1]);
    }
  }

  // Verify Region C (sprite diagonal edge at x=68, y=4): xBR interpolates corner!
  bool saw_interpolated_edge = false;
  for (int y = 2 * 4; y < 8 * 4; ++y) {
    for (int x = 66 * 4; x < 72 * 4; ++x) {
      const std::uint32_t px = out[y * ow + x];
      if (px != pal[0] && px != pal[2]) {
        saw_interpolated_edge = true;
      }
    }
  }
  CHECK(saw_interpolated_edge);
}

void test_hd_pack_png_and_occlusion_mask() {
  ScopedTempDir tmp;
  san4::VesaPlanarVideo vid;
  vid.reset_mode(0x0102u);

  // Blit a synthetic 16x16 4-bit sprite (checker of color 3 and 4) at (16, 16)
  vid.port_out8(0x3CEu, 0x05u);
  vid.port_out8(0x3CFu, 0x02u);  // Write Mode 2
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      const int px = 16 + x;
      const int py = 16 + y;
      const std::uint32_t off =
          static_cast<std::uint32_t>(py * 100 + (px >> 3));
      const std::uint8_t mask = static_cast<std::uint8_t>(0x80u >> (px & 7));
      vid.port_out8(0x3CEu, 0x08u);
      vid.port_out8(0x3CFu, mask);
      (void)vid.vram_read8(off);
      vid.vram_write8(off, static_cast<std::uint8_t>(((x + y) & 1) ? 4u : 3u));
    }
  }

  // Dump sprite via HdPackManager to discover its exact filename & hash
  const auto dump_dir = tmp.path() / "dump";
  const auto pack_dir = tmp.path() / "pack";
  std::filesystem::create_directories(pack_dir);

  san4::HdPackManager hd;
  hd.set_dump_dir(dump_dir);
  hd.on_planar_blit_complete(16, 16, 16, 16, vid);

  std::filesystem::path dumped_file;
  for (const auto& ent : std::filesystem::directory_iterator(dump_dir)) {
    dumped_file = ent.path();
  }
  CHECK(!dumped_file.empty());

  // Author a 2x (32x32) pure bright-green (0xFF11EE22) HD replacement PNG
  std::vector<std::uint32_t> hd_pixels(32 * 32, 0xFF11EE22u);
  const auto pack_file = pack_dir / dumped_file.filename();
  CHECK(san4::HdPackManager::save_argb_png(pack_file, 32, 32, hd_pixels.data()));

  int lw = 0;
  int lh = 0;
  std::vector<std::uint32_t> roundtrip;
  CHECK(san4::HdPackManager::load_argb_png(pack_file, lw, lh, roundtrip));
  CHECK(lw == 32 && lh == 32 && roundtrip[0] == 0xFF11EE22u);

  hd.set_dumping_enabled(false);
  hd.set_pack_dir(pack_dir);
  CHECK(hd.loaded_texture_count() == 1u);

  // Trigger sprite blit registration, then draw a foreground dialog pixel (color 15)
  // over the right half (x = 24..31, y = 16..31) in VRAM!
  hd.on_planar_blit_complete(16, 16, 16, 16, vid);
  CHECK(hd.active_overlay_count() == 1u);

  for (int y = 16; y < 32; ++y) {
    vid.port_out8(0x3CEu, 0x08u);
    vid.port_out8(0x3CFu, 0xFFu);
    const std::uint32_t off = static_cast<std::uint32_t>(y * 100 + 3);  // x=24..31
    (void)vid.vram_read8(off);
    vid.vram_write8(off, 15u);
  }

  vid.latch_frame();
  san4::PixelUpscaler up(2);
  up.set_mode(san4::UpscaleMode::Sharp2x);
  san4::UpscaleDirtyRect dirty = up.upscale_from_video(vid, true);
  hd.composite_overlays(up.mutable_output_argb(), up.output_width(),
                        up.output_height(), up.scale_factor(), vid, dirty);

  const auto& canvas = up.output_argb();
  const int cw = up.output_width();
  // Unoccluded left half (dos x=18, y=20 -> 2x out x=36, y=40): HD green!
  CHECK(canvas[40 * cw + 36] == 0xFF11EE22u);
  // Occluded right half (dos x=26, y=20 -> 2x out x=52, y=40): foreground color 15!
  CHECK(canvas[40 * cw + 52] == vid.latched_palette_argb()[15]);
}

void test_vbe_probe_ems_move_mode12_and_bda_keys() {
  ScopedTempDir tmp;
  san4::MachineConfig cfg;
  cfg.game_dir = tmp.path();
  cfg.save_dir = tmp.path();
  san4::X86Machine m(cfg);

  // 1. Verify VBE 4F00h & 4F01h (mode 0x0102)
  std::uint16_t ax = 0x4F00u, bx = 0, cx = 0, dx = 0, es = 0x2000u, bp = 0;
  m.video().handle_int10(ax, bx, cx, dx, es, bp, 0x0100u,
                         std::span<std::uint8_t>(m.guest_ram(), 0x100000u),
                         0xF000u, 0xB000u);
  CHECK(ax == 0x004Fu);
  CHECK(std::memcmp(m.guest_ram() + 0x20100u, "VESA", 4) == 0);
  ax = 0x4F01u;
  cx = 0x0102u;
  m.video().handle_int10(ax, bx, cx, dx, es, bp, 0x0200u,
                         std::span<std::uint8_t>(m.guest_ram(), 0x100000u),
                         0xF000u, 0xB000u);
  CHECK(ax == 0x004Fu);
  CHECK(m.guest_ram()[0x20210u] == 100u);  // 100 bytes/scanline

  // 2. Verify Mode 0x12 (640x480, 80 bytes/scanline) planar unwrapping & upscale
  m.video().reset_mode(0x0012u);
  CHECK(m.video().active_width() == 640);
  CHECK(m.video().active_height() == 480);
  m.video().vram_write8(80u * 5u + 2u, 0x80u);  // row 5, x=16
  m.video().latch_frame();
  CHECK(m.video().latched_vram()[5 * san4::VesaPlanarVideo::kWidth + 16] ==
        0x0Fu);
  san4::PixelUpscaler up(2);
  up.set_mode(san4::UpscaleMode::Sharp2x);
  (void)up.upscale_from_video(m.video(), true);
  CHECK(up.output_width() == 1280 && up.output_height() == 960);

  // 3. Verify EMS AH=57h Move Memory Region (conventional -> EMS -> conventional)
  // Guest snippet: allocate 1 page (AH=43h BX=1), move 16 bytes via AX=5700h
  san4::EmsManager& ems = m.ems();
  x86emu_t* emu = m.emu();
  emu->x86.R_AH = 0x43u;
  emu->x86.R_BX = 1u;
  ems.handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u);
  const std::uint16_t handle = emu->x86.R_DX;

  for (int i = 0; i < 16; ++i) {
    m.guest_ram()[0x22000u + i] = static_cast<std::uint8_t>(0xA0 + i);
  }
  // Build 18-byte move descriptor at 0x2100:0000 -> 0x21000:
  // len=16, src_type=0 (conv), src_h=0, src_off=0, src_seg=0x2200,
  // dst_type=1 (EMS), dst_h=handle, dst_off=0x0040, dst_page=0
  std::uint8_t* desc = m.guest_ram() + 0x21000u;
  std::memset(desc, 0, 18);
  desc[0] = 16u;
  desc[9] = 0x00u;
  desc[10] = 0x22u;
  desc[11] = 1u;
  desc[12] = static_cast<std::uint8_t>(handle & 0xFFu);
  desc[13] = static_cast<std::uint8_t>(handle >> 8);
  desc[14] = 0x40u;
  x86emu_set_seg_register(emu, emu->x86.R_DS_SEL, 0x2100u);
  emu->x86.R_SI = 0x0000u;
  emu->x86.R_AX = 0x5700u;
  ems.handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u);

  // Map page 0 into physical page 0 (0xD0000) and verify moved bytes!
  emu->x86.R_AX = 0x4400u;
  emu->x86.R_BX = 0u;
  emu->x86.R_DX = handle;
  ems.handle_int67(emu);
  CHECK(m.guest_ram()[0x22000u + 5] == 0xA5u);
  CHECK(x86emu_read_byte(emu, 0xD0040u + 5u) == 0xA5u);

  // 4. Verify direct BDA 0040:001A..003D multi-key consumption (MAIN.EXE pattern:
  //    read [0040:BX], then flush [0040:001A] = [0040:001C])!
  m.dos().clear_keys();
  m.dos().push_key('1', 0x02u);
  m.dos().push_key('2', 0x03u);

  // Guest routine at 0x3000:0000 reads two keys via BDA into [0x3000:0x0100] & [0x0102]
  static constexpr std::uint8_t kBdaReadTwo[] = {
      0xB8, 0x40, 0x00,              // mov ax, 0x0040
      0x8E, 0xD8,                    // mov ds, ax
      // First key:
      0x8B, 0x1E, 0x1A, 0x00,        // mov bx, [0x001A]
      0xA1, 0x1C, 0x00,              // mov ax, [0x001C]
      0x8B, 0x0F,                    // mov cx, [bx]
      0xA3, 0x1A, 0x00,              // mov [0x001A], ax  (MAIN.EXE flush!)
      0x26, 0x89, 0x0E, 0x00, 0x01,  // mov es:[0x0100], cx
      // Second key (staged automatically on next [0x001A] read!):
      0x8B, 0x1E, 0x1A, 0x00,        // mov bx, [0x001A]
      0xA1, 0x1C, 0x00,              // mov ax, [0x001C]
      0x8B, 0x0F,                    // mov cx, [bx]
      0xA3, 0x1A, 0x00,              // mov [0x001A], ax
      0x26, 0x89, 0x0E, 0x02, 0x01,  // mov es:[0x0102], cx
      0xF4,                          // hlt
  };
  m.load_synthetic_code(0x3000u, 0x0000u, kBdaReadTwo);
  m.step_instructions(32);
  const std::uint16_t k1 =
      static_cast<std::uint16_t>(m.guest_ram()[0x30100u]) |
      (static_cast<std::uint16_t>(m.guest_ram()[0x30101u]) << 8);
  const std::uint16_t k2 =
      static_cast<std::uint16_t>(m.guest_ram()[0x30102u]) |
      (static_cast<std::uint16_t>(m.guest_ram()[0x30103u]) << 8);
  CHECK(k1 == 0x0231u);  // '1' | (0x02 << 8)
  CHECK(k2 == 0x0332u);  // '2' | (0x03 << 8)
}

void test_multiplane_ah13_sprite_dump_and_hd_overlay() {
  ScopedTempDir tmp;
  const auto dump_dir = tmp.path() / "dump";
  const auto pack_dir = tmp.path() / "pack";
  std::filesystem::create_directories(pack_dir);

  san4::MachineConfig cfg;
  cfg.game_dir = tmp.path();
  cfg.save_dir = tmp.path();
  cfg.dump_sprites_dir = dump_dir;
  san4::X86Machine m(cfg);
  m.video().reset_mode(0x0102u);

  // Synthetic GRP600C dispatcher stub at 0x2800:0x0018:
  //   int 0xF1; cmp ah, 0x13; jne done;
  //   push dx; push ax; mov dx, 0x03C4; mov al, 0x02; out dx, al;
  //   inc dx; pop ax; out dx, al;             (AL holds plane mask 1/2/4/8)
  //   mov ax, 0xA000; mov es, ax; mov byte es:[20*100 + 4], 0xAA; pop dx;
  // done: retf
  static constexpr std::uint8_t kGrpStub[] = {
      0xCD, 0xF1,                    // int 0xF1
      0x80, 0xFC, 0x13,              // cmp ah, 0x13
      0x75, 0x17,                    // jne done (+23 -> retf at offset 30)
      0x52,                          // push dx
      0x50,                          // push ax
      0xBA, 0xC4, 0x03,              // mov dx, 0x03C4
      0xB0, 0x02,                    // mov al, 0x02
      0xEE,                          // out dx, al
      0x42,                          // inc dx (0x03C5)
      0x58,                          // pop ax
      0xEE,                          // out dx, al (write plane mask = AL)
      0xB8, 0x00, 0xA0,              // mov ax, 0xA000
      0x8E, 0xC0,                    // mov es, ax
      0x26, 0xC6, 0x06, 0xD4, 0x07, 0xAA,  // mov byte es:[2004], 0xAA (x=32..39, y=20)
      0x5A,                          // pop dx
      0xCB,                          // retf
  };
  std::memcpy(m.guest_ram() + 0x28018u, kGrpStub, sizeof(kGrpStub));

  // Caller at 0x3000:0x0000: executes 4 consecutive (AH=0x1B, AH=0x13) plane
  // passes for rect (x=32, y=20, w=32, h=24), then AH=0x1E to flush!
  std::vector<std::uint8_t> caller = {
      0xBB, 0x20, 0x00,  // mov bx, 32
      0xB9, 0x14, 0x00,  // mov cx, 20
      0xBE, 0x20, 0x00,  // mov si, 32
      0xBF, 0x18, 0x00,  // mov di, 24
  };
  for (std::uint8_t plane_bit : {1u, 2u, 4u, 8u}) {
    // mov ax, 0x1B00 | plane_bit; lcall 0x2800:0x0018
    caller.insert(caller.end(), {0xB8, plane_bit, 0x1Bu, 0x9A, 0x18, 0x00, 0x00, 0x28});
    // mov ax, 0x1300 | plane_bit; lcall 0x2800:0x0018
    caller.insert(caller.end(), {0xB8, plane_bit, 0x13u, 0x9A, 0x18, 0x00, 0x00, 0x28});
  }
  // Flush via AH=0x1E (`kbhit`) + hlt
  caller.insert(caller.end(), {0xB4, 0x1Eu, 0x9A, 0x18, 0x00, 0x00, 0x28, 0xF4});

  m.load_synthetic_code(0x3000u, 0x0000u, caller);
  m.step_instructions(500);

  // Must coalesce all 4 bitplane passes into ONE dumped PNG (32x24)!
  int dump_count = 0;
  std::filesystem::path dumped_png;
  for (const auto& ent : std::filesystem::directory_iterator(dump_dir)) {
    ++dump_count;
    dumped_png = ent.path();
  }
  CHECK(dump_count == 1);
  CHECK(!dumped_png.empty());

  // Install a 4x (128x96) HD texture replacement (0xFFEE22CC) and re-blit!
  std::vector<std::uint32_t> hd_px(128 * 96, 0xFFEE22CCu);
  CHECK(san4::HdPackManager::save_argb_png(pack_dir / dumped_png.filename(),
                                           128, 96, hd_px.data()));
  m.hd_pack().set_dumping_enabled(false);
  m.hd_pack().set_pack_dir(pack_dir);
  CHECK(m.hd_pack().loaded_texture_count() == 1u);

  m.load_synthetic_code(0x3000u, 0x0000u, caller);
  m.step_instructions(500);
  CHECK(m.hd_pack().active_overlay_count() == 1u);

  m.video().latch_frame();
  san4::PixelUpscaler up(2);
  up.set_mode(san4::UpscaleMode::Xbr4x);
  san4::UpscaleDirtyRect dirty = up.upscale_from_video(m.video(), true);
  m.hd_pack().composite_overlays(up.mutable_output_argb(), up.output_width(),
                                 up.output_height(), up.scale_factor(),
                                 m.video(), dirty);
  CHECK(up.output_argb()[(20 * 4) * up.output_width() + (32 * 4)] ==
        0xFFEE22CCu);
}

void test_mcb_last_fit_strategy_allows_heap_sbrk_growth() {
  ScopedTempDir tmp;
  san4::MachineConfig cfg;
  cfg.game_dir = tmp.path();
  cfg.save_dir = tmp.path();
  san4::X86Machine m(cfg);

  // Simulate MS C 7.0 startup + MOVE overlay cache initialization:
  // 1) Allocate program block `prog_seg` of 0x1000 paras and shrink to 0x0800.
  const auto prog_seg = m.dos().mcb_alloc(0x1000u, 0x0900u);
  CHECK(prog_seg.has_value());
  CHECK(m.dos().mcb_resize(*prog_seg, 0x0800u));

  // 2) Guest executes INT 21h AX=5801h BX=0002h (Last Fit), INT 21h AH=48h
  //    BX=0AC1h (overlay cache at top of 640 KB), INT 21h AX=5801h BX=0000h,
  //    then grows `prog_seg` via INT 21h AH=4Ah from 0x0800 -> 0x0C00!
  const std::uint8_t lo = static_cast<std::uint8_t>(*prog_seg & 0xFFu);
  const std::uint8_t hi = static_cast<std::uint8_t>(*prog_seg >> 8);
  const std::uint8_t kSbrkCode[] = {
      0xB8, 0x01, 0x58,        // mov ax, 0x5801
      0xBB, 0x02, 0x00,        // mov bx, 2 (Last Fit)
      0xCD, 0x21,              // int 0x21
      0xB4, 0x48,              // mov ah, 0x48
      0xBB, 0xC1, 0x0A,        // mov bx, 0x0AC1
      0xCD, 0x21,              // int 0x21 -> AX = high overlay segment
      0x89, 0xC6,              // mov si, ax
      0xB8, 0x01, 0x58,        // mov ax, 0x5801
      0x31, 0xDB,              // xor bx, bx (First Fit)
      0xCD, 0x21,              // int 0x21
      0xB8, lo, hi,            // mov ax, prog_seg
      0x8E, 0xC0,              // mov es, ax
      0xB4, 0x4A,              // mov ah, 0x4A
      0xBB, 0x00, 0x0C,        // mov bx, 0x0C00 (grow sbrk heap!)
      0xCD, 0x21,              // int 0x21 -> CF must be 0!
      0xF4,                    // hlt
  };
  m.load_synthetic_code(0x0700u, 0x0000u, kSbrkCode, 0x0700u, 0x0FF0u);
  m.step_instructions(64);

  CHECK((m.emu()->x86.R_EFLG & FB_CF) == 0u);
  CHECK(m.emu()->x86.R_SI ==
        static_cast<std::uint16_t>(san4::DosKernel::kConventionalTopSeg - 0x0AC1u));
}

void test_cross_boundary_memio_and_32bit_bda_poll() {
  san4::X86Machine m;
  m.video().reset_mode(0x0102u);

  // Enable all 4 VGA planes in Write Mode 0 for 0xA0000 straddle checks.
  m.video().port_out8(0x03C4u, 0x02u);
  m.video().port_out8(0x03C5u, 0x0Fu);
  m.video().port_out8(0x03CEu, 0x05u);
  m.video().port_out8(0x03CFu, 0x00u);

  // Seed ROM sentinel bytes at 0xF0000..0xF0001 and 0xFFFFE..0xFFFFF.
  m.guest_ram()[0xF0000u] = 0x5Au;
  m.guest_ram()[0xF0001u] = 0xA5u;
  m.guest_ram()[0xFFFFEu] = 0xCCu;
  m.guest_ram()[0xFFFFFu] = 0xDDu;

  // Queue a key so a 32-bit BDA read at 0040:001A stages head != tail.
  m.dos().clear_keys();
  m.dos().push_key('Z', 0x2Cu);

  // Guest code exercising 32-bit straddles across every memory boundary:
  //   1) 0x0000:0x0FFE -> 0x0FFE..0x1001 (Page 0 <-> mapped RAM page 1)
  //   2) 0x9000:0xFFFE -> 0x9FFFE..0xA0001 (RAM <-> Planar VRAM)
  //   3) 0xA000:0xFFFE -> 0xAFFFE..0xB0001 (VRAM tail <-> B0000 RAM)
  //   4) 0xE000:0xFFFE -> 0xEFFFE..0xF0001 (UMB <-> write-protected BIOS ROM)
  //   5) 0xFFFF:0x000E -> 0xFFFFE..0x100001 (BIOS ROM <-> HMA)
  //   6) 0x0040:0x001A -> 32-bit BDA head+tail poll into EDX
  static constexpr std::uint8_t kStraddleCode[] = {
      // 1) DS = 0x0000: [0x0FFE] = 0x11223344, EAX = [0x0FFE] -> [0x2000:0x0200]
      0x31, 0xC0,                                      // xor ax, ax
      0x8E, 0xD8,                                      // mov ds, ax
      0x66, 0xC7, 0x06, 0xFE, 0x0F, 0x44, 0x33, 0x22, 0x11,
      0x66, 0xA1, 0xFE, 0x0F,
      0x26, 0x66, 0xA3, 0x00, 0x02,
      // 2) DS = 0x9000: [0xFFFE] = 0xAA55BEEF, EAX = [0xFFFE] -> [0x2000:0x0204]
      0xB8, 0x00, 0x90,
      0x8E, 0xD8,
      0x66, 0xC7, 0x06, 0xFE, 0xFF, 0xEF, 0xBE, 0x55, 0xAA,
      0x66, 0xA1, 0xFE, 0xFF,
      0x26, 0x66, 0xA3, 0x04, 0x02,
      // 3) DS = 0xA000: [0xFFFE] = 0xDEADBEEF, EAX = [0xFFFE] -> [0x2000:0x0208]
      0xB8, 0x00, 0xA0,
      0x8E, 0xD8,
      0x66, 0xC7, 0x06, 0xFE, 0xFF, 0xEF, 0xBE, 0xAD, 0xDE,
      0x66, 0xA1, 0xFE, 0xFF,
      0x26, 0x66, 0xA3, 0x08, 0x02,
      // 4) DS = 0xE000: [0xFFFE] = 0x12345678, EAX = [0xFFFE] -> [0x2000:0x020C]
      0xB8, 0x00, 0xE0,
      0x8E, 0xD8,
      0x66, 0xC7, 0x06, 0xFE, 0xFF, 0x78, 0x56, 0x34, 0x12,
      0x66, 0xA1, 0xFE, 0xFF,
      0x26, 0x66, 0xA3, 0x0C, 0x02,
      // 5) DS = 0xFFFF: [0x000E] = 0xCAFEBABE, EAX = [0x000E] -> [0x2000:0x0210]
      0xB8, 0xFF, 0xFF,
      0x8E, 0xD8,
      0x66, 0xC7, 0x06, 0x0E, 0x00, 0xBE, 0xBA, 0xFE, 0xCA,
      0x66, 0xA1, 0x0E, 0x00,
      0x26, 0x66, 0xA3, 0x10, 0x02,
      // 6) DS = 0x0040: EDX = [0x001A] (32-bit BDA head+tail read)
      0xB8, 0x40, 0x00,
      0x8E, 0xD8,
      0x66, 0x8B, 0x16, 0x1A, 0x00,
      0xF4,
  };

  m.load_synthetic_code(0x2000u, 0x0100u, kStraddleCode);
  m.step_instructions(64);

  auto rd32 = [&](std::uint32_t lin) -> std::uint32_t {
    std::uint32_t v = 0;
    std::memcpy(&v, m.guest_ram() + lin, 4);
    return v;
  };

  // 1) Page 0 <-> Page 1 coherency
  CHECK(rd32(0x0FFEu) == 0x11223344u);
  CHECK(rd32(0x20200u) == 0x11223344u);

  // 2) Conventional RAM <-> Planar VRAM coherency
  CHECK(m.guest_ram()[0x9FFFEu] == 0xEFu);
  CHECK(m.guest_ram()[0x9FFFFu] == 0xBEu);
  CHECK(m.video().vram_read8(0u) == 0x55u);
  CHECK(m.video().vram_read8(1u) == 0xAAu);
  CHECK(rd32(0x20204u) == 0xAA55BEEFu);

  // 3) Off-screen Planar VRAM tail (0xFFFE..0xFFFF) <-> B0000 RAM
  CHECK(m.video().vram_read8(0xFFFEu) == 0xEFu);
  CHECK(m.video().vram_read8(0xFFFFu) == 0xBEu);
  CHECK(m.guest_ram()[0xB0000u] == 0xADu);
  CHECK(m.guest_ram()[0xB0001u] == 0xDEu);
  CHECK(rd32(0x20208u) == 0xDEADBEEFu);

  // 4) UMB <-> BIOS ROM write-protection
  CHECK(m.guest_ram()[0xEFFFEu] == 0x78u);
  CHECK(m.guest_ram()[0xEFFFFu] == 0x56u);
  CHECK(m.guest_ram()[0xF0000u] == 0x5Au);
  CHECK(m.guest_ram()[0xF0001u] == 0xA5u);
  CHECK(rd32(0x2020Cu) == 0xA55A5678u);

  // 5) BIOS ROM <-> HMA write-protection & HMA update
  CHECK(m.guest_ram()[0xFFFFEu] == 0xCCu);
  CHECK(m.guest_ram()[0xFFFFFu] == 0xDDu);
  CHECK(m.guest_ram()[0x100000u] == 0xFEu);
  CHECK(m.guest_ram()[0x100001u] == 0xCAu);
  CHECK(rd32(0x20210u) == 0xCAFEDDCCu);

  // 6) 32-bit BDA poll staged the queued key (head != tail)
  const std::uint32_t bda_dw = m.emu()->x86.R_EDX;
  CHECK((bda_dw & 0xFFFFu) != ((bda_dw >> 16) & 0xFFFFu));

  // 7) Out-of-bounds straddle at kGuestRamBytes - 2
  const auto edge = static_cast<std::uint32_t>(san4::X86Machine::kGuestRamBytes - 2u);
  x86emu_write_dword(m.emu(), edge, 0x778899AAu);
  CHECK(m.guest_ram()[edge + 0u] == 0xAAu);
  CHECK(m.guest_ram()[edge + 1u] == 0x99u);
  CHECK(x86emu_read_dword(m.emu(), edge) == 0xFFFF99AAu);
}

void test_smooth_fade_16slot_ega_grp_and_midfade_override() {
  san4::VesaPlanarVideo vid;
  vid.reset_mode(0x0102u);

  // 1) Default EGA Attribute Controller mapping (`dac_slot_map = nullptr`):
  //    slot 6 uses DAC 20, slots 8..15 use DAC 56..63.
  std::array<std::uint8_t, 48> src_black{};
  std::array<std::uint8_t, 48> dst_target{};
  // GRP600C 4-bit palette triples use NEC PC-98 [Blue, Red, Green] byte order.
  for (int i = 0; i < 16; ++i) {
    dst_target[static_cast<std::size_t>(i) * 3u + 0u] = 0x0Cu;
    dst_target[static_cast<std::size_t>(i) * 3u + 1u] = static_cast<std::uint8_t>(i);
    dst_target[static_cast<std::size_t>(i) * 3u + 2u] = static_cast<std::uint8_t>(15 - i);
  }

  vid.begin_smooth_fade(0u, 16u, src_black.data(), dst_target.data(), nullptr, 0.20);
  CHECK(vid.advance_fade(0.10));  // t = 0.5
  vid.latch_frame();
  for (int slot : {1, 6, 8, 12, 15}) {
    const std::uint32_t argb = vid.latched_palette_argb()[static_cast<std::size_t>(slot)];
    const int r = static_cast<int>((argb >> 16) & 0xFFu);
    const int g = static_cast<int>((argb >> 8) & 0xFFu);
    const int b = static_cast<int>(argb & 0xFFu);
    const int exp_r = (slot * 0x11 + 1) / 2;
    const int exp_g = ((15 - slot) * 0x11 + 1) / 2;
    CHECK(std::abs(r - exp_r) <= 4);
    CHECK(std::abs(g - exp_g) <= 4);
    CHECK(b >= 95 && b <= 110);
  }

  CHECK(!vid.advance_fade(0.15));  // t = 1.0 -> zero snap-back!
  vid.latch_frame();
  for (int slot : {1, 6, 8, 9, 10, 11, 12, 13, 14, 15}) {
    const std::uint32_t argb = vid.latched_palette_argb()[static_cast<std::size_t>(slot)];
    const std::uint32_t exp_r = static_cast<std::uint32_t>(slot * 0x11);
    const std::uint32_t exp_g = static_cast<std::uint32_t>((15 - slot) * 0x11);
    // 6-bit DAC quantization is within +/- 4 of 8-bit target.
    CHECK(std::abs(static_cast<int>((argb >> 16) & 0xFFu) - static_cast<int>(exp_r)) <= 4);
    CHECK(std::abs(static_cast<int>((argb >> 8) & 0xFFu) - static_cast<int>(exp_g)) <= 4);
    CHECK(std::abs(static_cast<int>(argb & 0xFFu) - 0xCC) <= 4);
  }

  // 2) GRP600C plane-swapped `slot_map` with `src_rgb_4bit = nullptr`:
  //    Program identity attr_regs_[i] = i and distinct DAC colors in DAC 2 (red)
  //    and DAC 4 (green), then fade to black using GRP600C's slot_map.
  (void)vid.port_in8(0x03DAu);
  for (std::uint8_t i = 0; i < 16u; ++i) {
    vid.port_out8(0x03C0u, i);
    vid.port_out8(0x03C0u, i);
  }
  vid.port_out8(0x03C0u, 0x20u);

  vid.port_out8(0x03C8u, 2u);
  vid.port_out8(0x03C9u, 63u);
  vid.port_out8(0x03C9u, 0u);
  vid.port_out8(0x03C9u, 0u);

  vid.port_out8(0x03C8u, 4u);
  vid.port_out8(0x03C9u, 0u);
  vid.port_out8(0x03C9u, 63u);
  vid.port_out8(0x03C9u, 0u);

  static constexpr std::array<std::uint8_t, 16> kGrpSlotMap = {
      0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15,
  };
  std::array<std::uint8_t, 48> dst_zero{};
  vid.begin_smooth_fade(0u, 16u, nullptr, dst_zero.data(), kGrpSlotMap.data(), 0.20);
  CHECK(vid.advance_fade(0.10));  // t = 0.5
  vid.latch_frame();
  const std::uint32_t p2_mid = vid.latched_palette_argb()[2];
  const std::uint32_t p4_mid = vid.latched_palette_argb()[4];
  CHECK(((p2_mid >> 16) & 0xFFu) >= 120u && ((p2_mid >> 8) & 0xFFu) == 0u);
  CHECK(((p4_mid >> 8) & 0xFFu) >= 120u && ((p4_mid >> 16) & 0xFFu) == 0u);
  CHECK(!vid.advance_fade(0.15));
  vid.latch_frame();
  CHECK((vid.latched_palette_argb()[2] & 0xFFFFFFu) == 0u);
  CHECK((vid.latched_palette_argb()[4] & 0xFFFFFFu) == 0u);

  // 3) Mid-fade explicit `OUT 0x3C8/0x3C9` override finishes fade first and
  //    preserves the newly written DAC color without snap-back.
  std::array<std::uint8_t, 48> dst_white{};
  dst_white.fill(0x0Fu);
  vid.begin_smooth_fade(0u, 16u, src_black.data(), dst_white.data(),
                        kGrpSlotMap.data(), 0.20);
  CHECK(vid.advance_fade(0.05));
  CHECK(vid.is_fading());

  vid.port_out8(0x03C8u, 1u);
  vid.port_out8(0x03C9u, 63u);
  vid.port_out8(0x03C9u, 0u);
  vid.port_out8(0x03C9u, 0u);  // Completes RGB triplet -> finishes fade first!
  CHECK(!vid.is_fading());

  (void)vid.advance_fade(0.50);
  vid.latch_frame();
  CHECK(vid.latched_palette_argb()[1] == 0xFFFF0000u);
  CHECK(vid.latched_palette_argb()[2] == 0xFFFFFFFFu);

  // 4) Back-to-back GRP600C `AH=0x16` smooth fades across `X86Machine::run_frame`:
  //    when Fade #1 finishes at the top of a frame and resumed mainline starts
  //    Fade #2 within the same `run_frame()` call, the ROM hold must remain active
  //    for Fade #2 (no stale-flag infinite loop) and complete cleanly on the next frame.
  {
    ScopedTempDir tmp;
    // TSR.COM: has GRP600C dispatch signature `E9 14 23 90 90` at CS:0x0018
    // (for a .COM image loaded at CS:0x0100, CS:0x0018 is in the PSP, so build a
    // tiny MZ .EXE whose CS starts at load_seg with `E9 14 23 90 90` at 0x0018!).
    std::vector<std::uint8_t> tsr_mz(0x20u + 0x2500u, 0u);
    auto wr16 = [](std::vector<std::uint8_t>& b, std::size_t off, std::uint16_t v) {
      b[off + 0u] = static_cast<std::uint8_t>(v & 0xFFu);
      b[off + 1u] = static_cast<std::uint8_t>(v >> 8);
    };
    const std::size_t total_sz = tsr_mz.size();
    wr16(tsr_mz, 0x00u, 0x5A4Du);
    wr16(tsr_mz, 0x02u, static_cast<std::uint16_t>(total_sz % 512u));
    wr16(tsr_mz, 0x04u, static_cast<std::uint16_t>((total_sz + 511u) / 512u));
    wr16(tsr_mz, 0x08u, 0x0002u);  // 32-byte header
    wr16(tsr_mz, 0x0Au, 0x0010u);
    wr16(tsr_mz, 0x0Cu, 0xFFFFu);
    wr16(tsr_mz, 0x0Eu, 0x0260u);  // SS
    wr16(tsr_mz, 0x10u, 0x0100u);  // SP
    wr16(tsr_mz, 0x14u, 0x0000u);  // IP = 0x0000 -> TSR entry
    wr16(tsr_mz, 0x16u, 0x0000u);  // CS = load_seg
    wr16(tsr_mz, 0x18u, 0x001Cu);
    // CS:0x0000: mov dx, 0x0300; mov ax, 0x3100; int 0x21 (TSR install!)
    const std::uint8_t kTsrEntry[] = {0xBA, 0x00, 0x03, 0xB8, 0x00, 0x31, 0xCD, 0x21};
    std::memcpy(tsr_mz.data() + 0x20u, kTsrEntry, sizeof(kTsrEntry));
    // CS:0x0018: expected GRP600C dispatcher signature `E9 14 23 90 90`
    const std::uint8_t kSig[] = {0xE9u, 0x14u, 0x23u, 0x90u, 0x90u};
    std::memcpy(tsr_mz.data() + 0x20u + 0x0018u, kSig, sizeof(kSig));
    // CS:0x232F: retf fallback if unpatched
    tsr_mz[0x20u + 0x232Fu] = 0xCBu;
    // CS:0x24E8: identity slot_map 0..15
    for (std::uint8_t i = 0; i < 16u; ++i) {
      tsr_mz[0x20u + 0x24E8u + i] = i;
    }
    {
      std::ofstream f(tmp.path() / "GRPTSR.EXE", std::ios::binary);
      f.write(reinterpret_cast<const char*>(tsr_mz.data()),
              static_cast<std::streamsize>(tsr_mz.size()));
    }

    // HOST.COM: shrinks MCB, EXECs GRPTSR.EXE, patches its far-call target CS
    // from `dos().grp600c_cs()` (stored at [0x0180] before running or read via
    // scanning for `CD F1` — even simpler: we pass `grp600c_cs` via memory after
    // TSR install, or boot HOST.COM up to HLT, then run two AH=0x16 far calls!).
    const std::vector<std::uint8_t> host_com = {
        0xBB, 0x00, 0x01,              // mov bx, 0x0100
        0xB4, 0x4A,                    // mov ah, 0x4A
        0xCD, 0x21,                    // int 0x21
        0xBA, 0x30, 0x01,              // mov dx, 0x0130 ("GRPTSR.EXE")
        0xBB, 0x40, 0x01,              // mov bx, 0x0140 (param block)
        0xB8, 0x00, 0x4B,              // mov ax, 0x4B00
        0xCD, 0x21,                    // int 0x21
        0xF4,                          // hlt (let test inspect grp600c_cs!)
        // Padding up to 0x0130 (offset 0x30 from 0x0100; current len = 19 bytes -> +29 zeros)
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        // 0x0130: "GRPTSR.EXE\0" (16 bytes -> 0x0140)
        'G', 'R', 'P', 'T', 'S', 'R', '.', 'E', 'X', 'E', 0, 0, 0, 0, 0, 0,
        // 0x0140: 14-byte param block
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    {
      std::ofstream f(tmp.path() / "HOST.COM", std::ios::binary);
      f.write(reinterpret_cast<const char*>(host_com.data()),
              static_cast<std::streamsize>(host_com.size()));
    }

    san4::MachineConfig cfg;
    cfg.game_dir = tmp.path();
    cfg.save_dir = tmp.path();
    san4::X86Machine m(cfg);
    m.video().reset_mode(0x0102u);
    CHECK(m.boot("HOST.COM"));
    m.step_instructions(200);

    const std::uint16_t grp_cs = m.dos().grp600c_cs();
    CHECK(grp_cs != 0u);
    const auto grp_lo = static_cast<std::uint8_t>(grp_cs & 0xFFu);
    const auto grp_hi = static_cast<std::uint8_t>(grp_cs >> 8);

    // Target 48-byte palette buffer at 0x3000:0x0200 (all 0x0F) and 0x3000:0x0240 (all 0x00)
    std::memset(m.guest_ram() + 0x30200u, 0x0Fu, 48u);
    std::memset(m.guest_ram() + 0x30240u, 0x00u, 48u);

    // Load synthetic sequence at 0x3000:0x0000:
    //   Fade #1: AX=1600h, CX=0110h (steps=1, count=16), DX=0002h (2/60s = 33.3ms),
    //            ES=3000h, BX=0200h -> lcall grp_cs:0x0018
    //   Fade #2: immediately upon resume, AX=1600h, CX=0110h, DX=0002h,
    //            ES=3000h, BX=0240h -> lcall grp_cs:0x0018
    //   Exit:    AX=4C42h, int 21h
    const std::uint8_t kTwoFades[] = {
        0xB8, 0x00, 0x30,                    // mov ax, 0x3000
        0x8E, 0xC0,                          // mov es, ax
        0xB8, 0x00, 0x16,                    // mov ax, 0x1600
        0xB9, 0x10, 0x01,                    // mov cx, 0x0110
        0xBA, 0x02, 0x00,                    // mov dx, 0x0002
        0xBB, 0x00, 0x02,                    // mov bx, 0x0200
        0x9A, 0x18, 0x00, grp_lo, grp_hi,    // lcall grp_cs:0x0018 (Fade #1!)
        0xB8, 0x00, 0x16,                    // mov ax, 0x1600
        0xB9, 0x10, 0x01,                    // mov cx, 0x0110
        0xBA, 0x02, 0x00,                    // mov dx, 0x0002
        0xBB, 0x40, 0x02,                    // mov bx, 0x0240
        0x9A, 0x18, 0x00, grp_lo, grp_hi,    // lcall grp_cs:0x0018 (Fade #2!)
        0xB8, 0x42, 0x4C,                    // mov ax, 0x4C42
        0xCD, 0x21,                          // int 0x21
    };
    m.load_synthetic_code(0x3000u, 0x0000u, kTwoFades, 0x3000u, 0x0FF0u);

    // Frame 1 (10 ms): starts Fade #1 (33.3 ms) and holds in BIOS fade ROM.
    m.run_frame(0.010);
    CHECK(m.video().is_fading());
    CHECK(!m.exited());

    // Frame 2 (40 ms > 33.3 ms): finishes Fade #1 at top of `run_frame`, resumes
    // mainline, which immediately starts Fade #2 (33.3 ms) within the SAME `run_frame`!
    m.run_frame(0.040);
    CHECK(m.video().is_fading());
    CHECK(!m.exited());

    // Frame 3 (40 ms): finishes Fade #2 and executes `AX=4C42h INT 21h`!
    m.run_frame(0.040);
    CHECK(!m.video().is_fading());
    CHECK(m.exited());
    CHECK(m.exit_code() == 0x42u);
  }
}

void test_ems_overlap_wrap_and_map_validation() {
  san4::X86Machine m;
  san4::EmsManager& ems = m.ems();
  x86emu_t* emu = m.emu();

  // Allocate 2 EMS pages
  emu->x86.R_AH = 0x43u;
  emu->x86.R_BX = 2u;
  ems.handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u);
  const std::uint16_t handle = emu->x86.R_DX;

  // Map page 0 -> physical slot 0 (0xD0000)
  emu->x86.R_AX = 0x4400u;
  emu->x86.R_BX = 0u;
  emu->x86.R_DX = handle;
  ems.handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u);

  auto seed_seq = [&]() {
    for (std::uint32_t i = 0; i < 32u; ++i) {
      x86emu_write_byte(emu, 0xD0000u + i, static_cast<std::uint8_t>(i));
    }
  };
  auto run_57 = [&](std::uint8_t al, std::uint32_t len, std::uint8_t st,
                    std::uint16_t sh, std::uint16_t soff, std::uint16_t sseg,
                    std::uint8_t dt, std::uint16_t dh, std::uint16_t doff,
                    std::uint16_t dseg) -> std::uint8_t {
    std::uint8_t* d = m.guest_ram() + 0x21000u;
    std::memset(d, 0, 18);
    std::memcpy(d + 0, &len, 4);
    d[4] = st;
    std::memcpy(d + 5, &sh, 2);
    std::memcpy(d + 7, &soff, 2);
    std::memcpy(d + 9, &sseg, 2);
    d[11] = dt;
    std::memcpy(d + 12, &dh, 2);
    std::memcpy(d + 14, &doff, 2);
    std::memcpy(d + 16, &dseg, 2);
    x86emu_set_seg_register(emu, emu->x86.R_DS_SEL, 0x2100u);
    emu->x86.R_SI = 0x0000u;
    emu->x86.R_AH = 0x57u;
    emu->x86.R_AL = al;
    ems.handle_int67(emu);
    return emu->x86.R_AH;
  };

  // 1) Forward overlapping EMS move (src=0 -> dst=4, len=16) => AH=0x92 + memmove intact
  seed_seq();
  CHECK(run_57(0x00u, 16u, 1u, handle, 0u, 0u, 1u, handle, 4u, 0u) == 0x92u);
  for (std::uint32_t i = 0; i < 16u; ++i) {
    CHECK(x86emu_read_byte(emu, 0xD0004u + i) == static_cast<std::uint8_t>(i));
  }

  // 2) Backward overlapping EMS move (src=4 -> dst=0, len=16) => AH=0x92 + memmove intact
  seed_seq();
  CHECK(run_57(0x00u, 16u, 1u, handle, 4u, 0u, 1u, handle, 0u, 0u) == 0x92u);
  for (std::uint32_t i = 0; i < 16u; ++i) {
    CHECK(x86emu_read_byte(emu, 0xD0000u + i) == static_cast<std::uint8_t>(i + 4u));
  }

  // 3) Overlapping EMS exchange (AL=01h) => AH=0x97 abort without mutating data
  seed_seq();
  CHECK(run_57(0x01u, 16u, 1u, handle, 0u, 0u, 1u, handle, 8u, 0u) == 0x97u);
  for (std::uint32_t i = 0; i < 32u; ++i) {
    CHECK(x86emu_read_byte(emu, 0xD0000u + i) == static_cast<std::uint8_t>(i));
  }

  // 4) Overlap between mapped 0xD000:0000 (type=0) and EMS page 0 (type=1)
  CHECK(run_57(0x01u, 16u, 0u, 0u, 0u, 0xD000u, 1u, handle, 8u, 0u) == 0x97u);

  // 5) Conventional wrap past 1 MiB (`FFFF:0018`, len=16) => AH=0xA2, IVT untouched
  std::array<std::uint8_t, 16> ivt_snap{};
  std::memcpy(ivt_snap.data(), m.guest_ram(), 16u);
  CHECK(run_57(0x00u, 16u, 1u, handle, 0u, 0u, 0u, 0u, 0x0018u, 0xFFFFu) == 0xA2u);
  CHECK(std::memcmp(ivt_snap.data(), m.guest_ram(), 16u) == 0);

  // 6) AH=50h AL=01h segment validation (0xD000 & 0xDC00 valid; 0xCF00 & 0xD100 -> 0x8B)
  std::uint8_t* map_tab = m.guest_ram() + 0x22000u;
  const std::uint16_t valid_entries[4] = {0u, 0xD000u, 1u, 0xDC00u};
  std::memcpy(map_tab, valid_entries, sizeof(valid_entries));
  x86emu_set_seg_register(emu, emu->x86.R_DS_SEL, 0x2200u);
  emu->x86.R_SI = 0u;
  emu->x86.R_AX = 0x5001u;
  emu->x86.R_CX = 2u;
  emu->x86.R_DX = handle;
  ems.handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u);

  for (std::uint16_t bad_seg : {std::uint16_t{0xCF00u}, std::uint16_t{0xD100u}}) {
    const std::uint16_t bad_entry[2] = {0u, bad_seg};
    std::memcpy(map_tab, bad_entry, sizeof(bad_entry));
    emu->x86.R_AX = 0x5001u;
    emu->x86.R_CX = 1u;
    emu->x86.R_DX = handle;
    ems.handle_int67(emu);
    CHECK(emu->x86.R_AH == 0x8Bu);
  }

  emu->x86.R_AX = 0x5002u;
  emu->x86.R_CX = 0u;
  emu->x86.R_DX = handle;
  ems.handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x8Fu);

  // 7) AH=4Eh AL=00h/01h validation against corrupted pool page index -> AH=0xA3
  x86emu_set_seg_register(emu, emu->x86.R_ES_SEL, 0x2300u);
  emu->x86.R_DI = 0u;
  emu->x86.R_AX = 0x4E00u;
  ems.handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u);

  const std::int32_t bogus_pool_page = 9999;
  std::memcpy(m.guest_ram() + 0x23000u, &bogus_pool_page, sizeof(bogus_pool_page));
  x86emu_set_seg_register(emu, emu->x86.R_DS_SEL, 0x2300u);
  emu->x86.R_SI = 0u;
  emu->x86.R_AX = 0x4E01u;
  ems.handle_int67(emu);
  CHECK(emu->x86.R_AH == 0xA3u);
}

void test_mz_exe_relocations_com_bounds_and_child_restore() {
  ScopedTempDir tmp;

  // Build synthetic MZ executable `CHILDMZ.EXE`:
  //   Header: 32 bytes (e_cparhdr = 2), 1 relocation at 0x0001:0x0020
  //   Image:
  //     para 0 (offset 0x00..0x0F): data padding
  //     para 1..2 (e_cs = 0x0001, e_ip = 0x0000):
  //       verifies relocated word at CS:0x0020 equals live CS (`load_seg + 1`),
  //       changes MCB strategy to Last Fit (`AX=5801h BX=2`), overwrites INT 24h,
  //       and exits with AL=0x77.
  //     para 5 (e_ss = 0x0005, e_sp = 0x0080): stack
  {
    const std::uint8_t cs_exact[] = {
        0x8C, 0xC8,                    // 0x00: mov ax, cs
        0x2E, 0x3B, 0x06, 0x20, 0x00,  // 0x02: cmp ax, cs:[0x0020]
        0x75, 0x12,                    // 0x07: jne 0x1B (fail)
        0xB8, 0x01, 0x58,              // 0x09: mov ax, 0x5801
        0xBB, 0x02, 0x00,              // 0x0C: mov bx, 0x0002 (Last Fit)
        0xCD, 0x21,                    // 0x0F: int 0x21
        0xB8, 0x24, 0x25,              // 0x11: mov ax, 0x2524 (set INT 24h)
        0xCD, 0x21,                    // 0x14: int 0x21
        0xB8, 0x77, 0x4C,              // 0x16: mov ax, 0x4C77
        0xCD, 0x21,                    // 0x19: int 0x21
        0xB8, 0xEE, 0x4C,              // 0x1B: mov ax, 0x4CEE
        0xCD, 0x21,                    // 0x1E: int 0x21
        0x01, 0x00,                    // 0x20: unrelocated 0x0001 -> load_seg + 1 == CS
    };
    std::vector<std::uint8_t> mz(32u + 16u + sizeof(cs_exact) + 32u, 0u);
    auto put16 = [&](std::size_t off, std::uint16_t v) {
      mz[off + 0] = static_cast<std::uint8_t>(v & 0xFFu);
      mz[off + 1] = static_cast<std::uint8_t>(v >> 8);
    };
    put16(0x00, 0x5A4Du);  // "MZ"
    put16(0x02, static_cast<std::uint16_t>(mz.size()));
    put16(0x04, 1u);       // e_cp = 1 page
    put16(0x06, 1u);       // e_crlc = 1 relocation
    put16(0x08, 2u);       // e_cparhdr = 2 paras (32 bytes)
    put16(0x0A, 16u);      // e_minalloc
    put16(0x0C, 32u);      // e_maxalloc
    put16(0x0E, 5u);       // e_ss = +5 paras
    put16(0x10, 0x0080u);  // e_sp
    put16(0x14, 0x0000u);  // e_ip
    put16(0x16, 1u);       // e_cs = +1 para
    put16(0x18, 0x001Cu);  // e_lfarlc = 28
    put16(0x1C, 0x0020u);  // reloc offset = 0x0020
    put16(0x1E, 0x0001u);  // reloc segment = 0x0001
    std::memcpy(mz.data() + 32u + 16u, cs_exact, sizeof(cs_exact));

    std::ofstream f(tmp.path() / "CHILDMZ.EXE", std::ios::binary);
    f.write(reinterpret_cast<const char*>(mz.data()),
            static_cast<std::streamsize>(mz.size()));

    // Also save `BADREL.EXE` with an out-of-bounds relocation segment (0x7FFF)
    put16(0x1E, 0x7FFFu);
    std::ofstream fb(tmp.path() / "BADREL.EXE", std::ios::binary);
    fb.write(reinterpret_cast<const char*>(mz.data()),
             static_cast<std::streamsize>(mz.size()));
  }

  // Write `PARMZ.COM`: shrinks MCB, EXECs `CHILDMZ.EXE`, verifies child rc=0x77,
  // verifies parent MCB strategy was restored to 0 (First Fit), exits 0x55.
  {
    std::vector<std::uint8_t> bin(0x50u, 0u);
    const std::uint8_t exact[] = {
        0xBB, 0x00, 0x01,  // 0x00: mov bx, 0x0100
        0xB4, 0x4A,        // 0x03: mov ah, 0x4A
        0xCD, 0x21,        // 0x05: int 0x21
        0x72, 0x23,        // 0x07: jc 0x2C (fail)
        0xBA, 0x32, 0x01,  // 0x09: mov dx, 0x0132 ("CHILDMZ.EXE")
        0xBB, 0x40, 0x01,  // 0x0C: mov bx, 0x0140
        0xB8, 0x00, 0x4B,  // 0x0F: mov ax, 0x4B00
        0xCD, 0x21,        // 0x12: int 0x21
        0x72, 0x16,        // 0x14: jc 0x2C (fail)
        0xB4, 0x4D,        // 0x16: mov ah, 0x4D
        0xCD, 0x21,        // 0x18: int 0x21
        0x3C, 0x77,        // 0x1A: cmp al, 0x77
        0x75, 0x0E,        // 0x1C: jne 0x2C (fail)
        0xB8, 0x00, 0x58,  // 0x1E: mov ax, 0x5800
        0xCD, 0x21,        // 0x21: int 0x21
        0x09, 0xC0,        // 0x23: or ax, ax
        0x75, 0x05,        // 0x25: jnz 0x2C (fail)
        0xB8, 0x55, 0x4C,  // 0x27: mov ax, 0x4C55
        0xCD, 0x21,        // 0x2A: int 0x21
        0xB8, 0xEE, 0x4C,  // 0x2C: fail: mov ax, 0x4CEE
        0xCD, 0x21,        // 0x2F: int 0x21
    };
    std::memcpy(bin.data(), exact, sizeof(exact));
    std::memcpy(bin.data() + 0x32u, "CHILDMZ.EXE", 12u);
    std::ofstream f(tmp.path() / "PARMZ.COM", std::ios::binary);
    f.write(reinterpret_cast<const char*>(bin.data()),
            static_cast<std::streamsize>(bin.size()));
  }

  // Oversized (> 0xFE00) and 1 MiB modulo-wrap .COM files
  {
    std::vector<std::uint8_t> big1(0xFE01u, 0x90u);
    std::ofstream f1(tmp.path() / "BIG1.COM", std::ios::binary);
    f1.write(reinterpret_cast<const char*>(big1.data()),
             static_cast<std::streamsize>(big1.size()));

    std::vector<std::uint8_t> big2(1048576u, 0x90u);
    std::ofstream f2(tmp.path() / "WRAP1M.COM", std::ios::binary);
    f2.write(reinterpret_cast<const char*>(big2.data()),
             static_cast<std::streamsize>(big2.size()));
  }

  san4::MachineConfig cfg;
  cfg.game_dir = tmp.path();
  {
    san4::X86Machine m(cfg);
    const std::uint32_t ivt24_before =
        static_cast<std::uint32_t>(m.guest_ram()[0x24 * 4 + 0]) |
        (static_cast<std::uint32_t>(m.guest_ram()[0x24 * 4 + 1]) << 8) |
        (static_cast<std::uint32_t>(m.guest_ram()[0x24 * 4 + 2]) << 16) |
        (static_cast<std::uint32_t>(m.guest_ram()[0x24 * 4 + 3]) << 24);
    CHECK(m.boot("PARMZ.COM"));
    m.step_instructions(500);
    CHECK(m.exited());
    CHECK(m.exit_code() == 0x55u);
    CHECK(m.dos().allocation_strategy() == 0u);
    const std::uint32_t ivt24_after =
        static_cast<std::uint32_t>(m.guest_ram()[0x24 * 4 + 0]) |
        (static_cast<std::uint32_t>(m.guest_ram()[0x24 * 4 + 1]) << 8) |
        (static_cast<std::uint32_t>(m.guest_ram()[0x24 * 4 + 2]) << 16) |
        (static_cast<std::uint32_t>(m.guest_ram()[0x24 * 4 + 3]) << 24);
    CHECK(ivt24_before == ivt24_after);
  }

  // Verify rejection of oversized .COM, modulo-wrap .COM, and out-of-bounds MZ relocations
  {
    san4::X86Machine m(cfg);
    CHECK(!m.boot("BIG1.COM"));
    CHECK(!m.boot("WRAP1M.COM"));

    std::uint16_t free_before = 0;
    (void)m.dos().mcb_alloc(0xFFFFu, 0x0900u, &free_before);
    CHECK(!m.dos().boot_program("BADREL.EXE"));
    std::uint16_t free_after = 0;
    (void)m.dos().mcb_alloc(0xFFFFu, 0x0900u, &free_after);
    CHECK(free_before == free_after);
  }
}

void test_vfs_traversal_cow_truncate_and_find_subdir() {
  ScopedTempDir tmp;
  const auto game_dir = tmp.path() / "game";
  const auto save_dir = tmp.path() / "save";
  std::filesystem::create_directories(game_dir / "SUBDIR");
  std::filesystem::create_directories(save_dir);

  {
    std::ofstream(tmp.path() / "OUTSIDE.S4") << "secret";
    std::ofstream(game_dir / "DATA.S4", std::ios::binary) << "hello world";
    std::ofstream(game_dir / "SUBDIR" / "MAP1.S4", std::ios::binary) << "m1";
    std::ofstream(game_dir / "SUBDIR" / "MAP2.S4", std::ios::binary) << "m2";
    std::ofstream(game_dir / "SUBDIR" / "NOTE.TXT", std::ios::binary) << "txt";
    std::error_code ec;
    std::filesystem::create_symlink(tmp.path() / "OUTSIDE.S4",
                                    game_dir / "SYM.S4", ec);
  }

  san4::CaseInsensitiveVfs vfs(game_dir, save_dir);
  CHECK(!vfs.resolve_for_read("..\\OUTSIDE.S4").has_value());
  CHECK(!vfs.resolve_for_read("SUBDIR\\..\\..\\OUTSIDE.S4").has_value());
  CHECK(!vfs.resolve_for_write("..\\ESCAPE.S4", true).has_value());
  CHECK(!vfs.resolve_for_read("SYM.S4").has_value());

  const auto root_s4 = vfs.find_matching("*.S4");
  CHECK(root_s4.size() == 1u && root_s4[0].dos_name == "DATA.S4");

  const auto sub_s4 = vfs.find_matching("subdir\\*.s4");
  CHECK(sub_s4.size() == 2u && sub_s4[0].dos_name == "MAP1.S4" &&
        sub_s4[1].dos_name == "MAP2.S4");

  // Exercise DosKernel copy-on-write, AH=40h CX=0 truncation, AH=4Eh/4Fh subdir,
  // and MS$KANJI / EMMXXXX0 device handles.
  san4::MachineConfig cfg;
  cfg.game_dir = game_dir;
  cfg.save_dir = save_dir;
  san4::X86Machine m(cfg);
  x86emu_t* emu = m.emu();

  // Place strings in guest RAM at 0x2000:0x0000..
  std::memcpy(m.guest_ram() + 0x20000u, "DATA.S4\0", 8u);
  std::memcpy(m.guest_ram() + 0x20020u, "NEW", 3u);
  std::memcpy(m.guest_ram() + 0x20040u, "SUBDIR\\*.S4\0", 12u);
  std::memcpy(m.guest_ram() + 0x20060u, "MS$KANJI\0", 9u);
  std::memcpy(m.guest_ram() + 0x20080u, "EMMXXXX0\0", 9u);
  x86emu_set_seg_register(emu, emu->x86.R_DS_SEL, 0x2000u);

  // Open "DATA.S4" read/write (copy-on-write to save_dir), write "NEW", truncate CX=0!
  emu->x86.R_AX = 0x3D02u;
  emu->x86.R_DX = 0x0000u;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u);
  const std::uint16_t fh = emu->x86.R_AX;

  emu->x86.R_AH = 0x40u;
  emu->x86.R_BX = fh;
  emu->x86.R_CX = 3u;
  emu->x86.R_DX = 0x0020u;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u && emu->x86.R_AX == 3u);

  // Truncate at current offset (3) via AH=40h CX=0
  emu->x86.R_AH = 0x40u;
  emu->x86.R_BX = fh;
  emu->x86.R_CX = 0u;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u);

  emu->x86.R_AH = 0x3Eu;
  emu->x86.R_BX = fh;
  m.dos().handle_int21(emu);

  std::string orig_txt;
  std::getline(std::ifstream(game_dir / "DATA.S4"), orig_txt);
  CHECK(orig_txt == "hello world");
  CHECK(std::filesystem::file_size(save_dir / "DATA.S4") == 3u);

  // Set DTA to 0x2000:0x0100 and enumerate SUBDIR\*.S4 via AH=4Eh/4Fh
  emu->x86.R_AH = 0x1Au;
  emu->x86.R_DX = 0x0100u;
  m.dos().handle_int21(emu);

  emu->x86.R_AH = 0x4Eu;
  emu->x86.R_CX = 0u;
  emu->x86.R_DX = 0x0040u;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u);
  CHECK(std::string(reinterpret_cast<const char*>(m.guest_ram() + 0x2011Eu)) ==
        "MAP1.S4");

  emu->x86.R_AH = 0x4Fu;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u);
  CHECK(std::string(reinterpret_cast<const char*>(m.guest_ram() + 0x2011Eu)) ==
        "MAP2.S4");

  emu->x86.R_AH = 0x4Fu;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) != 0u);

  // MS$KANJI fails open (CF=1); EMMXXXX0 succeeds and passes IOCTL 4400h / 4407h
  emu->x86.R_AX = 0x3D00u;
  emu->x86.R_DX = 0x0060u;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) != 0u);

  emu->x86.R_AX = 0x3D00u;
  emu->x86.R_DX = 0x0080u;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u);
  const std::uint16_t emm_fh = emu->x86.R_AX;

  emu->x86.R_AX = 0x4400u;
  emu->x86.R_BX = emm_fh;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u && emu->x86.R_DX == 0x0080u);

  emu->x86.R_AX = 0x4407u;
  emu->x86.R_BX = emm_fh;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u && emu->x86.R_AL == 0xFFu);
}

void test_vga_read_write_modes_pel_mask_and_reset() {
  san4::VesaPlanarVideo vid;
  vid.reset_mode(0x0102u);

  auto gc_out = [&](std::uint8_t idx, std::uint8_t val) {
    vid.port_out8(0x03CEu, idx);
    vid.port_out8(0x03CFu, val);
  };
  auto seq_out = [&](std::uint8_t idx, std::uint8_t val) {
    vid.port_out8(0x03C4u, idx);
    vid.port_out8(0x03C5u, val);
  };

  // 1) Write Mode 0: barrel rotate (rot=2) + Set/Reset + ALU XOR/OR/AND
  seq_out(0x02u, 0x0Fu);
  gc_out(0x05u, 0x00u);  // Write Mode 0, Read Mode 0
  gc_out(0x08u, 0xFFu);  // BitMask = 0xFF
  gc_out(0x01u, 0x01u);  // Enable Set/Reset on plane 0 only
  gc_out(0x00u, 0x01u);  // Plane 0 SR = 0xFF
  gc_out(0x03u, 0x02u);  // Rotate right by 2, ALU = replace (0)
  // CPU byte 0x80 rotated right by 2 -> 0x20 (pixel 2 gets plane 1..3 = 1 => color 0x0F,
  // other pixels get plane 0 = 1 => color 0x01!)
  vid.vram_write8(0u, 0x80u);
  CHECK(vid.shadow_vram()[2] == 0x0Fu);
  CHECK(vid.shadow_vram()[0] == 0x01u);

  // ALU XOR (`3 << 3 = 0x18`) on plane 0..3 with Enable SR = 0, rot = 0
  gc_out(0x01u, 0x00u);
  gc_out(0x03u, 0x18u);  // XOR
  (void)vid.vram_read8(0u);  // Fill latches
  vid.vram_write8(0u, 0x80u);  // XOR pixel 0 (0x01 ^ 0x0F -> 0x0E)
  CHECK(vid.shadow_vram()[0] == 0x0Eu);

  // 2) Write Mode 1: 32-bit latch copy from byte 0 -> byte 1 (x = 8..15)
  gc_out(0x03u, 0x00u);
  (void)vid.vram_read8(0u);
  gc_out(0x05u, 0x01u);  // Write Mode 1
  vid.vram_write8(1u, 0x00u);
  for (int x = 0; x < 8; ++x) {
    CHECK(vid.shadow_vram()[8 + x] == vid.shadow_vram()[x]);
  }

  // 3) Read Mode 0 (`GC[4]`) & Read Mode 1 (`GC[2]` Color Compare + `GC[7]` Color Don't Care)
  gc_out(0x05u, 0x00u);
  gc_out(0x04u, 0x00u);  // Read plane 0: pixel 0 is 0x0E (bit0=0), pixels 1..7 have bit0=1 -> 0x7F
  CHECK(vid.vram_read8(0u) == 0x7Fu);

  gc_out(0x05u, 0x08u);  // Read Mode 1
  gc_out(0x02u, 0x0Eu);  // Color Compare = 0x0E
  gc_out(0x07u, 0x0Fu);  // Care about all 4 planes -> only pixel 0 (bit 7 = 0x80) matches!
  CHECK(vid.vram_read8(0u) == 0x80u);
  gc_out(0x07u, 0x00u);  // Don't care about any planes -> all 8 pixels match (0xFF)!
  CHECK(vid.vram_read8(0u) == 0xFFu);

  // 4) `0x3C6` Pel Mask blank/unblank + `AX=1002h/1009h` overscan + `reset_mode` cleanup
  const std::uint32_t orig_c15 = vid.live_palette_argb()[15];
  CHECK(orig_c15 == 0xFFFFFFFFu);
  vid.port_out8(0x03C6u, 0x00u);
  CHECK(vid.live_palette_argb()[15] == vid.live_palette_argb()[0]);
  vid.port_out8(0x03C6u, 0xFFu);
  CHECK(vid.live_palette_argb()[15] == orig_c15);

  std::vector<std::uint8_t> ram(0x10000u, 0u);
  for (std::uint8_t i = 0; i < 16u; ++i) ram[0x0200u + i] = i;
  ram[0x0200u + 16u] = 0x2Au;  // 17th overscan byte
  std::uint16_t ax = 0x1002u, bx = 0, cx = 0, dx = 0x0200u, es = 0, bp = 0;
  vid.handle_int10(ax, bx, cx, dx, es, bp, 0, ram, 0xF000u, 0xB000u);
  ax = 0x1009u;
  dx = 0x0300u;
  vid.handle_int10(ax, bx, cx, dx, es, bp, 0, ram, 0xF000u, 0xB000u);
  CHECK(ram[0x0300u + 16u] == 0x2Au);

  // Write non-zero DAC entry 100 and pel_mask 0x0F, then verify reset_mode clears both!
  vid.port_out8(0x03C8u, 100u);
  vid.port_out8(0x03C9u, 45u);
  vid.port_out8(0x03C9u, 50u);
  vid.port_out8(0x03C9u, 55u);
  vid.port_out8(0x03C6u, 0x0Fu);
  vid.reset_mode(0x0102u);
  CHECK(vid.port_in8(0x03C6u) == 0xFFu);
  ax = 0x1015u;
  bx = 100u;
  vid.handle_int10(ax, bx, cx, dx, es, bp, 0, ram, 0xF000u, 0xB000u);
  CHECK(dx == 0u && cx == 0u);
}

void test_offscreen_clipping_and_hd_pack_bounds() {
  san4::VesaPlanarVideo vid;
  vid.reset_mode(0x0102u);

  // Completely off-screen glyph rects must leave border tiles untouched.
  vid.mark_glyph_rect(-32, 0, 16, 16);
  vid.mark_glyph_rect(816, 0, 16, 16);
  vid.mark_glyph_rect(0, -32, 16, 16);
  vid.mark_glyph_rect(0, 616, 16, 16);
  for (std::uint8_t t : vid.live_glyph_tiles()) {
    CHECK(t == 0u);
  }

  // Partially negative rect (-8, -8, 16, 16) marks only tile (0, 0).
  vid.mark_glyph_rect(-8, -8, 16, 16);
  CHECK(vid.live_glyph_tiles()[0] == 1u);
  CHECK(vid.live_glyph_tiles()[1] == 0u);
  vid.clear_glyph_rect(-32, 0, 16, 16);
  CHECK(vid.live_glyph_tiles()[0] == 1u);
  vid.clear_glyph_rect(-8, -8, 16, 16);
  CHECK(vid.live_glyph_tiles()[0] == 0u);

  // Strict sprite filename & PNG dimension validation
  int pw = 0, ph = 0;
  std::uint64_t phash = 0;
  CHECK(san4::HdPackManager::parse_sprite_filename(
      "img_64x80_0123456789abcdef.png", pw, ph, phash));
  CHECK(pw == 64 && ph == 80 && phash == 0x0123456789ABCDEFull);
  CHECK(!san4::HdPackManager::parse_sprite_filename("img_0x80_abcd.png", pw, ph, phash));
  CHECK(!san4::HdPackManager::parse_sprite_filename("img_8193x80_abcd.png", pw, ph, phash));
  CHECK(!san4::HdPackManager::parse_sprite_filename("img_+64x80_abcd.png", pw, ph, phash));
  CHECK(!san4::HdPackManager::parse_sprite_filename("img_-64x80_abcd.png", pw, ph, phash));
  CHECK(!san4::HdPackManager::parse_sprite_filename("img_64x80_0x1234.png", pw, ph, phash));
  CHECK(!san4::HdPackManager::parse_sprite_filename("img_64x80_1234zz.png", pw, ph, phash));
  CHECK(!san4::HdPackManager::parse_sprite_filename("img_64x80_1234_extra.png", pw, ph, phash));

  ScopedTempDir tmp;
  const std::uint32_t one_px = 0xFFFFFFFFu;
  CHECK(!san4::HdPackManager::save_argb_png(tmp.path() / "z.png", 0, 10, &one_px));
  CHECK(!san4::HdPackManager::save_argb_png(tmp.path() / "b.png", 8193, 1, &one_px));

  // Verify partially off-screen blit prunes covered ActiveOverlay AND on_vram_copy_rect caps at 64
  const auto dump_dir = tmp.path() / "dump";
  const auto pack_dir = tmp.path() / "pack";
  std::filesystem::create_directories(pack_dir);

  san4::HdPackManager hd;
  hd.set_dump_dir(dump_dir);
  hd.on_planar_blit_complete(16, 16, 8, 8, vid);
  std::filesystem::path dumped;
  for (const auto& ent : std::filesystem::directory_iterator(dump_dir)) {
    dumped = ent.path();
  }
  CHECK(!dumped.empty());
  const std::vector<std::uint32_t> hd_tile(16 * 16, 0xFF00FF00u);
  CHECK(san4::HdPackManager::save_argb_png(pack_dir / dumped.filename(), 16, 16,
                                           hd_tile.data()));
  hd.set_dumping_enabled(false);
  hd.set_pack_dir(pack_dir);
  CHECK(hd.loaded_texture_count() == 1u);

  hd.on_planar_blit_complete(16, 16, 8, 8, vid);
  CHECK(hd.active_overlay_count() == 1u);
  // Partially off-screen blit [-4, 28) x [12, 44) covers (16, 16, 8, 8)!
  hd.on_planar_blit_complete(-4, 12, 32, 32, vid);
  CHECK(hd.active_overlay_count() == 0u);

  // Populate 40 non-overlapping 8x8 overlays in left half and duplicate twice via on_vram_copy_rect
  for (int i = 0; i < 40; ++i) {
    const int ox = (i % 10) * 16;
    const int oy = (i / 10) * 16;
    hd.on_planar_blit_complete(ox, oy, 8, 8, vid);
  }
  CHECK(hd.active_overlay_count() == 40u);
  hd.on_vram_copy_rect(0, 0, 200, 100, 200, 0);
  CHECK(hd.active_overlay_count() == 80u);
}

void test_mouse_pit_readback_and_concurrency_stress() {
  // 1) MouseHle AX=0004h warp -> 0 mickeys on AX=000Bh, and AX=0005h BX=99 guard
  {
    san4::X86Machine m;
    x86emu_t* emu = m.emu();
    bool status_poll = false;
    emu->x86.R_AX = 0x0000u;
    m.mouse().handle_int33(emu, status_poll);

    emu->x86.R_AX = 0x0004u;
    emu->x86.R_CX = 620u;
    emu->x86.R_DX = 440u;
    m.mouse().handle_int33(emu, status_poll);

    emu->x86.R_AX = 0x000Bu;
    m.mouse().handle_int33(emu, status_poll);
    CHECK(emu->x86.R_CX == 0u && emu->x86.R_DX == 0u);

    m.mouse().set_button(2, true);
    m.mouse().set_button(2, false);
    emu->x86.R_AX = 0x0005u;
    emu->x86.R_BX = 99u;  // Out-of-range button index must not clobber button 2!
    m.mouse().handle_int33(emu, status_poll);
    CHECK(emu->x86.R_BX == 0u && emu->x86.R_CX == 0u && emu->x86.R_DX == 0u);

    emu->x86.R_AX = 0x0005u;
    emu->x86.R_BX = 2u;
    m.mouse().handle_int33(emu, status_poll);
    CHECK(emu->x86.R_BX == 1u);

    // PIT Ch0 live counter-latch readback (`OUT 0x43, 0x00` + `IN AL, 0x40`)
    static constexpr std::uint8_t kPitLatchCode[] = {
        0xB0, 0x36,        // mov al, 0x36
        0xE6, 0x43,        // out 0x43, al
        0xB0, 0x00,        // mov al, 0x00
        0xE6, 0x40,        // out 0x40, al
        0xB0, 0x10,        // mov al, 0x10 (divisor = 0x1000)
        0xE6, 0x40,        // out 0x40, al
        0x30, 0xC0,        // xor al, al (Counter Latch Command Ch0)
        0xE6, 0x43,        // out 0x43, al
        0xE4, 0x40,        // in al, 0x40
        0x88, 0xC3,        // mov bl, al
        0xE4, 0x40,        // in al, 0x40
        0x88, 0xC7,        // mov bh, al
        0xB9, 0x40, 0x00,  // mov cx, 64
        0xE2, 0xFE,        // loop $
        0x30, 0xC0,        // xor al, al
        0xE6, 0x43,        // out 0x43, al
        0xE4, 0x40,        // in al, 0x40
        0x88, 0xC2,        // mov dl, al
        0xE4, 0x40,        // in al, 0x40
        0x88, 0xC6,        // mov dh, al
        0xF4,              // hlt
    };
    m.load_synthetic_code(0x2000u, 0x0100u, kPitLatchCode);
    m.step_instructions(200);
    const std::uint16_t c1 = m.emu()->x86.R_BX;
    const std::uint16_t c2 = m.emu()->x86.R_DX;
    CHECK(c1 > c2 && c2 > 0u && c1 <= 0x1000u);
  }

  // 2) Concurrent Opl2Audio SPSC producer/consumer stress (TSan verification)
  {
    san4::Opl2Audio opl;
    opl.port_out8(0x388u, 0x20u);
    opl.port_out8(0x389u, 0x01u);
    opl.port_out8(0x388u, 0x23u);
    opl.port_out8(0x389u, 0x01u);
    opl.port_out8(0x388u, 0x63u);
    opl.port_out8(0x389u, 0xF0u);
    opl.port_out8(0x388u, 0xA0u);
    opl.port_out8(0x389u, 0x98u);
    opl.port_out8(0x388u, 0xB0u);
    opl.port_out8(0x389u, 0x31u);

    std::atomic<bool> prod_done{false};
    std::uint64_t consumed_non_zero = 0;

    std::thread consumer([&]() {
      std::int16_t buf[256 * 2];
      while (!prod_done.load(std::memory_order_acquire) ||
             opl.buffered_frames() > 0u) {
        const std::uint32_t avail = opl.buffered_frames();
        if (avail > 0u) {
          const std::uint32_t n =
              opl.pull_stereo_s16(buf, std::min<std::uint32_t>(avail, 256u));
          for (std::uint32_t i = 0; i < n * 2u; ++i) {
            if (buf[i] != 0) {
              ++consumed_non_zero;
            }
          }
        } else {
          std::this_thread::yield();
        }
      }
    });

    for (int i = 0; i < 60; ++i) {
      opl.advance_seconds(0.002);
    }
    // Also verify NaN / negative dt guards are no-ops
    const std::uint64_t before_guard = opl.total_rendered_frames();
    opl.advance_seconds(-1.0);
    opl.advance_seconds(0.0);
    CHECK(opl.total_rendered_frames() == before_guard);

    prod_done.store(true, std::memory_order_release);
    consumer.join();
    CHECK(consumed_non_zero > 100u);
    CHECK(opl.peak_abs_sample() > 100);
  }

    // 3) PixelUpscaler 4-worker multi-mode stress test (TSan verification)
  {
    san4::VesaPlanarVideo vid;
    vid.reset_mode(0x0102u);
    san4::PixelUpscaler up(4);
    for (int iter = 0; iter < 10; ++iter) {
      vid.vram_write8(static_cast<std::uint32_t>(iter * 200u),
                      static_cast<std::uint8_t>(0xA5u ^ iter));
      vid.latch_frame();
      if ((iter % 2) == 1) {
        up.cycle_mode();
      }
      const auto rect = up.upscale_from_video(vid, false);
      CHECK(rect.has_changes);
    }
  }
}

void test_hd_pack_v2_codecs_upscaler_canvas_and_grading() {
  auto encode_interleaved_from_slots = [](int w, int h, int planes,
                                          const std::uint8_t* vga_slots) {
    const int cols = (w + 7) / 8;
    std::vector<std::uint8_t> out(
        static_cast<std::size_t>(cols * planes * h), 0u);
    for (int y = 0; y < h; ++y) {
      for (int col = 0; col < cols; ++col) {
        std::uint8_t pb[4]{0u, 0u, 0u, 0u};
        for (int bit = 0; bit < 8; ++bit) {
          const int x = col * 8 + bit;
          if (x >= w) break;
          const std::uint8_t slot =
              static_cast<std::uint8_t>(vga_slots[y * w + x] & 0x0Fu);
          // kGrpSlotMap is an involution: slot -> game_color uses kGrpSlotMap.
          const std::uint8_t gc = san4::kGrpSlotMap[slot];
          const unsigned sh = static_cast<unsigned>(7 - bit);
          for (int p = 0; p < planes; ++p) {
            pb[p] |= static_cast<std::uint8_t>(((gc >> p) & 1u) << sh);
          }
        }
        std::uint8_t* dst =
            out.data() + static_cast<std::size_t>((y * cols + col) * planes);
        for (int p = 0; p < planes; ++p) {
          dst[p] = pb[p];
        }
      }
    }
    return out;
  };

  auto format_pal_csv = [](const std::array<std::uint32_t, 16>& pal) {
    std::string s;
    char buf[16];
    for (int i = 0; i < 16; ++i) {
      if (i > 0) s.push_back(',');
      std::snprintf(buf, sizeof(buf), "%06X", pal[static_cast<std::size_t>(i)] & 0x00FFFFFFu);
      s += buf;
    }
    return s;
  };

  // 1) Clean-room codecs: LS11, NPK016, MONDATA compositing, masked hash
  {
    std::vector<std::uint8_t> ls11(0x0110u + 16u + 3u, 0u);
    ls11[0] = 'L';
    ls11[1] = 'S';
    ls11[2] = '1';
    ls11[3] = '1';
    ls11[0x0010u + 0u] = 0xAAu;
    ls11[0x0010u + 1u] = 0x55u;
    // Entry 0: comp_len = 3, uncomp_len = 6, file_off = 0x0120
    ls11[0x0113u] = 3u;
    ls11[0x0117u] = 6u;
    ls11[0x011Au] = 0x01u;
    ls11[0x011Bu] = 0x20u;
    // Bitstream: code=0 (00), code=1 (01), code=258/dist=2 (11111110 + 00000100), len=1->4 (01)
    ls11[0x0120u] = 0x1Fu;
    ls11[0x0121u] = 0xE0u;
    ls11[0x0122u] = 0x44u;
    CHECK(san4::codecs::is_ls11_archive(ls11));
    std::vector<std::vector<std::uint8_t>> recs;
    CHECK(san4::codecs::decompress_ls11_archive(ls11, recs));
    CHECK(recs.size() == 1u && recs[0].size() == 6u);
    for (std::size_t i = 0; i < 6u; ++i) {
      CHECK(recs[0][i] == ((i & 1u) ? 0x55u : 0xAAu));
    }

    // NPK016 8x2 image with horizontal + vertical ring-buffer references
    std::vector<std::uint8_t> npk_file(48u + 5u, 0u);
    std::memcpy(npk_file.data(), "NPK016", 6u);
    npk_file[6] = 4u;
    npk_file[8] = 8u;
    npk_file[10] = 2u;
    npk_file[12] = 8u;
    npk_file[14] = 2u;
    // Game color 6 -> 0x0GRB = 0x08B3
    npk_file[16u + 6u * 2u] = 0xB3u;
    npk_file[16u + 6u * 2u + 1u] = 0x08u;
    npk_file[48] = 0x06u;
    npk_file[49] = 0x0Fu;
    npk_file[50] = 0xF0u;
    npk_file[51] = 0x00u;
    npk_file[52] = 0x81u;
    std::vector<san4::codecs::NpkImage> npk_imgs;
    san4::codecs::scan_and_decode_npk_images(npk_file, npk_imgs);
    CHECK(npk_imgs.size() == 1u);
    CHECK(npk_imgs[0].w == 8 && npk_imgs[0].h == 2);
    for (std::uint8_t s : npk_imgs[0].vga_slots) {
      CHECK(s == san4::kGrpSlotMap[6]);
    }
  }

  // 2) Artistic 4x upscaler: checkerboard de-dither, 1px catchlight lock, clean masked alpha
  {
    const auto pal = san4::codecs::default_san4_master_palette();
    std::vector<std::uint8_t> dither_slots(16u * 16u, 0u);
    const std::uint8_t c_dark = san4::kGrpSlotMap[11];
    const std::uint8_t c_light = san4::kGrpSlotMap[14];
    for (int y = 0; y < 16; ++y) {
      for (int x = 0; x < 16; ++x) {
        dither_slots[static_cast<std::size_t>(y * 16 + x)] =
            ((x + y) & 1) ? c_light : c_dark;
      }
    }
    // Place an isolated 1px white eye catchlight at (4, 4) surrounded by dark ink
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        dither_slots[static_cast<std::size_t>((4 + dy) * 16 + (4 + dx))] =
            san4::kGrpSlotMap[8];
      }
    }
    dither_slots[static_cast<std::size_t>(4 * 16 + 4)] = san4::kGrpSlotMap[15];

    std::vector<std::uint32_t> hd_port;
    san4::hd_gen::upscale_art_4x_argb(
        dither_slots.data(), nullptr, 16, 16, pal,
        san4::hd_gen::ArtAssetKind::Portrait, hd_port);
    CHECK(hd_port.size() == 64u * 64u);
    // Interior checkerboard at (11, 11) vs (12, 11) should be smoothly de-dithered!
    const std::uint32_t p_a = hd_port[static_cast<std::size_t>((11 * 4 + 1) * 64 + (11 * 4 + 1))];
    const std::uint32_t p_b = hd_port[static_cast<std::size_t>((11 * 4 + 1) * 64 + (12 * 4 + 1))];
    const int diff_r = std::abs(static_cast<int>((p_a >> 16) & 0xFFu) -
                                static_cast<int>((p_b >> 16) & 0xFFu));
    CHECK(diff_r <= 18);
    // Catchlight center at (4*4+1, 4*4+1) must stay crisp and bright!
    const std::uint32_t p_eye = hd_port[static_cast<std::size_t>((4 * 4 + 1) * 64 + (4 * 4 + 1))];
    CHECK(((p_eye >> 16) & 0xFFu) >= 200u);

    // Masked sprite: exterior alpha == 0, interior alpha == 255
    std::vector<std::uint8_t> spr_slots(16u * 16u, 0u);
    std::vector<std::uint8_t> spr_mask(16u * 16u, 0u);
    for (int y = 4; y < 12; ++y) {
      for (int x = 4; x < 12; ++x) {
        spr_slots[static_cast<std::size_t>(y * 16 + x)] = san4::kGrpSlotMap[10];
        spr_mask[static_cast<std::size_t>(y * 16 + x)] = 1u;
      }
    }
    std::vector<std::uint32_t> hd_spr;
    san4::hd_gen::upscale_art_4x_argb(
        spr_slots.data(), spr_mask.data(), 16, 16, pal,
        san4::hd_gen::ArtAssetKind::MaskedSprite, hd_spr);
    CHECK(((hd_spr[0] >> 24) & 0xFFu) == 0u);
    CHECK(((hd_spr[static_cast<std::size_t>(32 * 64 + 32)] >> 24) & 0xFFu) == 255u);
  }

  // 3) < 30 ms lazy indexing (3,000 manifest entries), LRU eviction, MONDATA,
  //    Button, > 200 simultaneous 16x16 tiles, tile_dict dynamic compositing,
  //    same-color vram_write8 occlusion, SavedHdPatch, AH=0x13 crisp 4x glyphs,
  //    masked sprite background independence, and 3D LUT crossfade/seasonal grading!
  ScopedTempDir tmp;
  const auto pack_dir = tmp.path() / "hd_pack";
  std::filesystem::create_directories(pack_dir / "tiles");
  std::filesystem::create_directories(pack_dir / "mondata");
  std::filesystem::create_directories(pack_dir / "sprites" / "buttons");
  std::filesystem::create_directories(pack_dir / "sprites" / "units");

  const auto master_pal = san4::codecs::default_san4_master_palette();
  // Create a spring green terrain reference palette for slots 0..7
  std::array<std::uint8_t, 24> spring_brg = {
      0, 0, 0,   6, 1, 3,   2, 3, 9,   3, 5, 11,
      2, 2, 7,   8, 3, 8,   3, 10, 9,  8, 12, 12,
  };
  const auto spring_pal =
      san4::codecs::apply_brg_subpalette(master_pal, spring_brg, 0, 8);
  const std::string master_csv = format_pal_csv(master_pal);
  const std::string spring_csv = format_pal_csv(spring_pal);

  // Create 5 distinct 16x16 opaque tiles and their 64x64 HD PNGs
  std::array<std::array<std::uint8_t, 256>, 5> tile_slots{};
  std::array<std::uint64_t, 5> tile_hashes{};
  for (int ti = 0; ti < 5; ++ti) {
    for (int p = 0; p < 256; ++p) {
      const std::uint8_t gc = static_cast<std::uint8_t>(1 + ((p + ti) % 6));
      tile_slots[static_cast<std::size_t>(ti)][static_cast<std::size_t>(p)] =
          san4::kGrpSlotMap[gc];
    }
    tile_hashes[static_cast<std::size_t>(ti)] =
        san4::HdPackManager::hash_sprite_4bit(
            16, 16, tile_slots[static_cast<std::size_t>(ti)].data());
    const std::uint32_t hd_col =
        (ti == 0) ? spring_pal[san4::kGrpSlotMap[2]]  // vivid spring green
                  : (0xFF102000u | (static_cast<std::uint32_t>(ti * 45) << 16) |
                     static_cast<std::uint32_t>(ti * 35));
    std::vector<std::uint32_t> hd_px(64u * 64u, hd_col);
    CHECK(san4::HdPackManager::save_argb_png(
        pack_dir / "tiles" / ("tile_" + std::to_string(ti) + ".png"), 64, 64,
        hd_px.data()));
  }

  // Create 1 masked 16x16 city overlay for tile_dict.bin + masked unit test
  std::array<std::uint8_t, 256> ov_slots{};
  std::array<std::uint8_t, 256> ov_mask{};
  int ov_nz = 0;
  for (int y = 4; y < 12; ++y) {
    for (int x = 4; x < 12; ++x) {
      ov_slots[static_cast<std::size_t>(y * 16 + x)] = san4::kGrpSlotMap[6];
      ov_mask[static_cast<std::size_t>(y * 16 + x)] = 1u;
      ++ov_nz;
    }
  }
  const std::uint64_t ov_masked_hash = san4::codecs::hash_masked_sprite_4bit(
      16, 16, ov_slots.data(), ov_mask.data());
  std::vector<std::uint32_t> ov_hd_px(64u * 64u, 0x00000000u);
  for (int y = 16; y < 48; ++y) {
    for (int x = 16; x < 48; ++x) {
      ov_hd_px[static_cast<std::size_t>(y * 64 + x)] = 0xFFEEDD33u;
    }
  }
  CHECK(san4::HdPackManager::save_argb_png(
      pack_dir / "sprites" / "units" / "ov_city.png", 64, 64, ov_hd_px.data()));

  // Write tile_dict.bin (base tile 0 + overlay city)
  {
    std::array<san4::codecs::PackedTileDictEntry, 2> dict{};
    dict[0].hd_hash = tile_hashes[0];
    dict[0].archive_group = 0u;
    dict[0].is_overlay = 0u;
    dict[0].non_zero_count = 255u;
    dict[0].full_zero_256 = 1u;
    for (int i = 0; i < 128; ++i) {
      dict[0].packed_slots[i] = static_cast<std::uint8_t>(
          (tile_slots[0][static_cast<std::size_t>(i * 2)] & 0x0Fu) |
          ((tile_slots[0][static_cast<std::size_t>(i * 2 + 1)] & 0x0Fu) << 4));
    }
    std::memset(dict[0].zero_mask, 0xFF, 32u);

    dict[1].hd_hash = ov_masked_hash;
    dict[1].archive_group = 0u;
    dict[1].is_overlay = 1u;
    dict[1].non_zero_count = static_cast<std::uint8_t>(ov_nz);
    for (int i = 0; i < 128; ++i) {
      dict[1].packed_slots[i] = static_cast<std::uint8_t>(
          (ov_slots[static_cast<std::size_t>(i * 2)] & 0x0Fu) |
          ((ov_slots[static_cast<std::size_t>(i * 2 + 1)] & 0x0Fu) << 4));
    }
    for (int i = 0; i < 256; ++i) {
      if (ov_mask[static_cast<std::size_t>(i)]) {
        dict[1].zero_mask[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
      }
    }
    std::ofstream tfs(pack_dir / "tiles" / "tile_dict.bin", std::ios::binary);
    tfs.write(reinterpret_cast<const char*>(dict.data()), sizeof(dict));
  }

  // Write MONDATA synthetic parts + mondata_index.bin
  std::vector<std::uint8_t> mondata_slots(64u * 80u, san4::kGrpSlotMap[9]);
  mondata_slots[0] = san4::kGrpSlotMap[14];
  const std::uint64_t mondata_hash =
      san4::HdPackManager::hash_sprite_4bit(64, 80, mondata_slots.data());
  {
    const std::vector<std::uint32_t> top_px(256u * 144u, 0xFF224488u);
    const std::vector<std::uint32_t> bot_px(256u * 176u, 0xFF4466AAu);
    std::vector<std::uint32_t> eye_px(256u * 64u, 0x00000000u);
    std::fill_n(eye_px.data() + 256u * 8u, 256u * 8u, 0xFFFFEECCu);
    std::vector<std::uint32_t> mouth_px(256u * 80u, 0x00000000u);
    std::fill_n(mouth_px.data() + 256u * 8u, 256u * 8u, 0xFFCC3344u);
    CHECK(san4::HdPackManager::save_argb_png(
        pack_dir / "mondata" / "t00_top_0.png", 256, 144, top_px.data()));
    CHECK(san4::HdPackManager::save_argb_png(
        pack_dir / "mondata" / "t00_bot_0.png", 256, 176, bot_px.data()));
    CHECK(san4::HdPackManager::save_argb_png(
        pack_dir / "mondata" / "t00_eye_0.png", 256, 64, eye_px.data()));
    CHECK(san4::HdPackManager::save_argb_png(
        pack_dir / "mondata" / "t00_mouth_0.png", 256, 80, mouth_px.data()));

    san4::MondataIndexEntry me{};
    me.hash64 = mondata_hash;
    me.tmpl = 0u;
    me.top = 0u;
    me.bot = 0u;
    me.eye = 0u;
    me.mouth = 0u;
    me.eye_y = 20u;
    me.mouth_y = 42u;
    std::ofstream mfs(pack_dir / "mondata" / "mondata_index.bin",
                      std::ios::binary);
    mfs.write(reinterpret_cast<const char*>(&me), sizeof(me));
  }

  // Write Button synthetic base + label + button_index.bin
  std::vector<std::uint8_t> btn_slots(32u * 32u, san4::kGrpSlotMap[13]);
  btn_slots[3] = san4::kGrpSlotMap[15];
  const std::uint64_t btn_hash =
      san4::HdPackManager::hash_sprite_4bit(32, 32, btn_slots.data());
  {
    const std::vector<std::uint32_t> base_px(128u * 128u, 0xFF335577u);
    const std::vector<std::uint32_t> lbl_px(128u * 64u, 0xFFDDBB55u);
    CHECK(san4::HdPackManager::save_argb_png(
        pack_dir / "sprites" / "buttons" / "base_00.png", 128, 128,
        base_px.data()));
    CHECK(san4::HdPackManager::save_argb_png(
        pack_dir / "sprites" / "buttons" / "label_00.png", 128, 64,
        lbl_px.data()));

    san4::ButtonIndexEntry be{};
    be.hash64 = btn_hash;
    be.base_id = 0u;
    be.label_id = 0u;
    std::ofstream bfs(pack_dir / "sprites" / "buttons" / "button_index.bin",
                      std::ios::binary);
    bfs.write(reinterpret_cast<const char*>(&be), sizeof(be));
  }

  // Write hd_manifest.tsv with 3,000 entries to benchmark lazy startup indexing!
  {
    std::ofstream mf(pack_dir / "hd_manifest.tsv", std::ios::binary);
    char hbuf[32];
    for (int ti = 0; ti < 5; ++ti) {
      std::snprintf(hbuf, sizeof(hbuf), "%016llx",
                    static_cast<unsigned long long>(tile_hashes[static_cast<std::size_t>(ti)]));
      mf << hbuf << "\t16\t16\t2\ttiles/tile_" << ti << ".png\t" << spring_csv
         << "\n";
    }
    std::snprintf(hbuf, sizeof(hbuf), "%016llx",
                  static_cast<unsigned long long>(ov_masked_hash));
    mf << hbuf << "\t16\t16\t2\tsprites/units/ov_city.png\t" << spring_csv
       << "\n";
    for (int dummy = 0; dummy < 2994; ++dummy) {
      std::snprintf(hbuf, sizeof(hbuf), "%016llx",
                    0xDEADBEEF00000000ull + static_cast<unsigned long long>(dummy));
      mf << hbuf << "\t16\t16\t0\ttiles/lazy_unloaded_" << dummy << ".png\t"
         << master_csv << "\n";
    }
  }

  san4::HdPackManager hd;
  hd.set_pack_dir(pack_dir);
  // 3,000 manifest + 1 mondata + 1 button = 3,002 indexed entries, 0 bytes decoded upfront!
  CHECK(hd.loaded_texture_count() == 3002u);
  CHECK(hd.cache_resident_bytes() == 0u);
  CHECK(hd.last_reload_ms() < 50.0);

  // Set up VesaPlanarVideo with matching spring_pal + master_pal
  san4::VesaPlanarVideo vid;
  vid.reset_mode(0x0102u);
  auto set_video_brg48 = [&](const std::array<std::uint8_t, 48>& brg48) {
    vid.begin_smooth_fade(0u, 16u, brg48.data(), brg48.data(),
                          san4::kGrpSlotMap.data(), 0.01);
    vid.finish_fade_immediately();
    vid.latch_frame();
    vid.clear_hd_occlude_bits();
  };
  std::array<std::uint8_t, 48> full_spring_brg{};
  std::copy(spring_brg.begin(), spring_brg.end(), full_spring_brg.begin());
  // Portrait/UI colors 8..15 matching default_san4_master_palette()
  static constexpr std::array<std::uint8_t, 24> kUpperMasterBrg = {
      2, 2, 2,  8, 4, 5,  3, 11, 3,   4,  9,  6,
      5, 5, 8,  9, 8, 9,  8, 13, 10,  13, 15, 14,
  };
  std::copy(kUpperMasterBrg.begin(), kUpperMasterBrg.end(),
            full_spring_brg.begin() + 24);
  set_video_brg48(full_spring_brg);

  const san4::GrpClipRect full_clip{0, 0, 799, 599};

  // Test bounded LRU eviction: 64 KB cap fits at most four 64x64 (16 KB) textures
  hd.set_max_cache_bytes(64u * 1024u);
  for (int ti = 0; ti < 5; ++ti) {
    const auto raw = encode_interleaved_from_slots(
        16, 16, 4, tile_slots[static_cast<std::size_t>(ti)].data());
    hd.on_planar_blit_source(ti * 16, 0, 16, 16, 4, 0u, 0u, 0u, 0u, full_clip,
                             raw, vid);
  }
  CHECK(hd.cache_evictions() >= 1u);
  CHECK(hd.cache_resident_bytes() <= hd.max_cache_bytes());
  hd.set_max_cache_bytes(san4::HdPackManager::kDefaultMaxCacheBytes);

  // Test > 200 simultaneous 16x16 HD tiles + overlap-safe AH=0x17 copy_rect!
  hd.clear_overlays();
  const auto tile0_raw = encode_interleaved_from_slots(16, 16, 4, tile_slots[0].data());
  for (int idx = 0; idx < 220; ++idx) {
    const int tx = (idx % 22) * 16;
    const int ty = (idx / 22) * 16;
    hd.on_planar_blit_source(tx, ty, 16, 16, 4, 0u, 0u, 0u, 0u, full_clip,
                             tile0_raw, vid);
  }
  CHECK(hd.active_overlay_count() >= 220u);
  hd.on_vram_copy_rect(0, 0, 160, 160, 32, 0);
  CHECK(hd.active_overlay_count() >= 220u);

  // Test dynamic MONDATA (64x80 -> 256x320) & Button (32x32 -> 128x128) compositing
  const auto mon_raw = encode_interleaved_from_slots(64, 80, 3, mondata_slots.data());
  hd.on_planar_blit_source(400, 0, 64, 80, 3, 1u, 0xFFu, 0u, 0u, full_clip,
                           mon_raw, vid);
  const auto btn_raw = encode_interleaved_from_slots(32, 32, 3, btn_slots.data());
  hd.on_planar_blit_source(500, 0, 32, 32, 3, 1u, 0xFFu, 0u, 0u, full_clip,
                           btn_raw, vid);

  // Test dynamic 16x16 cell compositing via tile_dict.bin (base tile 0 + city overlay + 2 road px)
  auto custom_cell_slots = tile_slots[0];
  for (int p = 0; p < 256; ++p) {
    if (ov_mask[static_cast<std::size_t>(p)]) {
      custom_cell_slots[static_cast<std::size_t>(p)] = ov_slots[static_cast<std::size_t>(p)];
    }
  }
  custom_cell_slots[2] = san4::kGrpSlotMap[7];
  custom_cell_slots[3] = san4::kGrpSlotMap[7];
  const auto cell_raw = encode_interleaved_from_slots(16, 16, 4, custom_cell_slots.data());
  hd.on_planar_blit_source(600, 0, 16, 16, 4, 0u, 0u, 0u, 0u, full_clip,
                           cell_raw, vid);

  // Test canonical masked sprite (planar_rop == 4) matching identically over TWO different terrains!
  const auto tile1_raw = encode_interleaved_from_slots(16, 16, 4, tile_slots[1].data());
  hd.on_planar_blit_source(640, 0, 16, 16, 4, 0u, 0u, 0u, 0u, full_clip,
                           tile0_raw, vid);
  hd.on_planar_blit_source(656, 0, 16, 16, 4, 0u, 0u, 0u, 0u, full_clip,
                           tile1_raw, vid);
  const auto ov_raw = encode_interleaved_from_slots(16, 16, 4, ov_slots.data());
  hd.on_planar_blit_source(640, 0, 16, 16, 4, 0u, 0u, 4u, 0u, full_clip, ov_raw,
                           vid);
  hd.on_planar_blit_source(656, 0, 16, 16, 4, 0u, 0u, 4u, 0u, full_clip, ov_raw,
                           vid);

  // Test AH=0x13 crisp 4x monochrome glyph over HD tile at (0, 0)
  std::vector<std::uint8_t> glyph_1bit(32u, 0u);
  glyph_1bit[8] = 0x7Eu;
  glyph_1bit[9] = 0x7Eu;
  hd.on_mono_blit_1bit(0, 0, 16, 16, 4u, san4::kGrpSlotMap[15], 0u, 0u,
                       full_clip, glyph_1bit, vid);

  std::vector<std::uint32_t> frame_4x(3200u * 2400u, 0xFF000000u);
  san4::UpscaleDirtyRect dirty{
      .has_changes = true, .x = 0, .y = 0, .w = 3200, .h = 2400};
  hd.composite_overlays(frame_4x, 3200, 2400, 4, vid, dirty);
  CHECK(dirty.has_changes);

  // Verify MONDATA eye row (y = 20*4 + 10 = 90) & mouth row (y = 42*4 + 10 = 178)
  CHECK((frame_4x[90u * 3200u + 400u * 4u + 16u] & 0x00FFFFFFu) == 0xFFEECCu);
  CHECK((frame_4x[178u * 3200u + 400u * 4u + 16u] & 0x00FFFFFFu) == 0xCC3344u);
  // Verify Button top half (y=10) == base_00 and bottom half (y=80) == label_00
  CHECK((frame_4x[10u * 3200u + 500u * 4u + 16u] & 0x00FFFFFFu) == 0x335577u);
  CHECK((frame_4x[80u * 3200u + 500u * 4u + 16u] & 0x00FFFFFFu) == 0xDDBB55u);
  // Verify tile_dict dynamic composite cell at (600, 0): center has city gold 0xEEDD33, corner has spring green
  CHECK((frame_4x[32u * 3200u + (600u * 4u + 32u)] & 0x00FFFFFFu) == 0xEEDD33u);
  CHECK((frame_4x[2u * 3200u + (600u * 4u + 2u)] & 0x00FFFFFFu) ==
        (spring_pal[san4::kGrpSlotMap[2]] & 0x00FFFFFFu));
  // Verify masked sprite matched over BOTH (640, 0) and (656, 0) while preserving distinct exterior terrains!
  CHECK((frame_4x[32u * 3200u + (640u * 4u + 32u)] & 0x00FFFFFFu) == 0xEEDD33u);
  CHECK((frame_4x[32u * 3200u + (656u * 4u + 32u)] & 0x00FFFFFFu) == 0xEEDD33u);
  CHECK(frame_4x[2u * 3200u + (640u * 4u + 2u)] !=
        frame_4x[2u * 3200u + (656u * 4u + 2u)]);
  // Verify AH=0x13 crisp 4x glyph stroke at row 4*4+1 over intact HD tile background at row 1*4+1
  CHECK(((frame_4x[(4u * 4u + 1u) * 3200u + (3u * 4u + 1u)] >> 16) & 0xFFu) >= 220u);
  CHECK((frame_4x[5u * 3200u + 5u] & 0x00FFFFFFu) ==
        (spring_pal[san4::kGrpSlotMap[2]] & 0x00FFFFFFu));

  // Test SavedHdPatch round-trip via AH=0x10 -> AH=0x0F AND AH=0x18 swap!
  // Write distinct non-manifest bytes into VRAM under (640, 0, 24, 24), paint HD,
  // then capture a 24x24 popup background snapshot via AH=0x10 (`on_vram_read_rect`).
  vid.vram_write8(640u / 8u, 0xAAu);
  vid.vram_write8(648u / 8u, 0x5Au);
  vid.latch_frame();
  vid.clear_hd_occlude_bits();
  hd.on_planar_blit_source(640, 0, 16, 16, 4, 0u, 0u, 0u, 0u, full_clip,
                           tile0_raw, vid);
  hd.on_planar_blit_source(640, 0, 16, 16, 4, 0u, 0u, 4u, 0u, full_clip, ov_raw,
                           vid);
  std::vector<std::uint8_t> popup_saved_slots(24u * 24u, 0u);
  for (int ry = 0; ry < 24; ++ry) {
    std::memcpy(popup_saved_slots.data() + static_cast<std::size_t>(ry * 24),
                vid.shadow_vram().data() +
                    static_cast<std::size_t>(ry * san4::VesaPlanarVideo::kWidth + 640),
                24u);
  }
  const auto popup_saved_raw =
      encode_interleaved_from_slots(24, 24, 4, popup_saved_slots.data());
  hd.on_vram_read_rect(640, 0, 24, 24, full_clip, vid);

  // Clobber (640..647, 0) with a raw VRAM write using the EXACT SAME byte 0xAA
  // (same 4-bit color slot!) to verify hardware-exact vram_write8 dialog occlusion!
  vid.vram_write8(640u / 8u, 0xAAu);
  vid.latch_frame();
  hd.sync_vram_occlusions(vid);
  std::fill(frame_4x.begin(), frame_4x.end(), 0xFF000000u);
  hd.composite_overlays(frame_4x, 3200, 2400, 4, vid, dirty);
  CHECK(frame_4x[2u * 3200u + (640u * 4u + 2u)] == 0xFF000000u);

  // Restore saved 24x24 popup background via AH=0x0F -> SavedHdPatch restores full 4x HD!
  hd.on_planar_blit_source(640, 0, 24, 24, 4, 0u, 0u, 0u, 0u, full_clip,
                           popup_saved_raw, vid);
  hd.composite_overlays(frame_4x, 3200, 2400, 4, vid, dirty);
  CHECK(frame_4x[2u * 3200u + (640u * 4u + 2u)] != 0xFF000000u);
  CHECK((frame_4x[32u * 3200u + (640u * 4u + 32u)] & 0x00FFFFFFu) == 0xEEDD33u);

  // Also test AH=0x18 SwapRect pre-swap capture & post-swap restore!
  auto pre_swap = hd.capture_patch_snapshot(640, 0, 24, 24, full_clip, vid);
  vid.vram_write8(640u / 8u, 0x55u);
  vid.latch_frame();
  vid.clear_hd_occlude_bits();
  hd.on_vram_swap_complete(640, 0, 24, 24, full_clip, std::move(pre_swap), vid);
  auto pre_swap2 = hd.capture_patch_snapshot(640, 0, 24, 24, full_clip, vid);
  vid.vram_write8(640u / 8u, 0xAAu);
  vid.latch_frame();
  vid.clear_hd_occlude_bits();
  hd.on_vram_swap_complete(640, 0, 24, 24, full_clip, std::move(pre_swap2), vid);
  hd.composite_overlays(frame_4x, 3200, 2400, 4, vid, dirty);
  CHECK((frame_4x[32u * 3200u + (640u * 4u + 32u)] & 0x00FFFFFFu) == 0xEEDD33u);

  // Test 17x17x17 3D LUT seasonal palette shift (spring green -> autumn gold) & smooth 50% fade!
  // Tile at (64, 0) uses spring_pal[kGrpSlotMap[2]] (R=0x33, G=0x99, B=0x22 -> G > R).
  const std::uint32_t spring_px = frame_4x[20u * 3200u + (64u * 4u + 20u)];
  CHECK(((spring_px >> 8) & 0xFFu) > ((spring_px >> 16) & 0xFFu));

  // Switch game color 2 from spring green [B=2, R=3, G=9] to autumn orange-gold [B=2, R=13, G=6]
  auto autumn_brg = full_spring_brg;
  autumn_brg[2 * 3 + 0] = 2u;
  autumn_brg[2 * 3 + 1] = 13u;
  autumn_brg[2 * 3 + 2] = 6u;
  set_video_brg48(autumn_brg);
  hd.composite_overlays(frame_4x, 3200, 2400, 4, vid, dirty);
  const std::uint32_t autumn_px = frame_4x[20u * 3200u + (64u * 4u + 20u)];
  // Now Red channel must strictly exceed Green channel!
  CHECK(((autumn_px >> 16) & 0xFFu) > ((autumn_px >> 8) & 0xFFu) + 40u);

  // Smooth 24-bit crossfade 50% to black -> graded HD pixels smoothly halve in brightness!
  std::array<std::uint8_t, 48> black_brg{};
  vid.begin_smooth_fade(0u, 16u, autumn_brg.data(), black_brg.data(),
                        san4::kGrpSlotMap.data(), 0.20);
  CHECK(vid.advance_fade(0.10));  // t = 0.5
  vid.latch_frame();
  hd.composite_overlays(frame_4x, 3200, 2400, 4, vid, dirty);
  const std::uint32_t half_fade_px = frame_4x[20u * 3200u + (64u * 4u + 20u)];
  const int r_full = static_cast<int>((autumn_px >> 16) & 0xFFu);
  const int r_half = static_cast<int>((half_fade_px >> 16) & 0xFFu);
  CHECK(r_half > r_full / 3 && r_half < (r_full * 2) / 3);
  vid.finish_fade_immediately();
  vid.latch_frame();

  // Test dynamic-palette NPK (`bank_hint == 3`) exact scene-palette re-upscaling
  // during fade-in from black (eliminating dummy PC-98 header palette artifacts)!
  {
    std::filesystem::create_directories(pack_dir / "cg");
    std::array<std::uint8_t, 256> dyn_slots{};
    for (int y = 0; y < 16; ++y) {
      for (int x = 0; x < 16; ++x) {
        const std::uint8_t gc =
            (((x + y) & 1) == 0) ? static_cast<std::uint8_t>(4u)
                                 : static_cast<std::uint8_t>(5u);
        dyn_slots[static_cast<std::size_t>(y * 16 + x)] =
            san4::kGrpSlotMap[gc];
      }
    }
    const std::uint64_t dyn_hash =
        san4::HdPackManager::hash_sprite_4bit(16, 16, dyn_slots.data());
    const std::vector<std::uint32_t> dummy_neon(64u * 64u, 0xFF00FF00u);
    CHECK(san4::HdPackManager::save_argb_png(
        pack_dir / "cg" / "dyn_sunset.png", 64, 64, dummy_neon.data()));
    {
      std::ofstream mf(pack_dir / "hd_manifest.tsv",
                       std::ios::binary | std::ios::app);
      char hbuf[32];
      std::snprintf(hbuf, sizeof(hbuf), "%016llx",
                    static_cast<unsigned long long>(dyn_hash));
      mf << hbuf << "\t16\t16\t3\tcg/dyn_sunset.png\t" << spring_csv << "\n";
    }
    CHECK(hd.reload_pack() == 3003u);
    hd.clear_overlays();

    // Blit dynamic CG at (320, 160) while screen palette is all-black!
    set_video_brg48(black_brg);
    const auto dyn_raw =
        encode_interleaved_from_slots(16, 16, 4, dyn_slots.data());
    hd.on_planar_blit_source(320, 160, 16, 16, 4, 0u, 0u, 0u, 0u, full_clip,
                             dyn_raw, vid);

    // Begin fade-in from black to warm sunset palette where colors 4 & 5 are
    // adjacent sunset shades: [B=2, R=13, G=5] and [B=2, R=15, G=6].
    auto sunset_brg = black_brg;
    sunset_brg[4 * 3 + 0] = 2u;
    sunset_brg[4 * 3 + 1] = 13u;
    sunset_brg[4 * 3 + 2] = 5u;
    sunset_brg[5 * 3 + 0] = 2u;
    sunset_brg[5 * 3 + 1] = 15u;
    sunset_brg[5 * 3 + 2] = 6u;
    vid.begin_smooth_fade(0u, 16u, black_brg.data(), sunset_brg.data(),
                          san4::kGrpSlotMap.data(), 0.20);
    CHECK((vid.reference_nonblack_palette_argb()[san4::kGrpSlotMap[5]] &
           0x00FFFFFFu) == 0xFF6622u);
    CHECK(vid.advance_fade(0.10));  // midpoint fade-in
    vid.latch_frame();
    hd.composite_overlays(frame_4x, 3200, 2400, 4, vid, dirty);

    vid.finish_fade_immediately();
    vid.latch_frame();
    hd.composite_overlays(frame_4x, 3200, 2400, 4, vid, dirty);

    const std::uint32_t s0 =
        frame_4x[(160u * 4u + 32u) * 3200u + (320u * 4u + 32u)];
    const std::uint32_t s1 =
        frame_4x[(160u * 4u + 32u) * 3200u + (320u * 4u + 36u)];
    // De-dithered against exact sunset palette (NOT dummy neon green 0x00FF00)!
    CHECK(((s0 >> 16) & 0xFFu) >= 0xDCu);
    CHECK(((s0 >> 8) & 0xFFu) >= 0x50u && ((s0 >> 8) & 0xFFu) <= 0x68u);
    CHECK(std::abs(static_cast<int>((s0 >> 16) & 0xFFu) -
                   static_cast<int>((s1 >> 16) & 0xFFu)) <= 2);
  }
}

// ---------------------------------------------------------------------------
// Stage 0/1 performance infrastructure: ThreadPool, SIMD blend kernels,
// upscaler tile-row spans, sRGB LUT upscaler parity, GRP600C census.
// ---------------------------------------------------------------------------

constexpr bool kSanitizedBuild =
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    true;
#else
    false;
#endif
#else
    false;
#endif

// Deterministic xorshift64* generator for synthetic test data.
class TestRng {
 public:
  explicit TestRng(std::uint64_t seed) : s_(seed ? seed : 1u) {}
  std::uint32_t next() {
    s_ ^= s_ >> 12;
    s_ ^= s_ << 25;
    s_ ^= s_ >> 27;
    return static_cast<std::uint32_t>((s_ * 0x2545F4914F6CDD1Dull) >> 32);
  }
  std::uint32_t below(std::uint32_t n) { return next() % n; }

 private:
  std::uint64_t s_;
};

bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!pred()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

void test_thread_pool_inline_nesting_async_and_stress() {
  CHECK(san4::ThreadPool::default_thread_count() >= 4u);
  CHECK(san4::ThreadPool::default_thread_count() <= 16u);
  {
    san4::ThreadPool auto_pool(0);
    CHECK(auto_pool.size() == san4::ThreadPool::default_thread_count());
  }

  san4::ThreadPool pool(4);
  CHECK(pool.size() == 4u);
  CHECK(!san4::ThreadPool::in_pool_work());

  // Every index is visited exactly once, in chunks of at most `grain`.
  {
    constexpr int kN = 10007;
    std::vector<std::atomic<int>> hits(kN);
    std::atomic<int> max_chunk{0};
    pool.parallel_for(0, kN, 7, [&](int b, int e) {
      int prev = max_chunk.load();
      while (e - b > prev && !max_chunk.compare_exchange_weak(prev, e - b)) {
      }
      for (int i = b; i < e; ++i) {
        hits[static_cast<std::size_t>(i)].fetch_add(1);
      }
    });
    bool all_once = true;
    for (const auto& h : hits) {
      all_once = all_once && (h.load() == 1);
    }
    CHECK(all_once);
    CHECK(max_chunk.load() <= 7);
  }

  // Empty ranges never call; single-chunk ranges run inline on the caller.
  {
    int calls = 0;
    pool.parallel_for(5, 5, 1, [&](int, int) { ++calls; });
    pool.parallel_for(9, 3, 1, [&](int, int) { ++calls; });
    CHECK(calls == 0);
    std::thread::id ran_on;
    pool.parallel_for(0, 16, 16, [&](int b, int e) {
      ran_on = std::this_thread::get_id();
      calls += e - b;
    });
    CHECK(calls == 16);
    CHECK(ran_on == std::this_thread::get_id());
  }

  // Nested parallel_for from inside a batch (worker or participating caller)
  // runs inline on the same thread instead of deadlocking.
  {
    std::atomic<int> nested_total{0};
    std::atomic<int> nested_off_thread{0};
    std::atomic<int> not_in_pool_work{0};
    pool.parallel_for(0, 64, 1, [&](int, int) {
      if (!san4::ThreadPool::in_pool_work()) {
        not_in_pool_work.fetch_add(1);
      }
      const auto outer = std::this_thread::get_id();
      pool.parallel_for(0, 100, 3, [&](int b, int e) {
        if (std::this_thread::get_id() != outer) {
          nested_off_thread.fetch_add(1);
        }
        nested_total.fetch_add(e - b);
      });
    });
    CHECK(nested_total.load() == 64 * 100);
    CHECK(nested_off_thread.load() == 0);
    CHECK(not_in_pool_work.load() == 0);
    CHECK(!san4::ThreadPool::in_pool_work());
  }

  // An async job that itself calls parallel_for completes (runs inline).
  {
    std::atomic<int> async_sum{0};
    std::atomic<bool> async_done{false};
    pool.enqueue([&]() {
      pool.parallel_for(0, 1000, 10, [&](int b, int e) {
        async_sum.fetch_add(e - b);
      });
      async_done.store(true);
    });
    CHECK(wait_until([&]() { return async_done.load(); }, 10000));
    CHECK(async_sum.load() == 1000);
  }

  // Exceptions propagate to the caller after the batch drains; the pool
  // stays usable afterwards.
  {
    std::atomic<int> ran{0};
    bool caught = false;
    try {
      pool.parallel_for(0, 256, 4, [&](int b, int e) {
        ran.fetch_add(e - b);
        if (b == 128) {
          throw std::runtime_error("chunk failure");
        }
      });
    } catch (const std::runtime_error&) {
      caught = true;
    }
    CHECK(caught);
    CHECK(ran.load() == 256);
    std::atomic<int> after{0};
    pool.parallel_for(0, 100, 5, [&](int b, int e) { after.fetch_add(e - b); });
    CHECK(after.load() == 100);
  }

  // A single-worker pool always runs parallel_for inline.
  {
    san4::ThreadPool solo(1);
    CHECK(solo.size() == 1u);
    std::thread::id ran_on;
    int total = 0;
    solo.parallel_for(0, 1000, 1, [&](int b, int e) {
      ran_on = std::this_thread::get_id();
      total += e - b;
    });
    CHECK(total == 1000);
    CHECK(ran_on == std::this_thread::get_id());
    std::atomic<bool> job_ran{false};
    solo.enqueue([&]() { job_ran.store(true); });
    CHECK(wait_until([&]() { return job_ran.load(); }, 10000));
  }

  // 10k async jobs interleaved with parallel batches from two concurrent
  // callers all complete (TSan stress).
  {
    constexpr int kJobs = 10000;
    std::atomic<int> jobs_done{0};
    std::atomic<long long> batch_sum{0};
    auto caller = [&](int seed) {
      for (int round = 0; round < 50; ++round) {
        for (int j = 0; j < kJobs / 100; ++j) {
          pool.enqueue([&]() { jobs_done.fetch_add(1); });
        }
        pool.parallel_for(0, 512 + seed, 8, [&](int b, int e) {
          long long local = 0;
          for (int i = b; i < e; ++i) {
            local += i;
          }
          batch_sum.fetch_add(local);
        });
      }
    };
    std::thread t1(caller, 0);
    std::thread t2(caller, 1);
    t1.join();
    t2.join();
    CHECK(wait_until([&]() { return jobs_done.load() == kJobs; }, 30000));
    const long long per0 = 511LL * 512LL / 2LL;
    const long long per1 = 512LL * 513LL / 2LL;
    CHECK(batch_sum.load() == 50LL * (per0 + per1));
  }

  // Destruction with queued jobs drops the remainder without hanging: only
  // the job already running completes. Dropped jobs are destroyed outside
  // the pool lock (a capture whose destructor enqueues must not deadlock),
  // and work enqueued once destruction has begun never runs.
  {
    struct DropProbe {
      DropProbe(san4::ThreadPool* p, std::atomic<int>* r, std::atomic<int>* d)
          : pool(p), runs(r), destroyed(d) {}
      DropProbe(const DropProbe&) = delete;
      DropProbe& operator=(const DropProbe&) = delete;
      ~DropProbe() {
        pool->enqueue([r = runs]() { r->fetch_add(1); });
        destroyed->fetch_add(1);
      }
      san4::ThreadPool* pool;
      std::atomic<int>* runs;
      std::atomic<int>* destroyed;
    };
    constexpr int kQueued = 99;
    std::atomic<int> started{0};
    std::atomic<int> destroyed{0};
    std::atomic<bool> blocked{false};
    std::atomic<bool> release{false};
    auto short_lived = std::make_unique<san4::ThreadPool>(1);
    short_lived->enqueue([&]() {
      started.fetch_add(1);
      blocked.store(true);
      while (!release.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
    const bool was_blocked = wait_until([&]() { return blocked.load(); }, 10000);
    for (int i = 0; i < kQueued; ++i) {
      auto probe =
          std::make_shared<DropProbe>(short_lived.get(), &started, &destroyed);
      short_lived->enqueue([probe]() { probe->runs->fetch_add(1); });
    }
    std::thread destroyer([&]() { short_lived.reset(); });
    // The destructor drops the queue before joining the blocked worker.
    const bool all_dropped =
        wait_until([&]() { return destroyed.load() == kQueued; }, 10000);
    release.store(true);
    destroyer.join();
    CHECK(was_blocked);
    CHECK(all_dropped);
    CHECK(started.load() == 1);
    CHECK(destroyed.load() == kQueued);
  }

  // A throwing async job is reported on stderr and the worker keeps going.
  {
    san4::ThreadPool solo(1);
    std::ostringstream captured;
    std::streambuf* const prev = std::cerr.rdbuf(captured.rdbuf());
    std::atomic<bool> after{false};
    solo.enqueue([]() { throw std::runtime_error("boom"); });
    solo.enqueue([]() { throw 42; });
    solo.enqueue([&]() { after.store(true); });
    const bool ran = wait_until([&]() { return after.load(); }, 10000);
    std::cerr.rdbuf(prev);
    CHECK(ran);
    CHECK(captured.str().find("[san4-pool] async job threw: boom") !=
          std::string::npos);
    CHECK(captured.str().find("non-standard exception") != std::string::npos);
  }
}

// Reference copies of the compositor's per-pixel math (hd_pack.cpp
// composite_overlays) used to validate the SIMD kernels.
std::uint32_t ref_box_sample(const std::uint32_t* hd_row0, std::ptrdiff_t stride,
                             int ds, int i) {
  std::uint32_t sum_a = 0, sum_r = 0, sum_g = 0, sum_b = 0, cnt_a = 0;
  for (int by = 0; by < ds; ++by) {
    const std::uint32_t* hrow = hd_row0 + by * stride + i * ds;
    for (int bx = 0; bx < ds; ++bx) {
      const std::uint32_t p = hrow[bx];
      const std::uint32_t pa = (p >> 24) & 0xFFu;
      sum_a += pa;
      if (pa > 0u) {
        sum_r += ((p >> 16) & 0xFFu) * pa;
        sum_g += ((p >> 8) & 0xFFu) * pa;
        sum_b += (p & 0xFFu) * pa;
        cnt_a += pa;
      }
    }
  }
  const std::uint32_t nsub = static_cast<std::uint32_t>(ds * ds);
  const std::uint32_t avg_a = (sum_a + nsub / 2u) / nsub;
  if (avg_a == 0u || cnt_a == 0u) {
    return 0u;
  }
  return (avg_a << 24) | (((sum_r + cnt_a / 2u) / cnt_a) << 16) |
         (((sum_g + cnt_a / 2u) / cnt_a) << 8) | ((sum_b + cnt_a / 2u) / cnt_a);
}

void ref_blend_pixel(std::uint32_t& dst, std::uint32_t spx,
                     const san4::simd::GradeFn* grade) {
  const std::uint32_t sa = (spx >> 24) & 0xFFu;
  if (sa == 0u) {
    return;
  }
  std::uint32_t g = spx & 0x00FFFFFFu;
  if (grade != nullptr) {
    g = (*grade)(g);
  }
  if (sa == 255u) {
    dst = 0xFF000000u | g;
    return;
  }
  const std::uint32_t d = dst;
  const std::uint32_t inv = 255u - sa;
  const std::uint32_t r =
      (((g >> 16) & 0xFFu) * sa + ((d >> 16) & 0xFFu) * inv + 127u) / 255u;
  const std::uint32_t gg =
      (((g >> 8) & 0xFFu) * sa + ((d >> 8) & 0xFFu) * inv + 127u) / 255u;
  const std::uint32_t b = ((g & 0xFFu) * sa + (d & 0xFFu) * inv + 127u) / 255u;
  dst = 0xFF000000u | (r << 16) | (gg << 8) | b;
}

std::uint32_t test_grade(const void* ctx, std::uint32_t rgb) {
  const std::uint32_t salt = *static_cast<const std::uint32_t*>(ctx);
  // Pure, non-linear channel remix.
  const std::uint32_t r = (rgb >> 16) & 0xFFu;
  const std::uint32_t g = (rgb >> 8) & 0xFFu;
  const std::uint32_t b = rgb & 0xFFu;
  return (((g * 7u + salt) & 0xFFu) << 16) | (((b ^ r) & 0xFFu) << 8) |
         ((255u - r + (salt >> 8)) & 0xFFu);
}

std::uint32_t random_hd_pixel(TestRng& rng) {
  const std::uint32_t kind = rng.below(8);
  std::uint32_t a = 0;
  if (kind < 3) {
    a = 0u;
  } else if (kind < 5) {
    a = 255u;
  } else {
    a = rng.below(256);
  }
  return (a << 24) | (rng.next() & 0x00FFFFFFu);
}

void test_simd_div255_exhaustive_and_blend_kernels() {
  using san4::simd::SimdLevel;
  std::cout << "[san4-simd] detected level: "
            << san4::simd::level_name(san4::simd::detected_level()) << "\n";

  // Exact floor(v / 255) for every 16-bit value, scalar and vector lanes.
  std::vector<std::uint16_t> in(65536);
  for (std::uint32_t v = 0; v < 65536u; ++v) {
    in[v] = static_cast<std::uint16_t>(v);
  }
  bool scalar_exact = true;
  for (std::uint32_t v = 0; v < 65536u; ++v) {
    scalar_exact = scalar_exact && (san4::simd::div255(v) == v / 255u);
  }
  CHECK(scalar_exact);
  for (SimdLevel level : {SimdLevel::Scalar, SimdLevel::Avx2}) {
    std::vector<std::uint16_t> out(65536, 0xFFFFu);
    san4::simd::div255_batch(level, in.data(), out.data(), in.size());
    bool exact = true;
    for (std::uint32_t v = 0; v < 65536u; ++v) {
      exact = exact && (out[v] == v / 255u);
    }
    CHECK(exact);
  }

  TestRng rng(0x51D0B1E5u);
  const std::uint32_t salt = 0x1234u;
  const san4::simd::GradeFn grade{&test_grade, &salt};

  // 1:1 blend over many lengths (tails), alpha mixes, and uniform runs that
  // exercise the all-transparent / all-opaque fast paths.
  for (int trial = 0; trial < 400; ++trial) {
    const int n = 1 + static_cast<int>(rng.below(77));
    std::vector<std::uint32_t> hd(static_cast<std::size_t>(n));
    std::vector<std::uint32_t> dst0(static_cast<std::size_t>(n));
    const std::uint32_t mode = rng.below(4);
    for (int i = 0; i < n; ++i) {
      std::uint32_t p = random_hd_pixel(rng);
      if (mode == 1) p &= 0x00FFFFFFu;            // all transparent
      if (mode == 2) p |= 0xFF000000u;            // all opaque
      hd[static_cast<std::size_t>(i)] = p;
      dst0[static_cast<std::size_t>(i)] = 0xFF000000u | (rng.next() & 0xFFFFFFu);
    }
    for (const san4::simd::GradeFn* g : {static_cast<const san4::simd::GradeFn*>(nullptr), &grade}) {
      std::vector<std::uint32_t> want = dst0;
      for (int i = 0; i < n; ++i) {
        ref_blend_pixel(want[static_cast<std::size_t>(i)],
                        hd[static_cast<std::size_t>(i)], g);
      }
      for (SimdLevel level : {SimdLevel::Scalar, SimdLevel::Avx2}) {
        std::vector<std::uint32_t> got = dst0;
        san4::simd::blend_row_4x(level, got.data(), hd.data(), n, g);
        CHECK(got == want);
      }
      std::vector<std::uint32_t> got = dst0;
      san4::simd::blend_row_4x(got.data(), hd.data(), n, g);
      CHECK(got == want);
    }
  }

  // Alpha-weighted ds x ds box downsample + blend (ds = 1, 2, 4).
  for (int ds : {1, 2, 4}) {
    for (int trial = 0; trial < 200; ++trial) {
      const int n = 1 + static_cast<int>(rng.below(45));
      const std::ptrdiff_t stride = static_cast<std::ptrdiff_t>(n * ds + 5);
      std::vector<std::uint32_t> hd(static_cast<std::size_t>(stride * ds));
      const std::uint32_t mode = rng.below(4);
      for (auto& p : hd) {
        p = random_hd_pixel(rng);
        if (mode == 1) p &= 0x00FFFFFFu;
        if (mode == 2) p |= 0xFF000000u;
      }
      std::vector<std::uint32_t> dst0(static_cast<std::size_t>(n));
      for (auto& d : dst0) {
        d = 0xFF000000u | (rng.next() & 0xFFFFFFu);
      }
      for (const san4::simd::GradeFn* g : {static_cast<const san4::simd::GradeFn*>(nullptr), &grade}) {
        std::vector<std::uint32_t> want = dst0;
        for (int i = 0; i < n; ++i) {
          const std::uint32_t spx =
              (ds == 1) ? hd[static_cast<std::size_t>(i)]
                        : ref_box_sample(hd.data(), stride, ds, i);
          ref_blend_pixel(want[static_cast<std::size_t>(i)], spx, g);
        }
        for (SimdLevel level : {SimdLevel::Scalar, SimdLevel::Avx2}) {
          std::vector<std::uint32_t> got = dst0;
          san4::simd::blend_row_box(level, got.data(), hd.data(), stride, ds, n,
                                    g);
          CHECK(got == want);
        }
      }
    }
  }

  // Worst-case box sums: every subsample opaque white or near-threshold
  // alpha, which stresses the float-division rounding argument.
  for (int ds : {2, 4}) {
    for (std::uint32_t a = 0; a < 256u; ++a) {
      for (std::uint32_t c : {0u, 1u, 127u, 128u, 254u, 255u}) {
        const int n = 8;
        const std::ptrdiff_t stride = n * ds;
        std::vector<std::uint32_t> hd(static_cast<std::size_t>(stride * ds));
        for (std::size_t k = 0; k < hd.size(); ++k) {
          const std::uint32_t ak = (k % 3u == 0u) ? a : (255u - a);
          const std::uint32_t ck = (k % 2u == 0u) ? c : (255u - c);
          hd[k] = (ak << 24) | (ck << 16) | (c << 8) | (255u - ck);
        }
        std::vector<std::uint32_t> want(static_cast<std::size_t>(n), 0xFF102030u);
        for (int i = 0; i < n; ++i) {
          ref_blend_pixel(want[static_cast<std::size_t>(i)],
                          ref_box_sample(hd.data(), stride, ds, i), nullptr);
        }
        std::vector<std::uint32_t> got(static_cast<std::size_t>(n), 0xFF102030u);
        san4::simd::blend_row_box(SimdLevel::Avx2, got.data(), hd.data(), stride,
                                  ds, n, nullptr);
        CHECK(got == want);
      }
    }
  }
}

// Fills every plane of a 0x102 (800x600) video with deterministic noise.
void fill_video_noise(san4::VesaPlanarVideo& vid, std::uint64_t seed) {
  TestRng rng(seed);
  for (std::uint8_t plane = 0; plane < 4u; ++plane) {
    vid.port_out8(0x3C4u, 0x02u);
    vid.port_out8(0x3C5u, static_cast<std::uint8_t>(1u << plane));
    for (std::uint32_t off = 0; off < san4::VesaPlanarVideo::kVisibleBytes;
         ++off) {
      // Mix flat runs (exercise solid/dither fast paths) with noise.
      const std::uint32_t r = rng.next();
      vid.vram_write8(off, ((off / 37u) % 3u == 0u)
                               ? static_cast<std::uint8_t>(0x00u)
                               : static_cast<std::uint8_t>(r & 0xFFu));
    }
  }
  vid.port_out8(0x3C4u, 0x02u);
  vid.port_out8(0x3C5u, 0x0Fu);
}

// Marks tile (tx, ty) dirty by rewriting its first byte with `val`.
void touch_tile(san4::VesaPlanarVideo& vid, int tx, int ty, std::uint8_t val) {
  const std::uint32_t off = static_cast<std::uint32_t>(
      (ty * san4::VesaPlanarVideo::kTileSize + 3) *
          san4::VesaPlanarVideo::kBytesPerLine +
      tx * 2);
  vid.vram_write8(off, val);
}

void test_upscaler_tile_row_spans_and_shared_pool() {
  using san4::UpscaleDirtyPolicy;
  using san4::UpscaleMode;
  constexpr std::uint32_t kSentinel = 0x12345678u;
  san4::ThreadPool pool(4);

  for (UpscaleMode mode : {UpscaleMode::Xbr4x, UpscaleMode::Xbr2x,
                           UpscaleMode::Scale2x, UpscaleMode::Sharp2x,
                           UpscaleMode::Nearest}) {
    // Four identical video timelines: spans (private workers), spans (shared
    // pool), bounding box, and a full-frame reference.
    auto v_span = std::make_unique<san4::VesaPlanarVideo>();
    v_span->reset_mode(0x0102u);
    fill_video_noise(*v_span, 0xC0FFEEu);
    v_span->latch_frame();
    auto v_pool = std::make_unique<san4::VesaPlanarVideo>(*v_span);
    auto v_box = std::make_unique<san4::VesaPlanarVideo>(*v_span);
    auto v_full = std::make_unique<san4::VesaPlanarVideo>(*v_span);

    san4::PixelUpscaler up_span(4);
    san4::PixelUpscaler up_pool(1);
    san4::PixelUpscaler up_box(4);
    san4::PixelUpscaler up_full(1);
    up_span.set_dirty_policy(UpscaleDirtyPolicy::TileRowSpans);
    up_pool.set_dirty_policy(UpscaleDirtyPolicy::TileRowSpans);
    up_pool.set_thread_pool(&pool);
    CHECK(up_box.dirty_policy() == UpscaleDirtyPolicy::BoundingBox);
    for (auto* up : {&up_span, &up_pool, &up_box, &up_full}) {
      up->set_mode(mode);
    }
    (void)up_span.upscale_from_video(*v_span, true);
    (void)up_pool.upscale_from_video(*v_pool, true);
    (void)up_box.upscale_from_video(*v_box, true);
    CHECK(up_span.dirty_rects().size() == 1u);
    CHECK(up_span.last_dirty_tile_count() == san4::VesaPlanarVideo::kTileCount);

    // Scattered edits: an isolated tile, a 3-tile run, a corner tile, a tile
    // row touched twice with a gap, and the bottom-right edge tile.
    auto edit = [](san4::VesaPlanarVideo& v) {
      touch_tile(v, 3, 2, 0x5Au);
      touch_tile(v, 10, 2, 0xA5u);
      touch_tile(v, 11, 2, 0x3Cu);
      touch_tile(v, 12, 2, 0xC3u);
      touch_tile(v, 0, 0, 0x81u);
      touch_tile(v, 20, 3, 0x7Eu);
      touch_tile(v, 23, 3, 0x18u);
      touch_tile(v, 49, 37, 0xE7u);
      v.latch_frame();
    };
    edit(*v_span);
    edit(*v_pool);
    edit(*v_box);
    edit(*v_full);

    std::fill(up_span.mutable_output_argb().begin(),
              up_span.mutable_output_argb().end(), kSentinel);
    std::fill(up_pool.mutable_output_argb().begin(),
              up_pool.mutable_output_argb().end(), kSentinel);
    const san4::UpscaleDirtyRect r_span = up_span.upscale_from_video(*v_span);
    const san4::UpscaleDirtyRect r_pool = up_pool.upscale_from_video(*v_pool);
    const san4::UpscaleDirtyRect r_box = up_box.upscale_from_video(*v_box);
    (void)up_full.upscale_from_video(*v_full, true);

    // Union return value is unchanged by the policy.
    CHECK(r_span.has_changes && r_box.has_changes);
    CHECK(r_span.x == r_box.x && r_span.y == r_box.y && r_span.w == r_box.w &&
          r_span.h == r_box.h);
    CHECK(r_pool.x == r_box.x && r_pool.w == r_box.w && r_pool.h == r_box.h);
    CHECK(up_box.dirty_rects().size() == 1u);
    CHECK(up_box.dirty_rects()[0].x == r_box.x &&
          up_box.dirty_rects()[0].w == r_box.w);
    CHECK(up_span.last_dirty_tile_count() == 8);

    // Spans: row 0 {0}, row 2 {3}, {10..12}, row 3 {20}, {23}, row 37 {49}.
    const auto rects = up_span.dirty_rects();
    CHECK(rects.size() == 6u);
    const int s = san4::PixelUpscaler::scale_factor_for(mode);
    CHECK(rects[0].x == 0 && rects[0].y == 0 && rects[0].w == 18 * s &&
          rects[0].h == 18 * s);
    CHECK(rects[2].x == (10 * 16 - 2) * s && rects[2].w == (3 * 16 + 4) * s &&
          rects[2].y == (2 * 16 - 2) * s && rects[2].h == 20 * s);
    CHECK(rects[5].x == (49 * 16 - 2) * s && rects[5].w == 18 * s &&
          rects[5].y == (37 * 16 - 2) * s && rects[5].h == 10 * s);
    std::size_t covered_px = 0;
    for (const auto& r : rects) {
      CHECK(r.has_changes && r.w > 0 && r.h > 0);
      covered_px += static_cast<std::size_t>(r.w) * static_cast<std::size_t>(r.h);
    }
    CHECK(covered_px < static_cast<std::size_t>(r_box.w) *
                           static_cast<std::size_t>(r_box.h));

    // Inside the spans: identical to a full-frame render. Outside: untouched.
    const int ow = up_full.output_width();
    const int oh = up_full.output_height();
    std::vector<std::uint8_t> inside(static_cast<std::size_t>(ow * oh), 0u);
    for (const auto& r : rects) {
      for (int y = r.y; y < r.y + r.h; ++y) {
        for (int x = r.x; x < r.x + r.w; ++x) {
          inside[static_cast<std::size_t>(y * ow + x)] = 1u;
        }
      }
    }
    const auto& full = up_full.output_argb();
    const auto& span_px = up_span.output_argb();
    bool spans_match = true;
    bool outside_untouched = true;
    for (std::size_t i = 0; i < full.size(); ++i) {
      if (inside[i] != 0u) {
        spans_match = spans_match && (span_px[i] == full[i]);
      } else {
        outside_untouched = outside_untouched && (span_px[i] == kSentinel);
      }
    }
    CHECK(spans_match);
    CHECK(outside_untouched);
    CHECK(up_pool.output_argb() == up_span.output_argb());
    // Bounding-box policy: the whole union is fresh.
    CHECK(up_box.output_argb() == full);

    // No changes -> no rects.
    (void)up_span.upscale_from_video(*v_span);
    CHECK(up_span.dirty_rects().empty());
  }

  // 640x480 mode: spans stay inside the active area and detaching the pool
  // restores the private workers.
  {
    auto vid = std::make_unique<san4::VesaPlanarVideo>();
    vid->reset_mode(0x0012u);
    vid->latch_frame();
    san4::PixelUpscaler up(4);
    up.set_mode(UpscaleMode::Xbr2x);
    up.set_dirty_policy(UpscaleDirtyPolicy::TileRowSpans);
    up.set_thread_pool(&pool);
    (void)up.upscale_from_video(*vid, true);
    CHECK(up.output_width() == 1280 && up.output_height() == 960);
    up.set_thread_pool(nullptr);
    vid->vram_write8(static_cast<std::uint32_t>(29 * 16 * 80 + 79), 0xFFu);
    vid->latch_frame();
    const auto r = up.upscale_from_video(*vid);
    CHECK(r.has_changes);
    CHECK(up.dirty_rects().size() == 1u);
    CHECK(up.dirty_rects()[0].x + up.dirty_rects()[0].w <= 1280);
    CHECK(up.dirty_rects()[0].y + up.dirty_rects()[0].h <= 960);
  }
}

struct SyntheticArt {
  int w;
  int h;
  san4::hd_gen::ArtAssetKind kind;
  bool masked;
};

// Builds a structured 4-bit test image: gradients, 2x2 checkerboard and Bayer
// dithers, 1px lines, isolated dots, and noise.
std::vector<std::uint8_t> make_synthetic_art(int w, int h, std::uint64_t seed) {
  TestRng rng(seed);
  std::vector<std::uint8_t> px(static_cast<std::size_t>(w * h));
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      std::uint8_t v = 0;
      switch (((x / 16) + (y / 16) * 3) % 5) {
        case 0:
          v = static_cast<std::uint8_t>((x + y) / 4 % 16);
          break;
        case 1:
          v = ((x + y) & 1) ? 3u : 9u;
          break;
        case 2:
          v = ((x & 1) == 0 && (y & 1) == 0) ? 12u : 5u;
          break;
        case 3:
          v = (x % 7 == 0 || y % 5 == 0) ? 15u : 1u;
          break;
        default:
          v = static_cast<std::uint8_t>(rng.below(16));
          break;
      }
      px[static_cast<std::size_t>(y * w + x)] = v;
    }
  }
  return px;
}

std::array<std::uint32_t, 16> synthetic_palette(std::uint64_t seed) {
  TestRng rng(seed);
  std::array<std::uint32_t, 16> pal{};
  for (std::size_t i = 0; i < pal.size(); ++i) {
    pal[i] = 0xFF000000u | (rng.next() & 0x00FFFFFFu);
  }
  pal[0] = 0xFF000000u;
  pal[15] = 0xFFFFFFFFu;
  return pal;
}

int max_channel_diff(const std::vector<std::uint32_t>& a,
                     const std::vector<std::uint32_t>& b) {
  if (a.size() != b.size()) {
    return 256;
  }
  int max_d = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    for (int sh = 0; sh < 32; sh += 8) {
      const int ca = static_cast<int>((a[i] >> sh) & 0xFFu);
      const int cb = static_cast<int>((b[i] >> sh) & 0xFFu);
      max_d = std::max(max_d, std::abs(ca - cb));
    }
  }
  return max_d;
}

void test_hd_art_upscaler_srgb_lut_and_pool_parity() {
  namespace hg = san4::hd_gen;

  // sRGB -> linear table is exact for all 256 codes.
  for (int c = 0; c < 256; ++c) {
    const float want =
        san4::test_ref::srgb8_to_linear_reference(static_cast<std::uint8_t>(c));
    CHECK(hg::srgb8_to_linear(static_cast<std::uint8_t>(c)) == want);
  }

  // linear -> sRGB: strided sweep of every float bit pattern in [0, 1], dense
  // windows around each code's decision threshold, and special values.
  int lut_mismatches = 0;
  auto check_lin = [&](float f) {
    if (hg::linear_to_srgb8(f) != san4::test_ref::linear_to_srgb8_reference(f)) {
      ++lut_mismatches;
    }
  };
  const std::uint32_t kOneBits = 0x3F800000u;
  const std::uint32_t stride = kSanitizedBuild ? 9973u : 997u;
  for (std::uint32_t bits = 0; bits <= kOneBits; bits += stride) {
    check_lin(std::bit_cast<float>(bits));
  }
  for (int v = 0; v < 256; ++v) {
    const double s = (static_cast<double>(v) - 0.5) / 255.0;
    const double lin = (s <= 0.04045) ? s / 12.92
                                      : std::pow((s + 0.055) / 1.055, 2.4);
    const float center = static_cast<float>(std::max(lin, 0.0));
    const std::uint32_t cb = std::bit_cast<std::uint32_t>(center);
    const std::uint32_t span = kSanitizedBuild ? 64u : 1024u;
    for (std::uint32_t d = 0; d <= 2u * span; ++d) {
      const std::uint32_t b = cb + d >= span ? cb + d - span : 0u;
      if (b <= kOneBits) {
        check_lin(std::bit_cast<float>(b));
      }
    }
  }
  for (float f : {-1.0f, -0.0f, 0.0f, 1e-30f, 0.0031308f, 0.5f, 1.0f, 1.5f,
                  std::numeric_limits<float>::infinity(),
                  -std::numeric_limits<float>::infinity()}) {
    check_lin(f);
  }
  CHECK(lut_mismatches == 0);
  CHECK(hg::linear_to_srgb8(std::numeric_limits<float>::quiet_NaN()) == 0u);

  // Full pipeline parity vs the frozen pre-LUT implementation, with and
  // without a pool, across every asset kind and masked/unmasked inputs.
  san4::ThreadPool pool(4);
  const std::array<SyntheticArt, 7> cases{{
      {16, 16, hg::ArtAssetKind::TerrainTile, false},
      {32, 32, hg::ArtAssetKind::UiChrome, false},
      {32, 24, hg::ArtAssetKind::UiChrome, true},
      {64, 80, hg::ArtAssetKind::Portrait, false},
      {48, 40, hg::ArtAssetKind::MaskedSprite, true},
      {96, 64, hg::ArtAssetKind::CgIllustration, false},
      {80, 72, hg::ArtAssetKind::Portrait, true},
  }};
  int overall_max_diff = 0;
  for (std::size_t ci = 0; ci < cases.size(); ++ci) {
    const SyntheticArt& c = cases[ci];
    const auto px = make_synthetic_art(c.w, c.h, 0xA11CE + ci);
    const auto pal = synthetic_palette(0xBEEF + ci);
    std::vector<std::uint8_t> mask;
    if (c.masked) {
      mask.resize(px.size());
      for (int y = 0; y < c.h; ++y) {
        for (int x = 0; x < c.w; ++x) {
          const int dx = 2 * x - c.w;
          const int dy = 2 * y - c.h;
          mask[static_cast<std::size_t>(y * c.w + x)] =
              (dx * dx + dy * dy < c.w * c.h) ? 1u : 0u;
        }
      }
    }
    const std::uint8_t* mask_ptr = c.masked ? mask.data() : nullptr;
    std::vector<std::uint32_t> ref;
    std::vector<std::uint32_t> got;
    std::vector<std::uint32_t> got_pool;
    san4::test_ref::upscale_art_4x_argb_reference(px.data(), mask_ptr, c.w, c.h,
                                                  pal, c.kind, ref);
    hg::upscale_art_4x_argb(px.data(), mask_ptr, c.w, c.h, pal, c.kind, got);
    hg::upscale_art_4x_argb(px.data(), mask_ptr, c.w, c.h, pal, c.kind,
                            got_pool, &pool);
    CHECK(ref.size() == static_cast<std::size_t>(c.w * c.h * 16));
    overall_max_diff = std::max(overall_max_diff, max_channel_diff(ref, got));
    CHECK(got_pool == got);
  }
  std::cout << "[san4-hdgen] sRGB-LUT upscaler max per-channel diff vs "
               "reference: "
            << overall_max_diff << " LSB\n";
  CHECK(overall_max_diff == 0);

  // 640x400 CG timing (reduced under sanitizers).
  const int bw = kSanitizedBuild ? 160 : 640;
  const int bh = kSanitizedBuild ? 100 : 400;
  const auto px = make_synthetic_art(bw, bh, 0xC6);
  const auto pal = synthetic_palette(0xC6);
  std::vector<std::uint32_t> ref;
  std::vector<std::uint32_t> single;
  std::vector<std::uint32_t> pooled;
  san4::ThreadPool big_pool(0);
  auto time_ms = [](auto&& fn) {
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0)
        .count();
  };
  const double t_ref = time_ms([&]() {
    san4::test_ref::upscale_art_4x_argb_reference(
        px.data(), nullptr, bw, bh, pal, hg::ArtAssetKind::CgIllustration, ref);
  });
  const double t_single = time_ms([&]() {
    hg::upscale_art_4x_argb(px.data(), nullptr, bw, bh, pal,
                            hg::ArtAssetKind::CgIllustration, single);
  });
  const double t_pool = time_ms([&]() {
    hg::upscale_art_4x_argb(px.data(), nullptr, bw, bh, pal,
                            hg::ArtAssetKind::CgIllustration, pooled, &big_pool);
  });
  CHECK(single == ref);
  CHECK(pooled == ref);
  std::cout << "[san4-bench] hd_art " << bw << "x" << bh << " -> " << bw * 4
            << "x" << bh * 4 << ": reference " << t_ref << " ms, LUT "
            << t_single << " ms, LUT+pool(" << big_pool.size() << ") "
            << t_pool << " ms\n";
}

void test_grp_census_profile_rows_and_frame_stats() {
  // GrpCensus bookkeeping and slice-sampling attribution.
  {
    san4::GrpCensus census;
    census.credit_grp_slice(0, 100);  // no call yet: ignored
    census.record_call(0x0F, 0x05, 4);
    census.credit_grp_slice(900, 1500);
    census.credit_grp_slice(1500, 1500);  // empty slice: ignored
    census.record_call(0x13, 0x01, 99);
    census.credit_grp_slice(1500, 1600);
    census.record_call(0x0F, 0x02, 4);
    census.credit_grp_slice(3000, 3050);
    census.add_guest_instrs(10000);
    CHECK(census.total_calls() == 3u);
    CHECK(census.total_guest_instrs() == 10000u);
    const auto rows = census.sorted_rows();
    CHECK(rows.size() == 3u);
    CHECK(rows[0].ah == 0x0F && rows[0].al_mode == 1 && rows[0].rop == 4 &&
          rows[0].calls == 1u && rows[0].instrs == 600u);
    CHECK(rows[1].ah == 0x13 && rows[1].al_mode == 1 &&
          rows[1].rop == san4::GrpCensus::kRopOther && rows[1].calls == 1u &&
          rows[1].instrs == 100u);
    CHECK(rows[2].ah == 0x0F && rows[2].al_mode == 2 && rows[2].instrs == 50u);
    std::ostringstream os;
    census.print(os);
    CHECK(os.str().find("[san4-census] 0x0F    2    4") != std::string::npos);
    CHECK(os.str().find(">15") != std::string::npos);
    CHECK(os.str().find("6.00%") != std::string::npos);
    CHECK(os.str().find("attributed=750 (7.50%)") != std::string::npos);
  }

  // X86Machine: INT F1h calls are counted, frame stats report instructions
  // and the budget stop reason; disabling drops the census.
  {
    san4::X86Machine m;
    CHECK(m.grp_census() == nullptr);
    m.set_grp_census_enabled(true);
    static constexpr std::uint8_t kCode[] = {
        0xB4, 0x1D,  // mov ah, 0x1D
        0xCD, 0xF1,  // int 0xF1
        0xB8, 0x02, 0x08,  // mov ax, 0x0802
        0xCD, 0xF1,  // int 0xF1
        0xB8, 0x02, 0x08,  // mov ax, 0x0802
        0xCD, 0xF1,  // int 0xF1
        0xEB, 0xFE,  // jmp $
    };
    m.load_synthetic_code(0x2000u, 0x0100u, kCode);
    m.run_frame(1.0 / 60.0, 0.002);
    const san4::GuestFrameStats& st = m.last_frame_stats();
    CHECK(st.guest_instrs > 1000u);
    CHECK(st.yield_reason == san4::GuestYieldReason::Budget);
    CHECK(std::string(san4::guest_yield_reason_name(st.yield_reason)) ==
          "budget");
    const san4::GrpCensus* census = m.grp_census();
    CHECK(census != nullptr);
    CHECK(census->total_calls() == 3u);
    CHECK(census->total_guest_instrs() == st.guest_instrs);
    const auto rows = census->sorted_rows();
    CHECK(rows.size() == 2u);
    CHECK(rows[0].ah == 0x08 && rows[0].al_mode == 2 && rows[0].calls == 2u);
    CHECK(rows[1].ah == 0x1D && rows[1].calls == 1u);
    m.set_grp_census_enabled(false);
    CHECK(m.grp_census() == nullptr);
    m.run_frame(1.0 / 60.0, 0.001);
    CHECK(m.last_frame_stats().guest_instrs > 0u);
  }

  // FrameProfiler TSV header and row format.
  {
    ScopedTempDir tmp;
    const auto path = tmp.path() / "frames.tsv";
    {
      san4::FrameProfiler prof;
      CHECK(!prof.enabled());
      CHECK(prof.open(path));
      CHECK(prof.enabled());
      san4::FrameProfileRow row;
      row.frame = 7;
      row.guest_instrs = 66000;
      row.run_frame_ms = 6.25;
      row.budget_capped = true;
      row.yield_reason = "budget";
      row.upscale_ms = 1.5;
      row.composite_ms = 2.0;
      row.upload_ms = 0.125;
      row.present_ms = 0.5;
      row.dirty_tiles = 12;
      row.dirty_px = 4096;
      prof.write(row);
      prof.flush();
    }
    std::ifstream in(path);
    std::string header;
    std::string line;
    std::getline(in, header);
    std::getline(in, line);
    CHECK(header ==
          "frame\tguest_instrs\trun_frame_ms\tbudget_capped\tyield_reason\t"
          "upscale_ms\tcomposite_ms\tupload_ms\tpresent_ms\tdirty_tiles\t"
          "dirty_px");
    CHECK(line ==
          "7\t66000\t6.250\t1\tbudget\t1.500\t2.000\t0.125\t0.500\t12\t4096");
    san4::FrameProfiler bad;
    CHECK(!bad.open(tmp.path() / "missing_dir" / "x.tsv"));
    CHECK(!bad.enabled());
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Stage 2: HdPackManager used-color masks, LUT prebuild/LRU, parallel span
// compositor, async dynamic-CG re-upscale, tile_dict index.
// ---------------------------------------------------------------------------

using HdPeer = san4::HdPackManagerTestPeer;

// Applies a 16-color 4-bit BRG palette (game-color order) as the latched
// palette, without leaving a fade active.
void set_video_palette_brg48(san4::VesaPlanarVideo& vid,
                             const std::array<std::uint8_t, 48>& brg48) {
  vid.begin_smooth_fade(0u, 16u, brg48.data(), brg48.data(),
                        san4::kGrpSlotMap.data(), 0.01);
  vid.finish_fade_immediately();
  vid.latch_frame();
  vid.clear_hd_occlude_bits();
}

std::array<std::uint8_t, 48> random_brg48(std::uint64_t seed) {
  TestRng rng(seed);
  std::array<std::uint8_t, 48> brg{};
  for (std::uint8_t& c : brg) {
    c = static_cast<std::uint8_t>(rng.below(16u));
  }
  return brg;
}

// Encodes VGA-slot pixels as GRP600C byte-interleaved planar data.
std::vector<std::uint8_t> encode_planar_from_slots(int w, int h, int planes,
                                                   const std::uint8_t* slots) {
  const int cols = (w + 7) / 8;
  std::vector<std::uint8_t> out(static_cast<std::size_t>(cols * planes * h),
                                0u);
  for (int y = 0; y < h; ++y) {
    for (int col = 0; col < cols; ++col) {
      std::uint8_t pb[4]{0u, 0u, 0u, 0u};
      for (int bit = 0; bit < 8; ++bit) {
        const int x = col * 8 + bit;
        if (x >= w) break;
        // kGrpSlotMap is an involution: slot -> game color uses kGrpSlotMap.
        const std::uint8_t gc = san4::kGrpSlotMap[slots[y * w + x] & 0x0Fu];
        for (int p = 0; p < planes; ++p) {
          pb[p] |= static_cast<std::uint8_t>(((gc >> p) & 1u)
                                             << static_cast<unsigned>(7 - bit));
        }
      }
      for (int p = 0; p < planes; ++p) {
        out[static_cast<std::size_t>((y * cols + col) * planes + p)] = pb[p];
      }
    }
  }
  return out;
}

std::string format_palette_csv(const std::array<std::uint32_t, 16>& pal) {
  std::string s;
  char buf[16];
  for (int i = 0; i < 16; ++i) {
    if (i > 0) s.push_back(',');
    std::snprintf(buf, sizeof(buf), "%06X",
                  pal[static_cast<std::size_t>(i)] & 0x00FFFFFFu);
    s += buf;
  }
  return s;
}

std::string hex16(std::uint64_t v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

std::uint16_t slot_bits_of_game_colors(std::initializer_list<int> gcs) {
  std::uint16_t m = 0u;
  for (int gc : gcs) {
    m = static_cast<std::uint16_t>(
        m | (1u << san4::kGrpSlotMap[static_cast<std::size_t>(gc)]));
  }
  return m;
}

void test_normalize_dirty_rects_disjoint_cover() {
  constexpr int kW = 61;
  constexpr int kH = 47;
  TestRng rng(0x5EED0001u);
  std::vector<san4::UpscaleDirtyRect> out;
  for (int iter = 0; iter < 300; ++iter) {
    std::vector<san4::UpscaleDirtyRect> in;
    const int n = static_cast<int>(rng.below(14u));
    for (int i = 0; i < n; ++i) {
      san4::UpscaleDirtyRect r;
      r.has_changes = rng.below(8u) != 0u;
      r.x = static_cast<int>(rng.below(kW + 20)) - 10;
      r.y = static_cast<int>(rng.below(kH + 20)) - 10;
      r.w = static_cast<int>(rng.below(30u));
      r.h = static_cast<int>(rng.below(30u));
      in.push_back(r);
    }
    std::vector<int> want(kW * kH, 0);
    for (const auto& r : in) {
      if (!r.has_changes) continue;
      for (int y = std::max(r.y, 0); y < std::min(r.y + r.h, kH); ++y) {
        for (int x = std::max(r.x, 0); x < std::min(r.x + r.w, kW); ++x) {
          want[static_cast<std::size_t>(y * kW + x)] = 1;
        }
      }
    }
    const san4::UpscaleDirtyRect u =
        san4::normalize_dirty_rects(in, kW, kH, out);
    std::vector<int> got(kW * kH, 0);
    int bx0 = kW, by0 = kH, bx1 = 0, by1 = 0;
    for (const auto& r : out) {
      CHECK(r.has_changes && r.w > 0 && r.h > 0);
      CHECK(r.x >= 0 && r.y >= 0 && r.x + r.w <= kW && r.y + r.h <= kH);
      for (int y = r.y; y < r.y + r.h; ++y) {
        for (int x = r.x; x < r.x + r.w; ++x) {
          ++got[static_cast<std::size_t>(y * kW + x)];
        }
      }
      bx0 = std::min(bx0, r.x);
      by0 = std::min(by0, r.y);
      bx1 = std::max(bx1, r.x + r.w);
      by1 = std::max(by1, r.y + r.h);
    }
    // Exactly-once coverage of exactly the input pixels.
    CHECK(got == want);
    CHECK(u.has_changes == !out.empty());
    if (u.has_changes) {
      CHECK(u.x == bx0 && u.y == by0 && u.w == bx1 - bx0 && u.h == by1 - by0);
    }
  }
  // Identical stacked rows merge into one rect.
  std::vector<san4::UpscaleDirtyRect> rows;
  for (int y = 0; y < 40; y += 4) {
    rows.push_back({.has_changes = true, .x = 3, .y = y, .w = 10, .h = 6});
  }
  san4::normalize_dirty_rects(rows, 100, 100, out);
  CHECK(out.size() == 1u);
  CHECK(out[0].x == 3 && out[0].y == 0 && out[0].w == 10 && out[0].h == 42);
}

void check_hd_composite_parallel_parity_and_spans() {
  auto vid = std::make_unique<san4::VesaPlanarVideo>();
  vid->reset_mode(0x0102u);
  fill_video_noise(*vid, 0xABCDu);
  set_video_palette_brg48(*vid, random_brg48(0x1234u));

  san4::HdPackManager hd;
  HdPeer::SyntheticRegionSpec spec;
  spec.seed = 0x77u;
  spec.x = 101;
  spec.y = 93;
  spec.w = 270;
  spec.h = 170;
  spec.num_regions = 40;
  spec.num_palettes = 6;
  HdPeer::seed_synthetic_regions(hd, *vid, spec);
  CHECK(hd.active_overlay_count() > 30u);

  san4::ThreadPool pool(4);
  TestRng rng(0x99u);
  for (int scale : {1, 2, 4}) {
    const int out_w = 800 * scale;
    const int out_h = 600 * scale;
    std::vector<std::uint32_t> base(static_cast<std::size_t>(out_w) * out_h);
    for (std::uint32_t& p : base) {
      p = 0xFF000000u | (rng.next() & 0x00FFFFFFu);
    }
    const san4::UpscaleDirtyRect full{
        .has_changes = true, .x = 0, .y = 0, .w = out_w, .h = out_h};

    std::vector<std::uint32_t> ref = base;
    san4::UpscaleDirtyRect ref_dirty = full;
    HdPeer::composite_overlays_reference(hd, ref, out_w, out_h, scale, *vid,
                                         ref_dirty);
    CHECK(ref != base);

    // Legacy single-rect entry point: serial and pooled are bit-identical to
    // the reference routine.
    for (san4::ThreadPool* p : {static_cast<san4::ThreadPool*>(nullptr), &pool}) {
      hd.set_thread_pool(p);
      std::vector<std::uint32_t> got = base;
      san4::UpscaleDirtyRect dirty = full;
      hd.composite_overlays(got, out_w, out_h, scale, *vid, dirty);
      CHECK(got == ref);
      CHECK(dirty.has_changes && dirty.x == 0 && dirty.y == 0 &&
            dirty.w == out_w && dirty.h == out_h);
    }

    // Span entry point with overlapping, unaligned and out-of-bounds rects:
    // pixels in the union equal the full-frame result (each blended exactly
    // once; translucent pixels would differ if blended twice) and every other
    // pixel is untouched.
    std::vector<san4::UpscaleDirtyRect> spans;
    for (int i = 0; i < 14; ++i) {
      san4::UpscaleDirtyRect r;
      r.has_changes = true;
      r.x = (90 + static_cast<int>(rng.below(300u))) * scale +
            static_cast<int>(rng.below(4u));
      r.y = (80 + static_cast<int>(rng.below(200u))) * scale +
            static_cast<int>(rng.below(4u));
      r.w = static_cast<int>(rng.below(120u)) * scale + 1;
      r.h = static_cast<int>(rng.below(60u)) * scale + 1;
      spans.push_back(r);
    }
    spans.push_back({.has_changes = true,
                     .x = out_w - 7,
                     .y = out_h - 9,
                     .w = 50,
                     .h = 50});
    spans.push_back(spans[3]);  // exact duplicate
    hd.set_thread_pool(&pool);
    std::vector<std::uint32_t> got = base;
    std::vector<san4::UpscaleDirtyRect> out_rects;
    const san4::UpscaleDirtyRect u = hd.composite_overlays(
        got, out_w, out_h, scale, *vid, spans, out_rects);
    std::vector<std::uint32_t> expected = base;
    std::vector<std::uint8_t> covered(base.size(), 0u);
    for (const auto& r : spans) {
      for (int y = std::max(r.y, 0); y < std::min(r.y + r.h, out_h); ++y) {
        for (int x = std::max(r.x, 0); x < std::min(r.x + r.w, out_w); ++x) {
          const std::size_t i = static_cast<std::size_t>(y) * out_w + x;
          expected[i] = ref[i];
          covered[i] = 1u;
        }
      }
    }
    CHECK(got == expected);
    std::uint64_t covered_px = 0;
    for (std::uint8_t c : covered) covered_px += c;
    std::uint64_t rect_px = 0;
    for (const auto& r : out_rects) {
      rect_px += static_cast<std::uint64_t>(r.w) * static_cast<std::uint64_t>(r.h);
    }
    CHECK(u.has_changes && rect_px == covered_px);
    hd.set_thread_pool(nullptr);
  }

  // No HD-only changes and no input rects: nothing composited or reported.
  {
    std::vector<std::uint32_t> canvas(1600u * 1200u, 0xFF000000u);
    std::vector<san4::UpscaleDirtyRect> out_rects{{.has_changes = true}};
    const auto u = hd.composite_overlays(canvas, 1600, 1200, 2, *vid, {},
                                         out_rects);
    CHECK(!u.has_changes && out_rects.empty());
    CHECK(!hd.has_pending_hd_changes());
  }

  // clear_overlays() of visible HD pixels is an HD-only change: the next
  // composite reports the whole screen even without base-frame changes.
  hd.clear_overlays();
  CHECK(hd.has_pending_hd_changes());
  {
    std::vector<std::uint32_t> canvas(1600u * 1200u, 0xFF000000u);
    san4::UpscaleDirtyRect dirty{};
    hd.composite_overlays(canvas, 1600, 1200, 2, *vid, dirty);
    CHECK(dirty.has_changes && dirty.x == 0 && dirty.y == 0 &&
          dirty.w == 1600 && dirty.h == 1200);
    CHECK(!hd.has_pending_hd_changes());
    san4::UpscaleDirtyRect again{};
    hd.composite_overlays(canvas, 1600, 1200, 2, *vid, again);
    CHECK(!again.has_changes);
  }
}

void test_hd_composite_parallel_parity_and_spans() {
  check_hd_composite_parallel_parity_and_spans();
  // Again on the scalar kernels, the production path of non-AVX2 CPUs.
  const san4::simd::ScopedLevelOverride scalar(san4::simd::SimdLevel::Scalar);
  CHECK(san4::simd::active_level() == san4::simd::SimdLevel::Scalar);
  check_hd_composite_parallel_parity_and_spans();
}

void test_simd_level_override() {
  using san4::simd::SimdLevel;
  const SimdLevel detected = san4::simd::detected_level();
  CHECK(san4::simd::active_level() == detected);
  {
    const san4::simd::ScopedLevelOverride scalar(SimdLevel::Scalar);
    CHECK(san4::simd::active_level() == SimdLevel::Scalar);
    {
      // A cap above the detected level never enables unsupported kernels.
      const san4::simd::ScopedLevelOverride avx2(SimdLevel::Avx2);
      CHECK(san4::simd::active_level() == detected);
    }
    CHECK(san4::simd::active_level() == SimdLevel::Scalar);

    // The level-less entry points follow the cap and match the explicit
    // scalar kernels bit for bit.
    TestRng rng(0x51D0u);
    std::vector<std::uint32_t> hd(4u * 64u);
    for (std::uint32_t& p : hd) {
      p = random_hd_pixel(rng);
    }
    std::vector<std::uint32_t> base(64u);
    for (std::uint32_t& p : base) {
      p = 0xFF000000u | (rng.next() & 0x00FFFFFFu);
    }
    auto got = base;
    auto want = base;
    san4::simd::blend_row_box(got.data(), hd.data(), 64, 2, 32, nullptr);
    san4::simd::blend_row_box(SimdLevel::Scalar, want.data(), hd.data(), 64, 2,
                              32, nullptr);
    CHECK(got == want);
  }
  CHECK(san4::simd::active_level() == detected);
}

void test_hd_lut_cache_lru_bound() {
  auto vid = std::make_unique<san4::VesaPlanarVideo>();
  vid->reset_mode(0x0102u);
  set_video_palette_brg48(*vid, random_brg48(0x4321u));

  san4::HdPackManager hd;
  HdPeer::SyntheticRegionSpec spec;
  spec.seed = 0x31u;
  spec.x = 0;
  spec.y = 0;
  spec.w = 800;
  spec.h = 120;
  spec.num_regions = 400;  // 400 distinct (palette, mask) keys
  spec.num_palettes = 1;
  spec.unique_masks = true;
  HdPeer::seed_synthetic_regions(hd, *vid, spec);

  san4::ThreadPool pool(4);
  hd.set_thread_pool(&pool);
  for (int round = 0; round < 2; ++round) {
    std::vector<std::uint32_t> base(1600u * 1200u, 0xFF203040u);
    std::vector<std::uint32_t> ref = base;
    std::vector<std::uint32_t> got = base;
    const san4::UpscaleDirtyRect full{
        .has_changes = true, .x = 0, .y = 0, .w = 1600, .h = 1200};
    san4::UpscaleDirtyRect d1 = full;
    san4::UpscaleDirtyRect d2 = full;
    HdPeer::composite_overlays_reference(hd, ref, 1600, 1200, 2, *vid, d1);
    hd.composite_overlays(got, 1600, 1200, 2, *vid, d2);
    // More keys than the cache holds in one frame: every LUT stays valid for
    // the whole frame and the cache is trimmed afterwards.
    CHECK(got == ref);
    CHECK(hd.lut_cache_size() == san4::HdPackManager::kMaxLutCacheEntries);
    // A palette change invalidates every cached LUT (rebuilt in place).
    set_video_palette_brg48(*vid, random_brg48(0x8888u + round));
  }
  hd.set_thread_pool(nullptr);
}

void test_hd_used_color_mask_identity_and_propagation() {
  ScopedTempDir tmp;
  const auto pack_dir = tmp.path() / "hd_pack";
  std::filesystem::create_directories(pack_dir / "tiles");

  auto vid = std::make_unique<san4::VesaPlanarVideo>();
  vid->reset_mode(0x0102u);
  std::array<std::uint8_t, 48> brg = random_brg48(0x600Du);
  set_video_palette_brg48(*vid, brg);
  const std::array<std::uint32_t, 16> scene_pal = vid->latched_palette_argb();

  // A bank-0 terrain tile drawn with game colors 1..6 and a bank-1 portrait
  // part drawn with game colors 9 and 10, both referenced to the scene
  // palette, so both start ungraded.
  std::array<std::uint8_t, 256> tile_slots{};
  std::array<std::uint8_t, 256> port_slots{};
  for (int p = 0; p < 256; ++p) {
    tile_slots[static_cast<std::size_t>(p)] =
        san4::kGrpSlotMap[static_cast<std::size_t>(1 + p % 6)];
    port_slots[static_cast<std::size_t>(p)] =
        san4::kGrpSlotMap[static_cast<std::size_t>(9 + (p & 1))];
  }
  const std::uint64_t tile_hash =
      san4::HdPackManager::hash_sprite_4bit(16, 16, tile_slots.data());
  const std::uint64_t port_hash =
      san4::HdPackManager::hash_sprite_4bit(16, 16, port_slots.data());
  constexpr std::uint32_t kTileHd = 0xFF3366AAu;
  constexpr std::uint32_t kPortHd = 0xFFAA7744u;
  const std::vector<std::uint32_t> tile_px(64u * 64u, kTileHd);
  const std::vector<std::uint32_t> port_px(64u * 64u, kPortHd);
  CHECK(san4::HdPackManager::save_argb_png(pack_dir / "tiles" / "t.png", 64,
                                           64, tile_px.data()));
  CHECK(san4::HdPackManager::save_argb_png(pack_dir / "tiles" / "p.png", 64,
                                           64, port_px.data()));
  {
    std::ofstream mf(pack_dir / "hd_manifest.tsv", std::ios::binary);
    const std::string csv = format_palette_csv(scene_pal);
    mf << hex16(tile_hash) << "\t16\t16\t0\ttiles/t.png\t" << csv << "\n";
    mf << hex16(port_hash) << "\t16\t16\t1\ttiles/p.png\t" << csv << "\n";
  }

  san4::HdPackManager hd;
  hd.set_pack_dir(pack_dir);
  const san4::GrpClipRect clip{0, 0, 799, 599};
  hd.on_planar_blit_source(0, 0, 16, 16, 4, 0u, 0u, 0u, 0u, clip,
                           encode_planar_from_slots(16, 16, 4, tile_slots.data()),
                           *vid);
  hd.on_planar_blit_source(32, 0, 16, 16, 4, 0u, 0u, 0u, 0u, clip,
                           encode_planar_from_slots(16, 16, 4, port_slots.data()),
                           *vid);
  const std::uint16_t tile_mask = slot_bits_of_game_colors({1, 2, 3, 4, 5, 6});
  const std::uint16_t port_mask = slot_bits_of_game_colors({9, 10});
  CHECK(HdPeer::region_used_mask_at(hd, 0, 0) == tile_mask);
  CHECK(HdPeer::region_used_mask_at(hd, 32, 0) == port_mask);

  std::vector<std::uint32_t> frame(3200u * 2400u, 0xFF000000u);
  auto composite_px = [&](int lx, int ly) {
    std::fill(frame.begin(), frame.end(), 0xFF000000u);
    san4::UpscaleDirtyRect dirty{
        .has_changes = true, .x = 0, .y = 0, .w = 3200, .h = 2400};
    hd.composite_overlays(frame, 3200, 2400, 4, *vid, dirty);
    return frame[static_cast<std::size_t>(ly * 4 + 2) * 3200u +
                 static_cast<std::size_t>(lx * 4 + 2)];
  };
  CHECK(composite_px(0, 0) == kTileHd);
  CHECK(composite_px(32, 0) == kPortHd);

  // Recolor game colors 7..15 (a portrait load): the terrain tile never uses
  // them, so it must stay exactly ungraded.
  auto brg_upper = brg;
  for (int gc = 7; gc < 16; ++gc) {
    brg_upper[static_cast<std::size_t>(gc * 3 + 0)] = 15u;
    brg_upper[static_cast<std::size_t>(gc * 3 + 1)] = 15u;
    brg_upper[static_cast<std::size_t>(gc * 3 + 2)] = 0u;
  }
  brg_upper[9 * 3 + 0] = brg[9 * 3 + 0];
  brg_upper[9 * 3 + 1] = brg[9 * 3 + 1];
  brg_upper[9 * 3 + 2] = brg[9 * 3 + 2];
  brg_upper[10 * 3 + 0] = brg[10 * 3 + 0];
  brg_upper[10 * 3 + 1] = brg[10 * 3 + 1];
  brg_upper[10 * 3 + 2] = brg[10 * 3 + 2];
  set_video_palette_brg48(*vid, brg_upper);
  CHECK(composite_px(0, 0) == kTileHd);
  // The bank-1 part keeps colors 9 and 10, so it also stays ungraded even
  // though other upper-bank colors changed.
  CHECK(composite_px(32, 0) == kPortHd);

  // Changing a color the asset uses grades it.
  auto brg_used = brg_upper;
  brg_used[3 * 3 + 0] = static_cast<std::uint8_t>(15u - brg[3 * 3 + 0]);
  brg_used[3 * 3 + 1] = static_cast<std::uint8_t>(15u - brg[3 * 3 + 1]);
  brg_used[3 * 3 + 2] = static_cast<std::uint8_t>(15u - brg[3 * 3 + 2]);
  brg_used[9 * 3 + 1] = static_cast<std::uint8_t>(15u - brg[9 * 3 + 1]);
  set_video_palette_brg48(*vid, brg_used);
  CHECK(composite_px(0, 0) != kTileHd);
  CHECK(composite_px(32, 0) != kPortHd);
  set_video_palette_brg48(*vid, brg);

  // Propagation: AH=0x17 copy, AH=0x10/0x18 patch capture and restore.
  hd.on_vram_copy_rect(0, 0, 16, 16, 200, 100);
  CHECK(HdPeer::region_used_mask_at(hd, 200, 100) == tile_mask);
  CHECK(HdPeer::region_used_mask_at(hd, 215, 115) == tile_mask);
  const auto patch = hd.capture_patch_snapshot(0, 0, 48, 16, clip, *vid);
  CHECK(patch.has_any_hd);
  CHECK(patch.used_mask == (tile_mask | port_mask));
  // Save the 16x16 tile background, wipe it with an XOR blit, then restore
  // it from the saved patch (the synthetic VRAM is all slot 0).
  hd.on_vram_read_rect(0, 0, 16, 16, clip, *vid);
  hd.on_planar_blit_source(0, 0, 16, 16, 4, 0u, 0u, 2u, 0u, clip,
                           encode_planar_from_slots(16, 16, 4, tile_slots.data()),
                           *vid);
  CHECK(HdPeer::region_used_mask_at(hd, 0, 0) == 0u);
  const std::array<std::uint8_t, 256> zero_slots{};
  hd.on_planar_blit_source(0, 0, 16, 16, 4, 0u, 0u, 0u, 0u, clip,
                           encode_planar_from_slots(16, 16, 4, zero_slots.data()),
                           *vid);
  CHECK(HdPeer::region_used_mask_at(hd, 0, 0) == tile_mask);
  set_video_palette_brg48(*vid, brg_upper);
  CHECK(composite_px(0, 0) == kTileHd);

  // HD-only changes reach the upscaler as dirty tiles via flush_dirty_tiles.
  std::array<std::uint8_t, san4::VesaPlanarVideo::kTileCount> tiles{};
  bool pal_changed = false;
  hd.flush_dirty_tiles(*vid);
  vid->consume_dirty_tiles(tiles, pal_changed);
  hd.flush_dirty_tiles(*vid);
  CHECK(!vid->consume_dirty_tiles(tiles, pal_changed));
  hd.on_vram_copy_rect(0, 0, 16, 16, 400, 304);
  hd.flush_dirty_tiles(*vid);
  CHECK(vid->consume_dirty_tiles(tiles, pal_changed));
  CHECK(std::count(tiles.begin(), tiles.end(), 1u) == 1);
  CHECK(tiles[19u * san4::VesaPlanarVideo::kTilesX + 25u] == 1u);
  hd.clear_overlays();
  hd.flush_dirty_tiles(*vid);
  CHECK(vid->consume_dirty_tiles(tiles, pal_changed));
  CHECK(std::count(tiles.begin(), tiles.end(), 1u) ==
        san4::VesaPlanarVideo::kTileCount);
}

void test_hd_tile_dict_exact_match_index() {
  ScopedTempDir tmp;
  const auto pack_dir = tmp.path() / "hd_pack";
  std::filesystem::create_directories(pack_dir / "tiles");

  std::array<std::uint8_t, 256> a_slots{};
  std::array<std::uint8_t, 256> b_slots{};
  for (int p = 0; p < 256; ++p) {
    a_slots[static_cast<std::size_t>(p)] =
        san4::kGrpSlotMap[static_cast<std::size_t>(1 + (p * 7) % 5)];
    b_slots[static_cast<std::size_t>(p)] =
        san4::kGrpSlotMap[static_cast<std::size_t>(2 + (p * 3) % 4)];
  }
  // Entries: 0 = A (red), 1 = overlay, 2 = A duplicate (blue), 3 = B (green).
  // HD hashes differ from the cells' own hashes so blits resolve through the
  // tile dictionary rather than the manifest.
  const std::array<std::uint64_t, 4> hd_hashes = {
      0x1111000000000001ull, 0x1111000000000002ull, 0x1111000000000003ull,
      0x1111000000000004ull};
  const std::array<std::uint32_t, 4> colors = {0xFFDD2020u, 0x00000000u,
                                               0xFF2020DDu, 0xFF20DD20u};
  std::array<san4::codecs::PackedTileDictEntry, 4> dict{};
  for (std::size_t i = 0; i < 4u; ++i) {
    const auto& slots = (i == 3u) ? b_slots : a_slots;
    dict[i].hd_hash = hd_hashes[i];
    dict[i].is_overlay = (i == 1u) ? 1u : 0u;
    dict[i].non_zero_count = (i == 1u) ? 8u : 255u;
    dict[i].full_zero_256 = (i == 1u) ? 0u : 1u;
    for (int k = 0; k < 128; ++k) {
      dict[i].packed_slots[k] = static_cast<std::uint8_t>(
          (slots[static_cast<std::size_t>(k * 2)] & 0x0Fu) |
          ((slots[static_cast<std::size_t>(k * 2 + 1)] & 0x0Fu) << 4));
    }
    if (i == 1u) {
      dict[i].zero_mask[0] = 0xFFu;
    } else {
      std::memset(dict[i].zero_mask, 0xFF, 32u);
    }
    const std::vector<std::uint32_t> px(64u * 64u, colors[i]);
    CHECK(san4::HdPackManager::save_argb_png(
        pack_dir / "tiles" / ("d" + std::to_string(i) + ".png"), 64, 64,
        px.data()));
  }
  {
    std::ofstream tfs(pack_dir / "tiles" / "tile_dict.bin", std::ios::binary);
    tfs.write(reinterpret_cast<const char*>(dict.data()), sizeof(dict));
    std::ofstream mf(pack_dir / "hd_manifest.tsv", std::ios::binary);
    for (std::size_t i = 0; i < 4u; ++i) {
      mf << hex16(hd_hashes[i]) << "\t16\t16\t0\ttiles/d" << i << ".png\n";
    }
  }

  auto vid = std::make_unique<san4::VesaPlanarVideo>();
  vid->reset_mode(0x0102u);
  set_video_palette_brg48(*vid, random_brg48(0x7u));
  san4::HdPackManager hd;
  hd.set_pack_dir(pack_dir);
  CHECK(HdPeer::tile_dict_index_size(hd) == 3u);

  const san4::GrpClipRect clip{0, 0, 799, 599};
  auto near_slots = a_slots;  // 236 of 256 slots match A: linear-scan path
  for (int p = 0; p < 20; ++p) {
    near_slots[static_cast<std::size_t>(p * 12)] = san4::kGrpSlotMap[7];
  }
  hd.on_planar_blit_source(0, 0, 16, 16, 4, 0u, 0u, 0u, 0u, clip,
                           encode_planar_from_slots(16, 16, 4, a_slots.data()),
                           *vid);
  hd.on_planar_blit_source(16, 0, 16, 16, 4, 0u, 0u, 0u, 0u, clip,
                           encode_planar_from_slots(16, 16, 4, b_slots.data()),
                           *vid);
  hd.on_planar_blit_source(32, 0, 16, 16, 4, 0u, 0u, 0u, 0u, clip,
                           encode_planar_from_slots(16, 16, 4, near_slots.data()),
                           *vid);
  std::vector<std::uint32_t> frame(800u * 600u, 0xFF000000u);
  san4::UpscaleDirtyRect dirty{
      .has_changes = true, .x = 0, .y = 0, .w = 800, .h = 600};
  hd.composite_overlays(frame, 800, 600, 1, *vid, dirty);
  // The first exact match wins over the later duplicate, as in the linear
  // scan.
  CHECK(frame[5u * 800u + 5u] == colors[0]);
  CHECK(frame[5u * 800u + 21u] == colors[3]);
  CHECK(frame[15u * 800u + 32u + 15u] == colors[0]);
}

// A synthetic HD pack holding one dynamic-palette CG (`bank_hint == 3`,
// opaque neon HD pixels) of kW x kH, placed at (kX, kY) by its planar blit.
struct DynamicCgPack {
  static constexpr int kW = 32;
  static constexpr int kH = 24;
  static constexpr int kX = 320;
  static constexpr int kY = 160;
  static constexpr std::uint32_t kNeon = 0xFF00FF00u;

  ScopedTempDir tmp;
  std::filesystem::path dir;
  std::vector<std::uint8_t> raw;  // GRP600C planar source of the CG
  bool ok = false;

  DynamicCgPack() : dir(tmp.path() / "hd_pack") {
    std::filesystem::create_directories(dir / "cg");
    std::vector<std::uint8_t> slots(kW * kH);
    for (int y = 0; y < kH; ++y) {
      for (int x = 0; x < kW; ++x) {
        slots[static_cast<std::size_t>(y * kW + x)] = san4::kGrpSlotMap
            [static_cast<std::size_t>(((x + y) & 1) ? 4 : 5 + (x / 8) % 3)];
      }
    }
    const std::uint64_t hash =
        san4::HdPackManager::hash_sprite_4bit(kW, kH, slots.data());
    const std::vector<std::uint32_t> neon(kW * 4u * kH * 4u, kNeon);
    ok = san4::HdPackManager::save_argb_png(dir / "cg" / "dyn.png", kW * 4,
                                            kH * 4, neon.data());
    std::ofstream mf(dir / "hd_manifest.tsv", std::ios::binary);
    mf << hex16(hash) << "\t" << kW << "\t" << kH << "\t3\tcg/dyn.png\t"
       << format_palette_csv(san4::codecs::default_san4_master_palette())
       << "\n";
    raw = encode_planar_from_slots(kW, kH, 4, slots.data());
  }

  void blit(san4::HdPackManager& hd, const san4::VesaPlanarVideo& vid) const {
    hd.on_planar_blit_source(kX, kY, kW, kH, 4, 0u, 0u, 0u, 0u,
                             san4::GrpClipRect{0, 0, 799, 599}, raw, vid);
  }
};

// Scene palettes used with DynamicCgPack: random but clearly non-black, so
// dynamic CGs re-upscale for them.
std::array<std::uint8_t, 48> dynamic_cg_scene(std::uint64_t seed) {
  auto brg = random_brg48(seed);
  brg[4 * 3 + 1] = 13u;
  return brg;
}

void test_hd_async_dynamic_cg_results_and_staleness() {
  const DynamicCgPack cg_pack;
  CHECK(cg_pack.ok);
  const auto& pack_dir = cg_pack.dir;
  constexpr int kCgW = DynamicCgPack::kW;
  constexpr int kCgH = DynamicCgPack::kH;
  constexpr int kCgX = DynamicCgPack::kX;
  constexpr int kCgY = DynamicCgPack::kY;
  const auto& cg_raw = cg_pack.raw;

  auto vid = std::make_unique<san4::VesaPlanarVideo>();
  vid->reset_mode(0x0102u);
  const std::array<std::uint8_t, 48> black{};
  const auto scene = dynamic_cg_scene;

  san4::HdPackManager hd_async;
  san4::HdPackManager hd_sync;
  hd_async.set_pack_dir(pack_dir);
  hd_sync.set_pack_dir(pack_dir);
  san4::ThreadPool pool(1);
  hd_async.set_async_pool(&pool);

  const san4::GrpClipRect clip{0, 0, 799, 599};
  auto blit_cg = [&]() {
    set_video_palette_brg48(*vid, black);
    hd_async.on_planar_blit_source(kCgX, kCgY, kCgW, kCgH, 4, 0u, 0u, 0u, 0u,
                                   clip, cg_raw, *vid);
    hd_sync.on_planar_blit_source(kCgX, kCgY, kCgW, kCgH, 4, 0u, 0u, 0u, 0u,
                                  clip, cg_raw, *vid);
  };

  // Holds the single pool worker so queued CG jobs cannot start.
  std::atomic<bool> release{false};
  std::atomic<bool> blocked{false};
  // Unblocks the worker on every exit path (a failed CHECK returns early and
  // the pool destructor would otherwise wait for it forever).
  struct ReleaseOnExit {
    std::atomic<bool>& flag;
    ~ReleaseOnExit() { flag.store(true); }
  } release_on_exit{release};
  auto block_worker = [&]() {
    release.store(false);
    blocked.store(false);
    pool.enqueue([&]() {
      blocked.store(true);
      while (!release.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
    return wait_until([&]() { return blocked.load(); }, 10000);
  };

  const san4::UpscaleDirtyRect full{
      .has_changes = true, .x = 0, .y = 0, .w = 3200, .h = 2400};
  std::vector<std::uint32_t> frame(3200u * 2400u);
  auto composite_full = [&](const san4::HdPackManager& hd) {
    std::fill(frame.begin(), frame.end(), 0xFF000000u);
    san4::UpscaleDirtyRect d = full;
    hd.composite_overlays(frame, 3200, 2400, 4, *vid, d);
    return std::vector<std::uint32_t>(frame);
  };
  // HD pixels of the CG block from a composited 4x frame.
  auto cg_block = [&](const std::vector<std::uint32_t>& f) {
    std::vector<std::uint32_t> blk;
    for (int y = kCgY * 4; y < (kCgY + kCgH) * 4; ++y) {
      for (int x = kCgX * 4; x < (kCgX + kCgW) * 4; ++x) {
        blk.push_back(f[static_cast<std::size_t>(y) * 3200u + x]);
      }
    }
    return blk;
  };

  // 1) Result applied: the CG keeps its (graded) pixels while the job is
  //    pending (deduplicated across frames), then a composite with no
  //    base-frame changes applies it and reports exactly the CG rect.
  blit_cg();
  set_video_palette_brg48(*vid, scene(1));
  CHECK(block_worker());
  const auto pending = cg_block(composite_full(hd_async));
  CHECK(hd_async.async_jobs_in_flight() == 1u);
  CHECK(cg_block(composite_full(hd_async)) == pending);
  CHECK(hd_async.async_jobs_in_flight() == 1u);
  CHECK(!hd_async.has_pending_hd_changes());
  release.store(true);
  CHECK(wait_until([&]() { return hd_async.async_results_ready() == 1u; },
                   20000));
  CHECK(hd_async.has_pending_hd_changes());
  std::fill(frame.begin(), frame.end(), 0xFF000000u);
  std::vector<san4::UpscaleDirtyRect> out_rects;
  const san4::UpscaleDirtyRect u =
      hd_async.composite_overlays(frame, 3200, 2400, 4, *vid, {}, out_rects);
  CHECK(u.has_changes && u.x == kCgX * 4 && u.y == kCgY * 4 &&
        u.w == kCgW * 4 && u.h == kCgH * 4);
  CHECK(out_rects.size() == 1u);
  CHECK(hd_async.async_jobs_in_flight() == 0u);
  const auto applied = cg_block(frame);
  const auto expect1 = cg_block(composite_full(hd_sync));
  CHECK(applied == expect1);
  CHECK(applied != pending);

  // 2) Stale target, coalesced: the palette moves on before the job runs.
  //    At most one job per CG is in flight, so the second palette change
  //    only retargets the CG. Draining the stale first result caches it and
  //    enqueues the newest target, whose result is applied.
  CHECK(block_worker());
  set_video_palette_brg48(*vid, scene(2));
  composite_full(hd_async);
  set_video_palette_brg48(*vid, scene(3));
  const auto before_drain = cg_block(composite_full(hd_async));
  CHECK(hd_async.async_jobs_in_flight() == 1u);
  release.store(true);
  CHECK(wait_until([&]() { return hd_async.async_results_ready() == 1u; },
                   20000));
  CHECK(cg_block(composite_full(hd_async)) == before_drain);  // not painted
  CHECK(hd_async.async_jobs_in_flight() == 1u);  // job for scene(3)
  CHECK(wait_until([&]() { return hd_async.async_results_ready() == 1u; },
                   20000));
  const auto got3 = cg_block(composite_full(hd_async));
  CHECK(hd_async.async_jobs_in_flight() == 0u);
  CHECK(got3 == cg_block(composite_full(hd_sync)));
  // The superseded scene(2) result was cached: returning to it repaints on
  // the same composite without a new job.
  set_video_palette_brg48(*vid, scene(2));
  const std::uint64_t misses_before = hd_async.cache_misses();
  const auto got2 = cg_block(composite_full(hd_async));
  CHECK(hd_async.async_jobs_in_flight() == 0u);
  CHECK(hd_async.cache_misses() == misses_before);
  CHECK(got2 == cg_block(composite_full(hd_sync)));

  // 3) Stale region: the CG is cleared while its job is pending. The result
  //    is cached but paints nothing.
  CHECK(block_worker());
  set_video_palette_brg48(*vid, scene(4));
  composite_full(hd_async);
  CHECK(hd_async.async_jobs_in_flight() == 1u);
  hd_async.clear_overlays();
  hd_sync.clear_overlays();
  release.store(true);
  CHECK(wait_until([&]() { return hd_async.async_results_ready() == 1u; },
                   20000));
  std::fill(frame.begin(), frame.end(), 0xFF000000u);
  san4::UpscaleDirtyRect d{};
  hd_async.composite_overlays(frame, 3200, 2400, 4, *vid, d);
  CHECK(d.has_changes && d.w == 3200 && d.h == 2400);  // clear_overlays
  CHECK(hd_async.async_jobs_in_flight() == 0u);
  CHECK(hd_async.active_overlay_count() == 0u);
  CHECK(std::all_of(frame.begin(), frame.end(),
                    [](std::uint32_t p) { return p == 0xFF000000u; }));
  blit_cg();
  set_video_palette_brg48(*vid, scene(4));
  const auto got4 = cg_block(composite_full(hd_async));
  CHECK(hd_async.async_jobs_in_flight() == 0u);
  CHECK(got4 == cg_block(composite_full(hd_sync)));

  // 4) Jobs may outlive their manager: results land in the shared queue.
  {
    auto hd_tmp = std::make_unique<san4::HdPackManager>();
    hd_tmp->set_pack_dir(pack_dir);
    hd_tmp->set_async_pool(&pool);
    set_video_palette_brg48(*vid, black);
    hd_tmp->on_planar_blit_source(kCgX, kCgY, kCgW, kCgH, 4, 0u, 0u, 0u, 0u,
                                  clip, cg_raw, *vid);
    set_video_palette_brg48(*vid, scene(5));
    std::fill(frame.begin(), frame.end(), 0xFF000000u);
    san4::UpscaleDirtyRect dd = full;
    hd_tmp->composite_overlays(frame, 3200, 2400, 4, *vid, dd);
    CHECK(hd_tmp->async_jobs_in_flight() == 1u);
    hd_tmp.reset();
  }
  CHECK(block_worker());  // runs after the orphaned job finished
  release.store(true);
  hd_async.set_async_pool(nullptr);
}

// Dynamic-CG re-upscalers substituted through HdPackManagerTestPeer.
void translucent_cg_upscale(const std::uint8_t*, int w, int h,
                            const std::array<std::uint32_t, 16>&,
                            std::vector<std::uint32_t>& out) {
  out.assign(static_cast<std::size_t>(w) * 4u * static_cast<std::size_t>(h) * 4u,
             0x80FF2040u);
}

void throwing_cg_upscale(const std::uint8_t*, int, int,
                         const std::array<std::uint32_t, 16>&,
                         std::vector<std::uint32_t>&) {
  throw std::bad_alloc();
}

std::atomic<bool> g_slow_cg_started{false};

void slow_cg_upscale(const std::uint8_t* slots, int w, int h,
                     const std::array<std::uint32_t, 16>& pal,
                     std::vector<std::uint32_t>& out) {
  g_slow_cg_started.store(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  HdPeer::default_cg_upscale_fn()(slots, w, h, pal, out);
}

void test_hd_async_dynamic_cg_failures_and_lifetime() {
  const DynamicCgPack cg_pack;
  CHECK(cg_pack.ok);
  auto vid = std::make_unique<san4::VesaPlanarVideo>();
  vid->reset_mode(0x0102u);
  const std::array<std::uint8_t, 48> black{};

  std::vector<std::uint32_t> frame(3200u * 2400u);
  auto composite_full = [&](const san4::HdPackManager& hd) {
    std::fill(frame.begin(), frame.end(), 0xFF000000u);
    san4::UpscaleDirtyRect d{
        .has_changes = true, .x = 0, .y = 0, .w = 3200, .h = 2400};
    hd.composite_overlays(frame, 3200, 2400, 4, *vid, d);
    std::vector<std::uint32_t> blk;
    for (int y = DynamicCgPack::kY * 4;
         y < (DynamicCgPack::kY + DynamicCgPack::kH) * 4; ++y) {
      for (int x = DynamicCgPack::kX * 4;
           x < (DynamicCgPack::kX + DynamicCgPack::kW) * 4; ++x) {
        blk.push_back(frame[static_cast<std::size_t>(y) * 3200u + x]);
      }
    }
    return blk;
  };
  san4::HdPackManager hd_sync;
  hd_sync.set_pack_dir(cg_pack.dir);
  auto blit_both = [&](san4::HdPackManager& hd) {
    set_video_palette_brg48(*vid, black);
    cg_pack.blit(hd, *vid);
    cg_pack.blit(hd_sync, *vid);
  };

  // (a) A throwing job is drained as a failure: it paints nothing, clears
  //     the CG's in-flight entry, and a later frame retries.
  {
    san4::ThreadPool pool(1);
    san4::HdPackManager hd;
    hd.set_pack_dir(cg_pack.dir);
    hd.set_async_pool(&pool);
    HdPeer::set_cg_upscale_fn(hd, &throwing_cg_upscale);
    blit_both(hd);
    set_video_palette_brg48(*vid, dynamic_cg_scene(11));
    const auto pending = composite_full(hd);
    CHECK(hd.async_jobs_in_flight() == 1u);
    CHECK(wait_until([&]() { return hd.async_results_ready() == 1u; }, 20000));
    HdPeer::set_cg_upscale_fn(hd, HdPeer::default_cg_upscale_fn());
    CHECK(composite_full(hd) == pending);
    // The failure freed the slot, so the same composite enqueued a retry.
    CHECK(hd.async_jobs_in_flight() == 1u);
    CHECK(wait_until([&]() { return hd.async_results_ready() == 1u; }, 20000));
    const auto applied = composite_full(hd);
    CHECK(hd.async_jobs_in_flight() == 0u);
    CHECK(applied == composite_full(hd_sync));
    hd.set_async_pool(nullptr);
  }

  // (b) Destroying the manager while its job runs, then the pool, is clean:
  //     the job only touches its own copies and the shared result queue.
  {
    san4::ThreadPool pool(1);
    g_slow_cg_started.store(false);
    {
      auto hd = std::make_unique<san4::HdPackManager>();
      hd->set_pack_dir(cg_pack.dir);
      hd->set_async_pool(&pool);
      HdPeer::set_cg_upscale_fn(*hd, &slow_cg_upscale);
      set_video_palette_brg48(*vid, black);
      cg_pack.blit(*hd, *vid);
      set_video_palette_brg48(*vid, dynamic_cg_scene(12));
      composite_full(*hd);
      CHECK(hd->async_jobs_in_flight() == 1u);
      CHECK(wait_until([&]() { return g_slow_cg_started.load(); }, 20000));
    }
  }

  // (c) reload_pack() while a job is in flight: the result is interned
  //     against the new palette table, paints no stale CG, and a re-blitted
  //     CG reuses it with valid palette ids.
  {
    san4::ThreadPool pool(1);
    std::atomic<bool> release{false};
    struct ReleaseOnExit {
      std::atomic<bool>& flag;
      ~ReleaseOnExit() { flag.store(true); }
    } release_on_exit{release};
    std::atomic<bool> blocked{false};
    san4::HdPackManager hd;
    hd.set_pack_dir(cg_pack.dir);
    hd.set_async_pool(&pool);
    blit_both(hd);
    pool.enqueue([&]() {
      blocked.store(true);
      while (!release.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
    CHECK(wait_until([&]() { return blocked.load(); }, 10000));
    set_video_palette_brg48(*vid, dynamic_cg_scene(13));
    composite_full(hd);
    CHECK(hd.async_jobs_in_flight() == 1u);
    hd.reload_pack();
    release.store(true);
    CHECK(wait_until([&]() { return hd.async_results_ready() == 1u; }, 20000));
    composite_full(hd);
    CHECK(hd.async_jobs_in_flight() == 0u);
    blit_both(hd);
    set_video_palette_brg48(*vid, dynamic_cg_scene(13));
    const std::uint64_t misses = hd.cache_misses();
    const auto got = composite_full(hd);
    CHECK(hd.cache_misses() == misses);  // served by the drained result
    CHECK(hd.async_jobs_in_flight() == 0u);
    CHECK(got == composite_full(hd_sync));
    for (int y = DynamicCgPack::kY; y < DynamicCgPack::kY + DynamicCgPack::kH;
         ++y) {
      for (int x = DynamicCgPack::kX;
           x < DynamicCgPack::kX + DynamicCgPack::kW; ++x) {
        CHECK(HdPeer::pal_id_valid_at(hd, x, y));
      }
    }
    hd.set_async_pool(nullptr);
  }
}

// prepare_frame() applies HD repaints before the upscale, so a frame whose
// only change is a finished async CG re-upscale re-upscales the CG's tiles
// and blends its translucent pixels exactly once over fresh base pixels:
// the frame equals a full re-upscale + composite of the same state.
void test_prepare_frame_blends_hd_repaints_once() {
  const DynamicCgPack cg_pack;
  CHECK(cg_pack.ok);
  san4::ThreadPool pool(1);
  san4::X86Machine m;
  m.video().reset_mode(0x0102u);
  m.hd_pack().set_pack_dir(cg_pack.dir);
  m.hd_pack().set_async_pool(&pool);
  HdPeer::set_cg_upscale_fn(m.hd_pack(), &translucent_cg_upscale);
  san4::PixelUpscaler up(1);
  up.set_mode(san4::UpscaleMode::Nearest);
  san4::EmuSession session(m, up);

  const std::array<std::uint8_t, 48> black{};
  set_video_palette_brg48(m.video(), black);
  cg_pack.blit(m.hd_pack(), m.video());
  bool force_full = true;
  session.produce_frame(force_full, nullptr);
  set_video_palette_brg48(m.video(), dynamic_cg_scene(21));
  session.produce_frame(force_full, nullptr);
  CHECK(m.hd_pack().async_jobs_in_flight() == 1u);
  const std::vector<std::uint32_t> before = up.output_argb();
  CHECK(wait_until([&]() { return m.hd_pack().async_results_ready() == 1u; },
                   20000));

  session.produce_frame(force_full, nullptr);
  CHECK(m.hd_pack().async_jobs_in_flight() == 0u);
  const std::vector<std::uint32_t> repainted = up.output_argb();
  std::uint64_t area = 0;
  for (const san4::UpscaleDirtyRect& r : session.frame_rects()) {
    area += static_cast<std::uint64_t>(r.w) * static_cast<std::uint64_t>(r.h);
  }
  CHECK(area >= static_cast<std::uint64_t>(DynamicCgPack::kW) *
                    DynamicCgPack::kH);
  const std::size_t cg_px =
      static_cast<std::size_t>(DynamicCgPack::kY + 3) * 800u +
      DynamicCgPack::kX + 3;
  CHECK(repainted[cg_px] != before[cg_px]);
  const std::uint32_t alpha = repainted[cg_px] >> 24;
  CHECK(alpha == 0xFFu);
  CHECK(repainted[cg_px] != 0xFFFF2040u);  // translucent: base shows through

  force_full = true;
  session.produce_frame(force_full, nullptr);
  CHECK(up.output_argb() == repainted);
  m.hd_pack().set_async_pool(nullptr);
}

std::uint32_t read_ram32(const san4::X86Machine& m, std::uint32_t lin) {
  std::uint32_t v = 0;
  std::memcpy(&v, m.guest_ram() + lin, sizeof(v));
  return v;
}

void test_memio_conv_fast_path_bda_poll_and_vram() {
  san4::X86Machine m;
  m.video().reset_mode(0x0102u);
  m.video().port_out8(0x03C4u, 0x02u);  // Map Mask = all planes
  m.video().port_out8(0x03C5u, 0x0Fu);
  m.video().port_out8(0x03CEu, 0x05u);  // Write Mode 0
  m.video().port_out8(0x03CFu, 0x00u);
  m.dos().clear_keys();

  static constexpr std::uint8_t kCode[] = {
      // 1) 8/16/32-bit conventional RAM writes + read-backs at lin 0x1000
      //    (first byte past page 0), stored again at 0x1010.. via the CPU.
      0xB8, 0x00, 0x01,                                // mov ax, 0x0100
      0x8E, 0xD8,                                      // mov ds, ax
      0xC6, 0x06, 0x00, 0x00, 0x5A,                    // mov byte [0], 0x5A
      0xC7, 0x06, 0x01, 0x00, 0xEF, 0xBE,              // mov word [1], 0xBEEF
      0x66, 0xC7, 0x06, 0x03, 0x00, 0x67, 0x45, 0x23, 0x01,  // mov dword [3]
      0xA0, 0x00, 0x00,                                // mov al, [0]
      0xA2, 0x10, 0x00,                                // mov [0x10], al
      0x8B, 0x1E, 0x01, 0x00,                          // mov bx, [1]
      0x89, 0x1E, 0x12, 0x00,                          // mov [0x12], bx
      0x66, 0x8B, 0x0E, 0x03, 0x00,                    // mov ecx, [3]
      0x66, 0x89, 0x0E, 0x14, 0x00,                    // mov [0x14], ecx
      // 2) Last conventional dword 0x9FFFC..0x9FFFF.
      0xB8, 0x00, 0x90,                                // mov ax, 0x9000
      0x8E, 0xD8,                                      // mov ds, ax
      0x66, 0xC7, 0x06, 0xFC, 0xFF, 0x0D, 0xF0, 0xFE, 0xCA,  // [0xFFFC] = CAFEF00D
      0x66, 0x8B, 0x16, 0xFC, 0xFF,                    // mov edx, [0xFFFC]
      0x66, 0x89, 0x16, 0xF8, 0xFF,                    // mov [0xFFF8], edx
      // 3) rep movsw 0x1000 -> 0x2000 and a 32-bit push/pop on the stack.
      0xB8, 0x00, 0x01, 0x8E, 0xD8,                    // ds = 0x0100
      0xB8, 0x00, 0x02, 0x8E, 0xC0,                    // es = 0x0200
      0x31, 0xF6,                                      // xor si, si
      0x31, 0xFF,                                      // xor di, di
      0xB9, 0x04, 0x00,                                // mov cx, 4
      0xFC,                                            // cld
      0xF3, 0xA5,                                      // rep movsw
      0x66, 0x68, 0x78, 0x56, 0x34, 0x12,              // push dword 0x12345678
      0x66, 0x5E,                                      // pop esi
      0x66, 0x89, 0x36, 0x18, 0x00,                    // mov [0x18], esi
      // 4) Planar VRAM MMIO (8- and 16-bit writes, 8-bit latch read).
      0xB8, 0x00, 0xA0, 0x8E, 0xC0,                    // es = 0xA000
      0x26, 0xC6, 0x06, 0x10, 0x00, 0xFF,              // mov byte es:[0x10], 0xFF
      0x26, 0xC7, 0x06, 0x20, 0x00, 0xFF, 0x00,        // mov word es:[0x20], 0x00FF
      0x26, 0x8A, 0x36, 0x10, 0x00,                    // mov dh, es:[0x10]
      // 5) Direct BDA keyboard poll: spin while tail == head.
      0x31, 0xC0,                                      // xor ax, ax
      0x8E, 0xD8,                                      // mov ds, ax
      0x8B, 0x36, 0x1C, 0x04,                          // poll: mov si, [0x041C]
      0x3B, 0x36, 0x1A, 0x04,                          // cmp si, [0x041A]
      0x74, 0xF6,                                      // je poll
      0xC6, 0x06, 0x00, 0x06, 0x77,                    // mov byte [0x0600], 0x77
      0xEB, 0xFE,                                      // jmp $
  };
  m.load_synthetic_code(0x2000u, 0x0100u, kCode);

  // The burst must end as an idle BDA poll long before the 50 ms budget.
  m.run_frame(1.0 / 60.0, 0.050);
  CHECK(m.last_frame_stats().yield_reason == san4::GuestYieldReason::Idle);
  CHECK(m.last_frame_stats().guest_instrs < 6000u);
  CHECK(m.guest_ram()[0x0600u] == 0u);

  CHECK(m.guest_ram()[0x1000u] == 0x5Au);
  CHECK(m.guest_ram()[0x1001u] == 0xEFu && m.guest_ram()[0x1002u] == 0xBEu);
  CHECK(read_ram32(m, 0x1003u) == 0x01234567u);
  CHECK(m.guest_ram()[0x1010u] == 0x5Au);
  CHECK(m.guest_ram()[0x1012u] == 0xEFu && m.guest_ram()[0x1013u] == 0xBEu);
  CHECK(read_ram32(m, 0x1014u) == 0x01234567u);
  CHECK(read_ram32(m, 0x9FFFCu) == 0xCAFEF00Du);
  CHECK(read_ram32(m, 0x9FFF8u) == 0xCAFEF00Du);
  CHECK(std::memcmp(m.guest_ram() + 0x2000u, m.guest_ram() + 0x1000u, 8u) == 0);
  CHECK(read_ram32(m, 0x1018u) == 0x12345678u);

  CHECK(((m.emu()->x86.R_EDX >> 8) & 0xFFu) == 0xFFu);
  CHECK(m.video().vram_read8(0x10u) == 0xFFu);
  m.video().latch_frame();
  const auto& vram = m.video().latched_vram();
  for (int x = 128; x < 136; ++x) {
    CHECK(vram[static_cast<std::size_t>(x)] == 15u);
  }
  CHECK(vram[256] == 15u && vram[263] == 15u);
  CHECK(vram[264] == 0u && vram[271] == 0u);
  for (std::uint32_t lin = 0xA0000u; lin < 0xA0030u; ++lin) {
    CHECK(m.guest_ram()[lin] == 0u);  // VRAM writes never land in guest RAM
  }

  // A queued key reaches the BDA through the poll and releases the loop.
  m.dos().push_key('A', 0x1Eu);
  m.run_frame(1.0 / 60.0, 0.002);
  CHECK(m.guest_ram()[0x0600u] == 0x77u);
}

void test_fmdrv_turbo_timer_budget() {
  // Re-arms timer F0 for one tick, polls until it expires, counts expiries.
  static constexpr std::uint8_t kTimerLoop[] = {
      0xB8, 0x01, 0xF0,        // 0x00: mov ax, 0xF001 (set timer F0)
      0xBB, 0x01, 0x00,        //       mov bx, 1 (1/60 s)
      0xCD, 0x66,              //       int 0x66
      0xFB,                    // 0x08: sti
      0xB8, 0x00, 0xF0,        //       mov ax, 0xF000 (poll timer F0)
      0xCD, 0x66,              //       int 0x66 -> BX = ticks left
      0x85, 0xDB,              //       test bx, bx
      0x75, 0xF6,              //       jnz 0x08
      0xFF, 0x06, 0x00, 0x06,  //       inc word [0x0600]
      0xEB, 0xE8,              //       jmp 0x00
  };
  constexpr std::uint32_t kCounterLin = (0x2000u << 4) + 0x0600u;
  auto expiries = [&](const san4::X86Machine& m) {
    return static_cast<int>(m.guest_ram()[kCounterLin]) |
           (static_cast<int>(m.guest_ram()[kCounterLin + 1u]) << 8);
  };
  // Per-frame expiry counts for frames 2..frames (frame 1 arms the timer).
  auto run = [&](int base_speed, bool turbo, int frames) {
    san4::X86Machine m;
    m.set_base_speed_multiplier(base_speed);
    m.set_turbo_held(turbo);
    m.load_synthetic_code(0x2000u, 0x0100u, kTimerLoop);
    std::vector<int> deltas;
    int prev = 0;
    for (int f = 0; f < frames; ++f) {
      m.run_frame(1.0 / 60.0, 0.050);
      const int now = expiries(m);
      if (f > 0) {
        deltas.push_back(now - prev);
      }
      prev = now;
      if (m.last_frame_stats().yield_reason != san4::GuestYieldReason::Idle) {
        deltas.push_back(-1);  // the poll loop must always end the burst
      }
    }
    return deltas;
  };

  // 1x: unchanged path, exactly one expiry per frame.
  for (int d : run(1, false, 6)) {
    CHECK(d == 1);
  }
  // 16x turbo: the frame's 16 ticks are observed mid-burst.
  for (int d : run(1, true, 6)) {
    CHECK(d >= 15 && d <= 16);
  }
  // F4 4x shares the budget path.
  for (int d : run(4, false, 6)) {
    CHECK(d >= 3 && d <= 4);
  }

  // Timer advance the guest never polls for still elapses at frame end.
  {
    san4::X86Machine m;
    m.set_turbo_held(true);
    static constexpr std::uint8_t kArmThenSpin[] = {
        0xB8, 0x01, 0xF0,        // mov ax, 0xF001
        0xBB, 0x58, 0x02,        // mov bx, 600 (10 s)
        0xCD, 0x66,              // int 0x66
        0xEB, 0xFE,              // 0x08: jmp $ (patched to fall through)
        0xB8, 0x00, 0xF0,        // 0x0A: mov ax, 0xF000
        0xCD, 0x66,              //       int 0x66
        0x89, 0x1E, 0x02, 0x06,  //       mov [0x0602], bx
        0xEB, 0xFE,              //       jmp $
    };
    m.load_synthetic_code(0x2000u, 0x0100u, kArmThenSpin);
    m.run_frame(1.0 / 60.0, 0.002);
    m.guest_ram()[(0x2000u << 4) + 0x0100u + 0x09u] = 0x00u;  // jmp +0
    m.set_turbo_held(false);
    m.run_frame(0.0, 0.002);
    const std::uint16_t left =
        static_cast<std::uint16_t>(m.guest_ram()[kCounterLin + 2u]) |
        static_cast<std::uint16_t>(m.guest_ram()[kCounterLin + 3u] << 8);
    CHECK(left == 584u || left == 585u);
  }
}

void test_deterministic_run_frame_and_dos_clock() {
  static constexpr std::uint8_t kSpin[] = {0xEB, 0xFE};  // jmp $
  constexpr double kBudget = 0.006;
  const auto budget_instrs = static_cast<std::uint64_t>(
      kBudget * san4::X86Machine::kDeterministicInstrsPerSec);

  {
    san4::X86Machine m;
    CHECK(!m.deterministic());
  }

  // Two machines fed the same dt sequence retire identical instruction
  // counts, bounded by the instruction budget instead of the host clock.
  auto trace = [&]() {
    san4::MachineConfig cfg;
    cfg.deterministic = true;
    san4::X86Machine m(cfg);
    m.load_synthetic_code(0x2000u, 0x0100u, kSpin);
    std::vector<std::uint64_t> instrs;
    static constexpr double kDts[] = {1.0 / 60.0, 1.0 / 30.0, 0.004, 0.050,
                                      1.0 / 60.0, 0.0,        0.020, 1.0 / 60.0};
    for (double dt : kDts) {
      m.run_frame(dt, kBudget);
      instrs.push_back(m.last_frame_stats().guest_instrs);
      if (m.last_frame_stats().yield_reason != san4::GuestYieldReason::Budget) {
        instrs.push_back(0u);
      }
    }
    instrs.push_back(m.emu()->x86.R_TSC);
    return instrs;
  };
  const auto a = trace();
  const auto b = trace();
  CHECK(a == b);
  for (std::size_t i = 0; i + 1u < a.size(); ++i) {
    CHECK(a[i] >= budget_instrs && a[i] < budget_instrs + 3u * 6000u);
  }

  // Half the budget, roughly half the instructions.
  {
    san4::X86Machine m;
    m.set_deterministic(true);
    m.load_synthetic_code(0x2000u, 0x0100u, kSpin);
    m.run_frame(1.0 / 60.0, kBudget / 2.0);
    const std::uint64_t n = m.last_frame_stats().guest_instrs;
    CHECK(n >= budget_instrs / 2u && n < budget_instrs / 2u + 3u * 6000u);
  }

  // INT 21h AH=2Ch derives the time of day from the BIOS tick count:
  // 74565 ticks * 65536 / 1193182 Hz = 4095.51 s = 01:08:15.51.
  {
    san4::X86Machine m;
    m.set_deterministic(true);
    static constexpr std::uint8_t kGetTime[] = {
        0xB4, 0x2C,  // mov ah, 0x2C
        0xCD, 0x21,  // int 0x21
        0xEB, 0xFE,  // jmp $
    };
    const std::uint32_t ticks = 74565u;
    std::memcpy(m.guest_ram() + 0x046Cu, &ticks, sizeof(ticks));
    m.load_synthetic_code(0x2000u, 0x0100u, kGetTime);
    m.step_instructions(3);
    CHECK(m.emu()->x86.R_CX == 0x0108u);  // CH = 1 h, CL = 8 min
    CHECK(m.emu()->x86.R_DX == 0x0F33u);  // DH = 15 s, DL = 51 cs
  }
}

// Writes a minimal GRP600C stand-in (dispatcher signature at CS:0x0018,
// `retf` at CS:0x232F) plus HOST.COM that EXECs it, boots HOST.COM, and
// returns the installed GRP600C code segment (0 on failure).
std::uint16_t boot_fake_grp600c(const std::filesystem::path& dir,
                                san4::X86Machine& m) {
  std::vector<std::uint8_t> tsr_mz(0x20u + 0x2500u, 0u);
  auto wr16 = [&](std::size_t off, std::uint16_t v) {
    tsr_mz[off + 0u] = static_cast<std::uint8_t>(v & 0xFFu);
    tsr_mz[off + 1u] = static_cast<std::uint8_t>(v >> 8);
  };
  const std::size_t total_sz = tsr_mz.size();
  wr16(0x00u, 0x5A4Du);
  wr16(0x02u, static_cast<std::uint16_t>(total_sz % 512u));
  wr16(0x04u, static_cast<std::uint16_t>((total_sz + 511u) / 512u));
  wr16(0x08u, 0x0002u);  // 32-byte header
  wr16(0x0Au, 0x0010u);
  wr16(0x0Cu, 0xFFFFu);
  wr16(0x0Eu, 0x0260u);  // SS
  wr16(0x10u, 0x0100u);  // SP
  wr16(0x14u, 0x0000u);  // IP
  wr16(0x16u, 0x0000u);  // CS
  wr16(0x18u, 0x001Cu);
  static constexpr std::uint8_t kTsrEntry[] = {
      0xBA, 0x00, 0x03,  // mov dx, 0x0300
      0xB8, 0x00, 0x31,  // mov ax, 0x3100
      0xCD, 0x21,        // int 0x21 (stay resident)
  };
  std::memcpy(tsr_mz.data() + 0x20u, kTsrEntry, sizeof(kTsrEntry));
  static constexpr std::uint8_t kSig[] = {0xE9u, 0x14u, 0x23u, 0x90u, 0x90u};
  std::memcpy(tsr_mz.data() + 0x20u + 0x0018u, kSig, sizeof(kSig));
  tsr_mz[0x20u + 0x232Fu] = 0xCBu;
  for (std::uint8_t i = 0; i < 16u; ++i) {
    tsr_mz[0x20u + san4::kGrpSlotMapOff + i] = i;
  }
  std::vector<std::uint8_t> host_com = {
      0xBB, 0x00, 0x01,  // mov bx, 0x0100
      0xB4, 0x4A,        // mov ah, 0x4A (shrink)
      0xCD, 0x21,        // int 0x21
      0xBA, 0x30, 0x01,  // mov dx, 0x0130 (name)
      0xBB, 0x40, 0x01,  // mov bx, 0x0140 (param block)
      0xB8, 0x00, 0x4B,  // mov ax, 0x4B00 (exec)
      0xCD, 0x21,        // int 0x21
      0xF4,              // hlt
  };
  host_com.resize(0x30u, 0u);
  for (char c : std::string_view("GRPTSR.EXE")) {
    host_com.push_back(static_cast<std::uint8_t>(c));
  }
  host_com.resize(0x40u + 14u, 0u);
  for (const auto& [name, bytes] :
       {std::pair{"GRPTSR.EXE", &tsr_mz}, std::pair{"HOST.COM", &host_com}}) {
    std::ofstream f(dir / name, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes->data()),
            static_cast<std::streamsize>(bytes->size()));
  }
  if (!m.boot("HOST.COM")) {
    return 0u;
  }
  m.step_instructions(200);
  return m.dos().grp600c_cs();
}

void test_grp_drawbox_tile_fill_clears_glyph_marks() {
  ScopedTempDir tmp;
  san4::MachineConfig cfg;
  cfg.game_dir = tmp.path();
  cfg.save_dir = tmp.path();
  san4::X86Machine m(cfg);
  m.video().reset_mode(0x0102u);
  const std::uint16_t grp_cs = boot_fake_grp600c(tmp.path(), m);
  CHECK(grp_cs != 0u);
  const auto lo = static_cast<std::uint8_t>(grp_cs & 0xFFu);
  const auto hi = static_cast<std::uint8_t>(grp_cs >> 8);

  static_assert(san4::kGrpDrawBox == 0x0Du && san4::kGrpFloodFill == 0x0Eu);
  static_assert(san4::kGrpCsBoxX0Offset == san4::kGrpCsClipY1Offset + 2u);
  // AX, BX, CX, SI, DI, then `lcall grp_cs:0x0018`.
  auto box_call = [&](std::uint16_t ax, std::uint16_t x0, std::uint16_t y0,
                      std::uint16_t x1, std::uint16_t y1) {
    std::vector<std::uint8_t> c;
    static constexpr std::uint8_t kMovImm16[] = {0xB8, 0xBB, 0xB9, 0xBE, 0xBF};
    const std::uint16_t values[] = {ax, x0, y0, x1, y1};
    for (std::size_t i = 0; i < std::size(values); ++i) {
      c.push_back(kMovImm16[i]);
      c.push_back(static_cast<std::uint8_t>(values[i] & 0xFFu));
      c.push_back(static_cast<std::uint8_t>(values[i] >> 8));
    }
    c.insert(c.end(), {0x9A, 0x18, 0x00, lo, hi});
    return c;
  };
  // mov word es:[off], v
  auto set_cs16 = [](std::uint16_t off, std::uint16_t v) {
    return std::vector<std::uint8_t>{
        0x26, 0xC7, 0x06, static_cast<std::uint8_t>(off & 0xFFu),
        static_cast<std::uint8_t>(off >> 8), static_cast<std::uint8_t>(v & 0xFFu),
        static_cast<std::uint8_t>(v >> 8)};
  };
  std::vector<std::uint8_t> code = {0xB8, lo, hi, 0x8E, 0xC0};  // es = grp_cs
  auto add = [&](const std::vector<std::uint8_t>& part) {
    code.insert(code.end(), part.begin(), part.end());
  };
  add(set_cs16(san4::kGrpCsClipX0Offset, 0u));
  add(set_cs16(san4::kGrpCsClipY0Offset, 0u));
  add(set_cs16(san4::kGrpCsClipX1Offset, 799u));
  add(set_cs16(san4::kGrpCsClipY1Offset, 599u));
  add(box_call(0x0D02u, 100u, 70u, 20u, 40u));     // PSET tile fill, swapped corners
  add(box_call(0x0D02u, 200u, 200u, 300u, 200u));  // zero height: a line
  add(box_call(0x0D01u, 400u, 300u, 500u, 340u));  // solid fill
  add(box_call(0x0E02u, 640u, 96u, 700u, 140u));   // FloodFill, not a box
  add(set_cs16(san4::kGrpCsPlanarRopOffset, 1u));  // OR keeps text visible
  add(box_call(0x0D02u, 600u, 400u, 700u, 440u));
  add(set_cs16(san4::kGrpCsPlanarRopOffset, 0u));
  add(set_cs16(san4::kGrpCsPlaneDisableOffset, 0x02u));  // plane 1 masked
  add(box_call(0x0D02u, 600u, 500u, 700u, 540u));
  add(set_cs16(san4::kGrpCsPlaneDisableOffset, 0u));
  add(set_cs16(san4::kGrpCsClipX1Offset, 399u));
  add(set_cs16(san4::kGrpCsClipY1Offset, 299u));
  add(box_call(0x0D02u, 380u, 280u, 450u, 330u));  // clipped at (399, 299)
  add({0xEB, 0xFE});

  m.video().mark_glyph_rect(0, 0, san4::VesaPlanarVideo::kWidth,
                            san4::VesaPlanarVideo::kHeight);
  m.load_synthetic_code(0x3000u, 0x0000u, code, 0x3000u, 0x0FF0u);
  m.step_instructions(2000);
  CHECK(m.emu()->x86.R_CS == 0x3000u);

  const auto& glyphs = m.video().live_glyph_tiles();
  auto marked = [&](int tx, int ty) {
    return glyphs[static_cast<std::size_t>(ty * san4::VesaPlanarVideo::kTilesX +
                                           tx)] != 0u;
  };
  // Box (20..100, 40..70) covers tiles x 1..6, y 2..4.
  for (int ty = 2; ty <= 4; ++ty) {
    for (int tx = 1; tx <= 6; ++tx) {
      CHECK(!marked(tx, ty));
    }
  }
  CHECK(marked(0, 3) && marked(7, 3) && marked(3, 1) && marked(3, 5));
  CHECK(marked(15, 12));  // line
  CHECK(marked(28, 20));  // solid fill
  CHECK(marked(41, 7));   // flood fill
  CHECK(marked(40, 26));  // OR ROP
  CHECK(marked(40, 32));  // plane-masked
  // Clipped box (380..399, 280..299) covers tiles x 23..24, y 17..18 only.
  CHECK(!marked(23, 17) && !marked(24, 18));
  CHECK(marked(25, 17) && marked(24, 19) && marked(22, 18));
}

void test_render_policy_profile_vsync_and_scale() {
  using san4::RendererPreference;
  using san4::RenderProfile;
  using san4::UpscaleMode;

  RendererPreference pref = RendererPreference::Cpu;
  CHECK(san4::parse_renderer_preference("auto", pref) &&
        pref == RendererPreference::Auto);
  CHECK(san4::parse_renderer_preference("gpu", pref) &&
        pref == RendererPreference::Gpu);
  CHECK(san4::parse_renderer_preference("cpu", pref) &&
        pref == RendererPreference::Cpu);
  CHECK(!san4::parse_renderer_preference("GPU", pref));
  CHECK(!san4::parse_renderer_preference("", pref));
  CHECK(pref == RendererPreference::Cpu);  // unchanged on failure

  CHECK(san4::is_software_gl_renderer("llvmpipe (LLVM 15.0.6, 256 bits)"));
  CHECK(san4::is_software_gl_renderer("Gallium 0.4 on softpipe"));
  CHECK(san4::is_software_gl_renderer("Mesa swrast"));
  CHECK(san4::is_software_gl_renderer("SVGA3D; build: RELEASE; LLVM;"));
  CHECK(san4::is_software_gl_renderer("LLVMPIPE"));
  CHECK(!san4::is_software_gl_renderer("NVIDIA GeForce RTX 3080/PCIe/SSE2"));
  CHECK(!san4::is_software_gl_renderer("Mesa Intel(R) UHD Graphics 620"));
  CHECK(!san4::is_software_gl_renderer("AMD Radeon RX 6800 (radeonsi)"));
  CHECK(!san4::is_software_gl_renderer(""));

  const auto profile = [](RendererPreference p, std::string_view driver,
                          std::string_view gl) {
    return san4::select_render_profile(p, driver, gl);
  };
  CHECK(profile(RendererPreference::Auto, "opengles2", "llvmpipe (LLVM 15)") ==
        RenderProfile::Software);
  CHECK(profile(RendererPreference::Auto, "opengl", "AMD Radeon") ==
        RenderProfile::Hardware);
  CHECK(profile(RendererPreference::Auto, "software", "") ==
        RenderProfile::Software);
  CHECK(profile(RendererPreference::Auto, "vulkan", "") ==
        RenderProfile::Hardware);
  CHECK(profile(RendererPreference::Gpu, "opengles2", "llvmpipe") ==
        RenderProfile::Hardware);
  CHECK(profile(RendererPreference::Gpu, "software", "") ==
        RenderProfile::Hardware);
  CHECK(profile(RendererPreference::Cpu, "opengl", "NVIDIA") ==
        RenderProfile::Software);
  CHECK(std::string_view(san4::render_profile_name(RenderProfile::Hardware)) ==
        "hw");
  CHECK(std::string_view(san4::render_profile_name(RenderProfile::Software)) ==
        "sw");

  CHECK(san4::select_vsync(RenderProfile::Hardware, 60));
  CHECK(san4::select_vsync(RenderProfile::Hardware, 144));
  CHECK(san4::select_vsync(RenderProfile::Hardware, 55));
  CHECK(!san4::select_vsync(RenderProfile::Hardware, 54));
  CHECK(!san4::select_vsync(RenderProfile::Hardware, 30));
  CHECK(!san4::select_vsync(RenderProfile::Hardware, 0));  // unknown
  CHECK(!san4::select_vsync(RenderProfile::Software, 60));
  CHECK(!san4::select_vsync(RenderProfile::Software, 144));

  san4::AutoScaleInputs in;
  in.hd_pack = true;
  in.current = UpscaleMode::Xbr4x;
  in.profile = RenderProfile::Hardware;
  in.drawable_h = 1200;
  CHECK(san4::select_auto_upscale_mode(in) == UpscaleMode::Xbr4x);
  in.drawable_h = 2160;
  CHECK(san4::select_auto_upscale_mode(in) == UpscaleMode::Xbr4x);
  in.profile = RenderProfile::Software;
  in.drawable_h = 1200;
  CHECK(san4::select_auto_upscale_mode(in) == UpscaleMode::Xbr2x);
  in.drawable_h = 1440;
  CHECK(san4::select_auto_upscale_mode(in) == UpscaleMode::Xbr2x);
  in.drawable_h = 1441;
  CHECK(san4::select_auto_upscale_mode(in) == UpscaleMode::Xbr4x);
  in.drawable_h = 1200;
  in.current = UpscaleMode::Scale2x;
  in.explicit_scale = true;
  CHECK(san4::select_auto_upscale_mode(in) == UpscaleMode::Scale2x);
  in.explicit_scale = false;
  in.user_cycled = true;
  CHECK(san4::select_auto_upscale_mode(in) == UpscaleMode::Scale2x);
  in.user_cycled = false;
  in.hd_pack = false;
  in.current = UpscaleMode::Sharp2x;
  CHECK(san4::select_auto_upscale_mode(in) == UpscaleMode::Sharp2x);
}

std::uint64_t argb_checksum(std::span<const std::uint32_t> px) {
  std::uint64_t h = 0xCBF29CE484222325ull;
  for (const std::uint32_t v : px) {
    h = (h ^ v) * 0x100000001B3ull;
  }
  return h;
}

// Copies `rects` of the `w`-wide frame `src` into `dst` (same size), the way
// the host uploads changed tiles into its texture.
void blit_rects(const std::vector<std::uint32_t>& src, int w,
                std::span<const san4::UpscaleDirtyRect> rects,
                std::vector<std::uint32_t>& dst) {
  for (const san4::UpscaleDirtyRect& r : rects) {
    for (int y = r.y; y < r.y + r.h; ++y) {
      const std::size_t off = static_cast<std::size_t>(y) *
                                  static_cast<std::size_t>(w) +
                              static_cast<std::size_t>(r.x);
      std::copy_n(src.begin() + static_cast<std::ptrdiff_t>(off), r.w,
                  dst.begin() + static_cast<std::ptrdiff_t>(off));
    }
  }
}

void test_frame_mailbox_tile_mapping_and_partial_rows() {
  using san4::FrameMailbox;
  using san4::UpscaleDirtyRect;
  using san4::UpscaleMode;

  auto rect_is = [](const UpscaleDirtyRect& r, int x, int y, int w, int h) {
    return r.has_changes && r.x == x && r.y == y && r.w == w && r.h == h;
  };

  struct ScaleCase {
    int s;
    UpscaleMode mode;
  };
  for (const ScaleCase sc : {ScaleCase{1, UpscaleMode::Nearest},
                             ScaleCase{2, UpscaleMode::Sharp2x},
                             ScaleCase{4, UpscaleMode::Xbr4x}}) {
    const int s = sc.s;
    const int b = 16 * s;
    const int w = 800 * s;
    const int h = 600 * s;
    CHECK(san4::PixelUpscaler::scale_factor_for(sc.mode) == s);

    // 800x600: 50 x 37.5 tiles, so the last tile row is half height.
    CHECK(rect_is(FrameMailbox::tile_output_rect(0, s, w, h), 0, 0, b, b));
    CHECK(rect_is(FrameMailbox::tile_output_rect(51, s, w, h), b, b, b, b));
    CHECK(rect_is(FrameMailbox::tile_output_rect(37 * 50, s, w, h), 0, 37 * b,
                  b, 8 * s));
    CHECK(rect_is(FrameMailbox::tile_output_rect(1899, s, w, h), 49 * b,
                  37 * b, b, 8 * s));
    CHECK(!FrameMailbox::tile_output_rect(1900, s, w, h).has_changes);
    CHECK(!FrameMailbox::tile_output_rect(-1, s, w, h).has_changes);
    // 640x480 frames use only the top-left 40 x 30 tiles.
    CHECK(rect_is(FrameMailbox::tile_output_rect(29 * 50 + 39, s, 640 * s,
                                                 480 * s),
                  39 * b, 29 * b, b, b));
    CHECK(!FrameMailbox::tile_output_rect(40, s, 640 * s, 480 * s).has_changes);
    CHECK(!FrameMailbox::tile_output_rect(30 * 50, s, 640 * s, 480 * s)
               .has_changes);

    FrameMailbox mb;
    std::vector<std::uint32_t> frame(static_cast<std::size_t>(w) *
                                     static_cast<std::size_t>(h));
    for (std::size_t i = 0; i < frame.size(); ++i) {
      frame[i] = static_cast<std::uint32_t>(i * 2654435761u);
    }
    san4::HostStatus status;
    status.mode = sc.mode;
    const san4::FrameProfileRow row{};
    std::vector<UpscaleDirtyRect> rects;

    // First publish: every tile is new.
    mb.publish(frame, w, h, sc.mode, {}, status, row);
    const FrameMailbox::Slot* slot = mb.acquire();
    CHECK(slot != nullptr);
    CHECK(slot->gen == 1u && slot->w == w && slot->h == h &&
          slot->mode == sc.mode && slot->scale == s);
    CHECK(slot->argb == frame);
    CHECK(FrameMailbox::changed_rects(*slot, 0u, rects) ==
          static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h));
    CHECK(mb.acquire() == nullptr);

    // Second publish: the bottom-right pixel (partial last tile row) and two
    // pixels straddling tiles 0 and 1.
    frame[frame.size() - 1u] ^= 0x00FFFFFFu;
    frame[static_cast<std::size_t>(b - 1)] ^= 0x00FFFFFFu;
    frame[static_cast<std::size_t>(b)] ^= 0x00FFFFFFu;
    const std::array<UpscaleDirtyRect, 2> dirty{{
        {.has_changes = true, .resized = false, .x = w - 1, .y = h - 1, .w = 1, .h = 1},
        {.has_changes = true, .resized = false, .x = b - 1, .y = 0, .w = 2, .h = 1},
    }};
    mb.publish(frame, w, h, sc.mode, dirty, status, row);
    slot = mb.acquire();
    CHECK(slot != nullptr && slot->gen == 2u);
    // A different slot than the first one: it must have copied every tile.
    CHECK(slot->argb == frame);
    const std::uint64_t covered = FrameMailbox::changed_rects(*slot, 1u, rects);
    CHECK(rects.size() == 2u);
    CHECK(rect_is(rects[0], 0, 0, 2 * b, b));
    CHECK(rect_is(rects[1], 49 * b, 37 * b, b, 8 * s));
    CHECK(covered == static_cast<std::uint64_t>(2 * b * b + b * 8 * s));

    // Third publish without changes: nothing to upload, content intact.
    mb.publish(frame, w, h, sc.mode, {}, status, row);
    slot = mb.acquire();
    CHECK(slot != nullptr && slot->gen == 3u);
    CHECK(FrameMailbox::changed_rects(*slot, 2u, rects) == 0u && rects.empty());
    CHECK(slot->argb == frame);
  }

  // Waiting: times out empty, wakes on notify_waiters, and on a publish from
  // another thread.
  using Clock = std::chrono::steady_clock;
  FrameMailbox mb;
  CHECK(!mb.wait_for_fresh_until(Clock::now() + std::chrono::milliseconds(5)));
  mb.notify_waiters();
  auto t0 = Clock::now();
  CHECK(!mb.wait_for_fresh_until(t0 + std::chrono::seconds(10)));
  CHECK(Clock::now() - t0 < std::chrono::seconds(5));
  std::vector<std::uint32_t> small(800u * 600u, 0xFF000000u);
  std::thread publisher([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mb.publish(small, 800, 600, UpscaleMode::Nearest, {}, san4::HostStatus{},
               san4::FrameProfileRow{});
  });
  t0 = Clock::now();
  const bool woke = mb.wait_for_fresh_until(t0 + std::chrono::seconds(10));
  const auto waited = Clock::now() - t0;
  publisher.join();
  CHECK(woke);
  CHECK(waited < std::chrono::seconds(5));
  CHECK(mb.acquire() != nullptr);
  CHECK(mb.acquire() == nullptr);
}

void test_frame_mailbox_concurrent_skips_and_resize() {
  using Clock = std::chrono::steady_clock;
  using san4::FrameMailbox;
  using san4::UpscaleDirtyRect;
  using san4::UpscaleMode;

  struct Published {
    std::uint64_t checksum = 0;
    int w = 0;
    int h = 0;
  };
  constexpr std::uint64_t kMaxFrames = 1u << 15;
  // Entry `g` is written before publish `g`, so a consumer holding a slot of
  // generation `g` may read it.
  std::vector<Published> published(kMaxFrames + 1u);
  FrameMailbox mb;
  std::atomic<bool> producer_done{false};
  std::uint64_t final_gen = 0;

  // ~1 kHz producer for 3 s of small frames (320x200 at 1x, i.e. 12.5 tile
  // rows), switching to 640x400 at 2x halfway.
  std::thread producer([&] {
    std::uint64_t rng = 0x9E3779B97F4A7C15ull;
    auto next_rand = [&rng](int bound) {
      rng ^= rng << 13;
      rng ^= rng >> 7;
      rng ^= rng << 17;
      return static_cast<int>(rng % static_cast<std::uint64_t>(bound));
    };
    int w = 320;
    int h = 200;
    UpscaleMode mode = UpscaleMode::Nearest;
    std::vector<std::uint32_t> canvas(static_cast<std::size_t>(w * h), 0u);
    std::vector<UpscaleDirtyRect> dirty;
    const auto t_start = Clock::now();
    bool resized = false;
    while (Clock::now() - t_start < std::chrono::seconds(3) &&
           mb.published_gen() < kMaxFrames) {
      const std::uint64_t gen = mb.published_gen() + 1u;
      auto paint = [&](int x0, int y0, int rw, int rh) {
        for (int y = y0; y < y0 + rh; ++y) {
          for (int x = x0; x < x0 + rw; ++x) {
            canvas[static_cast<std::size_t>(y * w + x)] =
                static_cast<std::uint32_t>(gen * 0x9E3779B1u) ^
                static_cast<std::uint32_t>(y * w + x);
          }
        }
        dirty.push_back(UpscaleDirtyRect{
            .has_changes = true, .resized = false, .x = x0, .y = y0, .w = rw, .h = rh});
      };
      dirty.clear();
      if (!resized && Clock::now() - t_start >= std::chrono::milliseconds(1500)) {
        resized = true;
        w = 640;
        h = 400;
        mode = UpscaleMode::Sharp2x;
        canvas.assign(static_cast<std::size_t>(w * h), 0u);
        paint(0, 0, w, h);
      } else {
        const int n = 1 + next_rand(4);
        for (int i = 0; i < n; ++i) {
          const int x0 = next_rand(w);
          const int y0 = next_rand(h);
          paint(x0, y0, 1 + next_rand(std::min(48, w - x0)),
                1 + next_rand(std::min(48, h - y0)));
        }
      }
      published[gen] = Published{argb_checksum(canvas), w, h};
      san4::HostStatus status;
      status.hd_loaded = static_cast<std::size_t>(gen);
      san4::FrameProfileRow row;
      row.guest_instrs = gen;
      mb.publish(canvas, w, h, mode, dirty, status, row);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    final_gen = mb.published_gen();
    producer_done.store(true, std::memory_order_release);
  });

  // 60 Hz consumer: every acquired slot must equal a complete published
  // frame, and a texture fed only changed tiles must too, despite skips.
  std::vector<std::uint32_t> texture;
  int tex_w = 0;
  int tex_h = 0;
  std::uint64_t uploaded_gen = 0;
  std::uint64_t last_gen = 0;
  int acquired = 0;
  int skips = 0;
  int bad_slot = 0;
  int bad_texture = 0;
  int bad_meta = 0;
  bool saw_resize = false;
  std::vector<UpscaleDirtyRect> rects;
  auto consume = [&](const FrameMailbox::Slot& s) {
    ++acquired;
    if (s.gen <= last_gen || s.gen > kMaxFrames) {
      ++bad_meta;
      return;
    }
    if (s.gen > last_gen + 1u) {
      ++skips;
    }
    last_gen = s.gen;
    const Published& p = published[s.gen];
    if (s.w != p.w || s.h != p.h || s.status.hd_loaded != s.gen ||
        s.profile.guest_instrs != s.gen) {
      ++bad_meta;
    }
    if (argb_checksum(s.argb) != p.checksum) {
      ++bad_slot;
    }
    if (s.w != tex_w || s.h != tex_h) {
      saw_resize = saw_resize || tex_w != 0;
      tex_w = s.w;
      tex_h = s.h;
      texture.assign(static_cast<std::size_t>(tex_w * tex_h), 0u);
      uploaded_gen = 0;
    }
    FrameMailbox::changed_rects(s, uploaded_gen, rects);
    blit_rects(s.argb, s.w, rects, texture);
    uploaded_gen = s.gen;
    if (argb_checksum(texture) != p.checksum) {
      ++bad_texture;
    }
  };
  auto next = Clock::now();
  while (!producer_done.load(std::memory_order_acquire)) {
    if (const FrameMailbox::Slot* s = mb.acquire()) {
      consume(*s);
    }
    next += std::chrono::microseconds(16667);
    std::this_thread::sleep_until(next);
  }
  producer.join();
  if (const FrameMailbox::Slot* s = mb.acquire()) {
    consume(*s);
  }

  CHECK(bad_meta == 0);
  CHECK(bad_slot == 0);
  CHECK(bad_texture == 0);
  CHECK(acquired >= 20);
  CHECK(skips > 0);
  CHECK(saw_resize);
  CHECK(last_gen == final_gen);
}

void test_host_command_and_event_queues() {
  using san4::HostCmd;
  using san4::HostCmdType;

  // FIFO across threads: a producer pushes while the consumer drains.
  san4::HostCommandQueue q;
  constexpr int kN = 20000;
  std::thread producer([&] {
    for (int i = 0; i < kN; ++i) {
      q.push(HostCmd::mouse_pos(i, -i));
    }
  });
  std::vector<HostCmd> batch;
  int expect = 0;
  bool ordered = true;
  while (expect < kN) {
    q.drain(batch);
    if (batch.empty()) {
      std::this_thread::yield();
    }
    for (const HostCmd& c : batch) {
      if (c.type != HostCmdType::MousePos || c.a != expect || c.b != -expect) {
        ordered = false;
      }
      ++expect;
    }
  }
  producer.join();
  CHECK(ordered);
  q.drain(batch);
  CHECK(batch.empty());

  // Mixed command kinds keep their order and payloads.
  q.push(HostCmd::push_key('a', 0x1Eu));
  q.push(HostCmd::set_turbo(true));
  q.push(HostCmd::with_path(HostCmdType::SetPackDir, "pack"));
  q.push(HostCmd::set_scale(san4::UpscaleMode::Xbr2x));
  q.push(HostCmd::simple(HostCmdType::Quit));
  q.drain(batch);
  CHECK(batch.size() == 5u);
  CHECK(batch[0].type == HostCmdType::PushKey && batch[0].a == 'a' &&
        batch[0].b == 0x1E);
  CHECK(batch[1].type == HostCmdType::SetTurbo && batch[1].a == 1);
  CHECK(batch[2].type == HostCmdType::SetPackDir && batch[2].path == "pack");
  CHECK(batch[3].type == HostCmdType::SetScale &&
        batch[3].a == static_cast<int>(san4::UpscaleMode::Xbr2x));
  CHECK(batch[4].type == HostCmdType::Quit);

  san4::HostEventQueue events;
  san4::HostEvent warp;
  warp.type = san4::HostEvent::Type::Warp;
  warp.x = 12;
  warp.y = 34;
  warp.active_w = 640;
  warp.active_h = 480;
  san4::HostEvent cursor;
  cursor.type = san4::HostEvent::Type::Cursor;
  cursor.shape.hot_x = 3;
  cursor.shape.argb[17] = 0xFF123456u;
  cursor.visible = true;
  events.push(warp);
  events.push(cursor);
  std::vector<san4::HostEvent> evs;
  events.drain(evs);
  CHECK(evs.size() == 2u);
  CHECK(evs[0].type == san4::HostEvent::Type::Warp && evs[0].x == 12 &&
        evs[0].y == 34 && evs[0].active_w == 640 && evs[0].active_h == 480);
  CHECK(evs[1].type == san4::HostEvent::Type::Cursor &&
        evs[1].shape.hot_x == 3 && evs[1].shape.argb[17] == 0xFF123456u &&
        evs[1].visible);
}

void test_emu_session_command_semantics() {
  using san4::HostCmd;
  using san4::HostCmdType;
  using san4::UpscaleMode;

  san4::X86Machine m;
  san4::PixelUpscaler up(1);
  up.set_mode(UpscaleMode::Nearest);
  san4::EmuSession session(m, up);
  m.dos().clear_keys();

  // Middle-drag panning measures travel from the guest-side position:
  // +50 px -> two right arrows (2 px left over), -40 px -> one up arrow.
  session.apply(HostCmd::mouse_pos(400, 300));
  session.apply(HostCmd::mouse_button(2, true));
  session.apply(HostCmd::mouse_motion(450, 300));
  session.apply(HostCmd::mouse_motion(450, 260));
  session.apply(HostCmd::mouse_button(2, false));
  session.apply(HostCmd::mouse_motion(100, 100));  // no drag: no keys
  session.apply(HostCmd::push_key('a', 0x1Eu));
  CHECK(m.mouse().x() == 100 && m.mouse().y() == 100);

  // Reads four keys with INT 16h AH=00h into DS:0200..0207.
  static constexpr std::uint8_t kReadKeys[] = {
      0xB4, 0x00, 0xCD, 0x16, 0xA3, 0x00, 0x02,  // mov ah,0; int 16h; mov [0200],ax
      0xB4, 0x00, 0xCD, 0x16, 0xA3, 0x02, 0x02,  // ... [0202]
      0xB4, 0x00, 0xCD, 0x16, 0xA3, 0x04, 0x02,  // ... [0204]
      0xB4, 0x00, 0xCD, 0x16, 0xA3, 0x06, 0x02,  // ... [0206]
      0xEB, 0xFE,                                // jmp $
  };
  m.load_synthetic_code(0x2000u, 0x0100u, kReadKeys);
  m.step_instructions(64);
  auto key_at = [&](std::uint32_t off) {
    const std::uint8_t* p = m.guest_ram() + 0x20000u + off;
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
  };
  CHECK(key_at(0x200u) == 0x4D00u);
  CHECK(key_at(0x202u) == 0x4D00u);
  CHECK(key_at(0x204u) == 0x4800u);
  CHECK(key_at(0x206u) == 0x1E61u);
  CHECK(!m.dos().has_keys());

  session.apply(HostCmd::set_shift_flags(0x05u));
  CHECK(m.guest_ram()[0x0417u] == 0x05u);

  CHECK(session.status().speed_multiplier == 1);
  session.apply(HostCmd::set_turbo(true));
  CHECK(m.effective_speed_multiplier() == 16);
  CHECK(session.status().speed_multiplier == 16);
  session.apply(HostCmd::set_turbo(false));
  session.apply(HostCmd::simple(HostCmdType::CycleSpeed));
  CHECK(m.effective_speed_multiplier() == 2);

  // Automatic scale follows the drawable height until F5 picks a mode.
  session.set_auto_scale_policy(san4::EmuSession::AutoScalePolicy{
      .profile = san4::RenderProfile::Software,
      .hd_pack = true,
      .explicit_scale = false,
  });
  up.set_mode(UpscaleMode::Sharp2x);
  session.apply(HostCmd::auto_scale(1200));
  CHECK(up.mode() == UpscaleMode::Xbr2x);
  session.apply(HostCmd::auto_scale(2000));
  CHECK(up.mode() == UpscaleMode::Xbr4x);
  CHECK(!session.user_cycled_scale());
  session.apply(HostCmd::simple(HostCmdType::CycleScale));
  CHECK(up.mode() == UpscaleMode::Nearest);
  CHECK(session.user_cycled_scale());
  session.apply(HostCmd::auto_scale(1200));
  CHECK(up.mode() == UpscaleMode::Nearest);
  session.apply(HostCmd::set_scale(UpscaleMode::Scale2x));
  CHECK(up.mode() == UpscaleMode::Scale2x);
  CHECK(session.status().mode == UpscaleMode::Scale2x);
  // Without an HD pack configured, auto scale keeps the current mode until
  // SetPackDir configures one.
  {
    san4::X86Machine m2;
    san4::PixelUpscaler up2(1);
    up2.set_mode(UpscaleMode::Sharp2x);
    san4::EmuSession s2(m2, up2);
    s2.set_auto_scale_policy(san4::EmuSession::AutoScalePolicy{
        .profile = san4::RenderProfile::Hardware,
        .hd_pack = false,
        .explicit_scale = false,
    });
    s2.apply(HostCmd::auto_scale(1200));
    CHECK(up2.mode() == UpscaleMode::Sharp2x);
    ScopedTempDir pack;
    s2.apply(HostCmd::with_path(HostCmdType::SetPackDir, pack.path()));
    CHECK(m2.hd_pack().pack_dir() == pack.path());
    s2.apply(HostCmd::auto_scale(1200));
    CHECK(up2.mode() == UpscaleMode::Xbr4x);
    s2.apply(HostCmd::simple(HostCmdType::ReloadPack));
    CHECK(m2.hd_pack().loaded_texture_count() == 0u);
  }

  // MarkAllDirty forces the next frame to cover the whole output.
  up.set_mode(UpscaleMode::Nearest);
  bool force_full = true;
  session.produce_frame(force_full, nullptr);
  CHECK(!force_full);
  session.produce_frame(force_full, nullptr);
  CHECK(session.frame_rects().empty());
  session.apply(HostCmd::simple(HostCmdType::MarkAllDirty));
  m.video().latch_frame();
  san4::FrameProfileRow row;
  session.produce_frame(force_full, &row);
  std::uint64_t area = 0;
  for (const san4::UpscaleDirtyRect& r : session.frame_rects()) {
    area += static_cast<std::uint64_t>(r.w) * static_cast<std::uint64_t>(r.h);
  }
  CHECK(area == 800u * 600u);
  CHECK(row.dirty_tiles == 50 * 38);
  CHECK(row.upscale_ms >= 0.0 && row.composite_ms >= 0.0);

  ScopedTempDir dump;
  session.apply(HostCmd::with_path(HostCmdType::ToggleDump, dump.path()));
  CHECK(m.hd_pack().dumping_enabled());
  CHECK(session.status().hd_dumping);
  session.apply(HostCmd::with_path(HostCmdType::ToggleDump, dump.path()));
  CHECK(!m.hd_pack().dumping_enabled());

  const san4::HostStatus st = session.status();
  CHECK(st.active_w == 800 && st.active_h == 600 && !st.exited);
  CHECK(!session.quit_requested());
  session.apply(HostCmd::simple(HostCmdType::Quit));
  CHECK(session.quit_requested());
}

// Sets 800x600 planar mode, then keeps incrementing VRAM bytes all over the
// screen; with `exit_after_loops`, terminates via INT 21h AH=4Ch after that
// many 65535-write passes.
std::vector<std::uint8_t> make_vram_churn_program(std::uint8_t exit_after_loops) {
  std::vector<std::uint8_t> code = {
      0xB8, 0x02, 0x4F,  // mov ax, 0x4F02
      0xBB, 0x02, 0x01,  // mov bx, 0x0102
      0xCD, 0x10,        // int 0x10
      0xB8, 0x00, 0xA0,  // mov ax, 0xA000
      0x8E, 0xC0,        // mov es, ax
      0xBA, 0xC4, 0x03,  // mov dx, 0x03C4
      0xB8, 0x02, 0x0F,  // mov ax, 0x0F02 (map mask: all planes)
      0xEF,              // out dx, ax
      0x31, 0xFF,        // xor di, di
  };
  if (exit_after_loops == 0u) {
    const std::uint8_t loop[] = {
        0xB9, 0xFF, 0xFF,        // outer: mov cx, 0xFFFF
        0x26, 0xFE, 0x05,        // inner: inc byte es:[di]
        0x81, 0xC7, 0xD5, 0x04,  //        add di, 1237
        0xE2, 0xF7,              //        loop inner
        0xEB, 0xF2,              //        jmp outer
    };
    code.insert(code.end(), std::begin(loop), std::end(loop));
  } else {
    const std::uint8_t loop[] = {
        0xBE, exit_after_loops, 0x00,  // mov si, N
        0xB9, 0xFF, 0xFF,              // outer: mov cx, 0xFFFF
        0x26, 0xFE, 0x05,              // inner: inc byte es:[di]
        0x81, 0xC7, 0xD5, 0x04,        //        add di, 1237
        0xE2, 0xF7,                    //        loop inner
        0x4E,                          //        dec si
        0x75, 0xF1,                    //        jnz outer
        0xB8, 0x00, 0x4C,              // mov ax, 0x4C00
        0xCD, 0x21,                    // int 0x21
        0xEB, 0xFE,                    // jmp $
    };
    code.insert(code.end(), std::begin(loop), std::end(loop));
  }
  return code;
}

void test_emu_thread_end_to_end_synthetic() {
  using Clock = std::chrono::steady_clock;
  using san4::FrameMailbox;
  using san4::HostCmd;
  using san4::HostCmdType;

  const san4::EmuSession::LoopOptions opts{
      .guest_budget_sec = 0.004,
      .drain_audio = true,
      .profiling = true,
  };

  // A host-like main loop consumes frames while the emulation thread runs a
  // synthetic guest, receives commands, and shuts down on Quit.
  {
    san4::ThreadPool pool(4);
    san4::X86Machine m;
    san4::PixelUpscaler up(1);
    up.set_thread_pool(&pool);
    up.set_dirty_policy(san4::UpscaleDirtyPolicy::TileRowSpans);
    up.set_mode(san4::UpscaleMode::Nearest);
    m.hd_pack().set_thread_pool(&pool);
    const std::vector<std::uint8_t> code = make_vram_churn_program(0u);
    m.load_synthetic_code(0x2000u, 0x0100u, code);
    m.dos().clear_keys();
    san4::EmuSession session(m, up);
    FrameMailbox mb;
    mb.set_thread_pool(&pool);
    san4::HostCommandQueue cmds;
    san4::EmuThread emu;
    emu.start(session, mb, cmds, opts);
    CHECK(emu.running());

    std::vector<std::uint32_t> texture;
    int tex_w = 0;
    int tex_h = 0;
    std::uint64_t uploaded_gen = 0;
    std::uint64_t last_gen = 0;
    int frames = 0;
    int bad = 0;
    bool profiled = false;
    std::vector<san4::UpscaleDirtyRect> rects;
    auto consume = [&](const FrameMailbox::Slot& s) {
      ++frames;
      if (s.gen <= last_gen || s.w != 800 || s.h != 600 ||
          s.status.active_w != 800 || s.status.active_h != 600 ||
          s.status.exited) {
        ++bad;
      }
      last_gen = s.gen;
      profiled = profiled || s.profile.guest_instrs > 0u;
      if (s.w != tex_w || s.h != tex_h) {
        tex_w = s.w;
        tex_h = s.h;
        texture.assign(static_cast<std::size_t>(tex_w * tex_h), 0u);
        uploaded_gen = 0;
      }
      FrameMailbox::changed_rects(s, uploaded_gen, rects);
      blit_rects(s.argb, s.w, rects, texture);
      uploaded_gen = s.gen;
    };

    const auto t0 = Clock::now();
    bool sent_speed = false;
    bool sent_release = false;
    while (Clock::now() - t0 < std::chrono::milliseconds(1000) &&
           !emu.finished()) {
      const auto elapsed = Clock::now() - t0;
      if (!sent_speed && elapsed > std::chrono::milliseconds(300)) {
        sent_speed = true;
        cmds.push(HostCmd::simple(HostCmdType::CycleSpeed));
        cmds.push(HostCmd::set_turbo(true));
      }
      if (!sent_release && elapsed > std::chrono::milliseconds(600)) {
        sent_release = true;
        cmds.push(HostCmd::set_turbo(false));
        cmds.push(HostCmd::push_key('x', 0x2Du));
      }
      if (const FrameMailbox::Slot* s = mb.acquire()) {
        consume(*s);
      }
      mb.wait_for_fresh_until(Clock::now() + std::chrono::milliseconds(8));
    }
    CHECK(!emu.finished());
    emu.stop();
    CHECK(!emu.running());
    CHECK(emu.finished());
    // The frame published last is the final machine state.
    if (const FrameMailbox::Slot* s = mb.acquire()) {
      consume(*s);
    }
    CHECK(bad == 0);
    CHECK(frames >= 5);
    CHECK(profiled);
    CHECK(texture == up.output_argb());
    CHECK(m.base_speed_multiplier() == 2);
    CHECK(m.effective_speed_multiplier() == 2);
    CHECK(m.dos().has_keys());
  }

  // The guest exiting ends the loop on its own, after publishing the frame
  // that observed the exit.
  {
    san4::X86Machine m;
    san4::PixelUpscaler up(1);
    up.set_mode(san4::UpscaleMode::Nearest);
    const std::vector<std::uint8_t> code = make_vram_churn_program(8u);
    m.load_synthetic_code(0x2000u, 0x0100u, code);
    san4::EmuSession session(m, up);
    FrameMailbox mb;
    san4::HostCommandQueue cmds;
    san4::EmuThread emu;
    emu.start(session, mb, cmds, opts);
    bool saw_exit = false;
    const auto deadline = Clock::now() + std::chrono::seconds(60);
    while (!emu.finished() && Clock::now() < deadline) {
      mb.wait_for_fresh_until(Clock::now() + std::chrono::milliseconds(50));
      if (const FrameMailbox::Slot* s = mb.acquire()) {
        saw_exit = saw_exit || s->status.exited;
      }
    }
    CHECK(emu.finished());
    if (const FrameMailbox::Slot* s = mb.acquire()) {
      saw_exit = saw_exit || s->status.exited;
    }
    emu.stop();
    CHECK(saw_exit);
    CHECK(m.exited());
  }

  // Destroying a running EmuThread sends Quit and joins.
  {
    san4::X86Machine m;
    san4::PixelUpscaler up(1);
    up.set_mode(san4::UpscaleMode::Nearest);
    const std::vector<std::uint8_t> code = make_vram_churn_program(0u);
    m.load_synthetic_code(0x2000u, 0x0100u, code);
    san4::EmuSession session(m, up);
    FrameMailbox mb;
    san4::HostCommandQueue cmds;
    {
      san4::EmuThread emu;
      emu.start(session, mb, cmds, opts);
      mb.wait_for_fresh_until(Clock::now() + std::chrono::seconds(10));
    }
    CHECK(session.quit_requested());
    CHECK(mb.acquire() != nullptr);
  }
}

// IN/OUT on ports in [0x1000, 0xFFFF] fall inside the numeric window of the
// conventional-RAM fast path, but must still reach port_in8 / port_out8 (the
// op-type guard), never libx86emu's default memory/port handler.
void test_memio_port_io_not_fast_pathed() {
  san4::X86Machine m;
  static constexpr std::uint8_t kCode[] = {
      0xBA, 0x00, 0x50,  // mov dx, 0x5000
      0xB0, 0x5A,        // mov al, 0x5A
      0xEE,              // out dx, al
      0xEC,              // in al, dx
      0xA2, 0x00, 0x06,  // mov [0x0600], al
      0xBA, 0x00, 0x90,  // mov dx, 0x9000
      0xB8, 0x34, 0x12,  // mov ax, 0x1234
      0xEF,              // out dx, ax
      0xED,              // in ax, dx
      0xA3, 0x02, 0x06,  // mov [0x0602], ax
      0xBA, 0x20, 0x00,  // mov dx, 0x0020
      0xB8, 0x00, 0xA5,  // mov ax, 0xA500 (port 0x21 = PIC IMR <- 0xA5)
      0xEF,              // out dx, ax
      0xE4, 0x21,        // in al, 0x21
      0xA2, 0x04, 0x06,  // mov [0x0604], al
      0xEB, 0xFE,        // jmp $
  };
  m.guest_ram()[0x5000u] = 0x11u;
  m.guest_ram()[0x9000u] = 0x22u;
  m.guest_ram()[0x9001u] = 0x33u;
  m.load_synthetic_code(0x2000u, 0x0100u, kCode);
  const x86emu_t* emu = m.emu();
  CHECK(emu->io.stats_i != nullptr && emu->io.stats_o != nullptr);
  const unsigned in0 = emu->io.stats_i[0x5000u] + emu->io.stats_i[0x9000u];
  const unsigned out0 = emu->io.stats_o[0x5000u] + emu->io.stats_o[0x9000u];
  m.step_instructions(64);
  constexpr std::uint32_t kData = 0x2000u << 4;
  CHECK(m.guest_ram()[kData + 0x0600u] == 0xFFu);  // unhandled port: 0xFF
  CHECK(m.guest_ram()[kData + 0x0602u] == 0xFFu &&
        m.guest_ram()[kData + 0x0603u] == 0xFFu);
  CHECK(m.guest_ram()[kData + 0x0604u] == 0xA5u);  // 16-bit OUT split
  // Neither guest RAM at the port numbers nor libx86emu's port handler saw
  // the accesses.
  CHECK(m.guest_ram()[0x5000u] == 0x11u);
  CHECK(m.guest_ram()[0x9000u] == 0x22u && m.guest_ram()[0x9001u] == 0x33u);
  CHECK(emu->io.stats_i[0x5000u] + emu->io.stats_i[0x9000u] == in0);
  CHECK(emu->io.stats_o[0x5000u] + emu->io.stats_o[0x9000u] == out0);
}

// Turbo (16x): the guest re-arms timer F0 after its first expiry consumed
// part of the frame's budget. The re-armed timer draws only on what is left,
// and the frame-end subtraction does not advance it again, so the total
// advance per frame never exceeds the scaled frame time.
void test_fmdrv_turbo_timer_rearm_mid_burst() {
  static constexpr std::uint8_t kCode[] = {
      0xB8, 0x01, 0xF0,        // 0x00: mov ax, 0xF001 (set timer F0)
      0xBB, 0x03, 0x00,        //       mov bx, 3
      0xCD, 0x66,              //       int 0x66
      0xB8, 0x00, 0xF0,        // 0x08: mov ax, 0xF000 (poll)
      0xCD, 0x66,              //       int 0x66
      0x85, 0xDB,              //       test bx, bx
      0x75, 0xF7,              //       jnz 0x08
      0xB8, 0x01, 0xF0,        // 0x11: mov ax, 0xF001
      0xBB, 0x64, 0x00,        //       mov bx, 100
      0xCD, 0x66,              //       int 0x66
      0xB8, 0x00, 0xF0,        //       mov ax, 0xF000
      0xCD, 0x66,              //       int 0x66
      0x89, 0x1E, 0x00, 0x06,  //       mov [0x0600], bx
      0xEB, 0xFE,              // 0x22: jmp $ (patched to fall through)
      0xB8, 0x00, 0xF0,        // 0x24: mov ax, 0xF000
      0xCD, 0x66,              //       int 0x66
      0x89, 0x1E, 0x02, 0x06,  //       mov [0x0602], bx
      0xEB, 0xFE,              //       jmp $
  };
  constexpr std::uint32_t kData = 0x2000u << 4;
  auto read16 = [](const san4::X86Machine& m, std::uint32_t lin) {
    return static_cast<int>(m.guest_ram()[lin]) |
           (static_cast<int>(m.guest_ram()[lin + 1u]) << 8);
  };
  san4::X86Machine m;
  m.set_turbo_held(true);
  m.load_synthetic_code(0x2000u, 0x0100u, kCode);
  m.run_frame(1.0 / 60.0, 0.002);
  // 16 ticks of budget: 3 for the first timer, the remaining 13 for the
  // re-armed one (100 -> 87; 88 allows for rounding at the tick boundary).
  const int after_rearm = read16(m, kData + 0x0600u);
  CHECK(after_rearm == 87 || after_rearm == 88);
  m.guest_ram()[kData + 0x0100u + 0x23u] = 0x00u;  // jmp +0
  m.set_turbo_held(false);
  m.run_frame(0.0, 0.002);
  // The frame end did not subtract the consumed budget a second time.
  CHECK(read16(m, kData + 0x0602u) == after_rearm);
}

void test_render_policy_edge_cases_and_scale_pinning() {
  using san4::RenderProfile;
  using san4::UpscaleMode;
  san4::AutoScaleInputs in;
  in.hd_pack = true;
  in.profile = RenderProfile::Software;
  in.current = UpscaleMode::Sharp2x;
  // Unknown drawable size (minimized window): keep the current mode.
  for (int h : {0, -1, -600}) {
    in.drawable_h = h;
    CHECK(san4::select_auto_upscale_mode(in) == UpscaleMode::Sharp2x);
  }
  in.profile = RenderProfile::Hardware;
  in.drawable_h = 0;
  CHECK(san4::select_auto_upscale_mode(in) == UpscaleMode::Sharp2x);
  in.drawable_h = 1;
  CHECK(san4::select_auto_upscale_mode(in) == UpscaleMode::Xbr4x);

  // Headless runs force the dummy video driver unless the user chose one.
  CHECK(san4::use_dummy_video_driver(true, nullptr));
  CHECK(san4::use_dummy_video_driver(true, ""));
  CHECK(!san4::use_dummy_video_driver(true, "x11"));
  CHECK(!san4::use_dummy_video_driver(true, "dummy"));
  CHECK(!san4::use_dummy_video_driver(false, nullptr));
  CHECK(!san4::use_dummy_video_driver(false, "wayland"));

  // F5 and the texture-failure fallback both pin the scale against later
  // AutoScale commands.
  for (const bool via_fallback : {false, true}) {
    san4::X86Machine m;
    san4::PixelUpscaler up(1);
    up.set_mode(UpscaleMode::Sharp2x);
    san4::EmuSession session(m, up);
    session.set_auto_scale_policy(san4::EmuSession::AutoScalePolicy{
        .profile = RenderProfile::Software,
        .hd_pack = true,
        .explicit_scale = false,
    });
    session.apply(san4::HostCmd::auto_scale(1200));
    CHECK(up.mode() == UpscaleMode::Xbr2x);
    session.apply(san4::HostCmd::auto_scale(0));
    CHECK(up.mode() == UpscaleMode::Xbr2x);
    if (via_fallback) {
      session.apply(san4::HostCmd::set_scale(UpscaleMode::Nearest));
    } else {
      session.apply(san4::HostCmd::simple(san4::HostCmdType::CycleScale));
    }
    const UpscaleMode pinned = up.mode();
    CHECK(session.user_cycled_scale());
    session.apply(san4::HostCmd::auto_scale(2000));
    session.apply(san4::HostCmd::auto_scale(1200));
    CHECK(up.mode() == pinned);
  }
}

void test_frame_mailbox_mode_aba_and_vertical_merge() {
  using san4::FrameMailbox;
  using san4::UpscaleDirtyRect;
  using san4::UpscaleMode;
  constexpr int kW = 1600;
  constexpr int kH = 1200;
  const san4::FrameProfileRow row{};
  auto frame_of = [](std::uint32_t salt) {
    std::vector<std::uint32_t> f(static_cast<std::size_t>(kW) * kH);
    for (std::size_t i = 0; i < f.size(); ++i) {
      f[i] = static_cast<std::uint32_t>(i * 2654435761u) ^ salt;
    }
    return f;
  };

  // Same-size mode change A -> B -> A with the B frame never acquired: the
  // second A frame differs everywhere but reports only a small dirty rect.
  // The mode changes still stamp every tile, so a texture fed only by
  // changed_rects (no texture recreation: A == A) ends up equal.
  {
    FrameMailbox mb;
    std::vector<std::uint32_t> texture(static_cast<std::size_t>(kW) * kH, 0u);
    std::vector<UpscaleDirtyRect> rects;
    const auto a1 = frame_of(0x1u);
    mb.publish(a1, kW, kH, UpscaleMode::Sharp2x, {}, san4::HostStatus{}, row);
    const FrameMailbox::Slot* slot = mb.acquire();
    CHECK(slot != nullptr && slot->mode == UpscaleMode::Sharp2x);
    FrameMailbox::changed_rects(*slot, 0u, rects);
    blit_rects(slot->argb, kW, rects, texture);
    std::uint64_t uploaded = slot->gen;
    CHECK(texture == a1);

    const auto b = frame_of(0x2u);
    mb.publish(b, kW, kH, UpscaleMode::Xbr2x, {}, san4::HostStatus{}, row);
    const auto a2 = frame_of(0x3u);
    const std::array<UpscaleDirtyRect, 1> small{{
        {.has_changes = true, .resized = false, .x = 5, .y = 5, .w = 3, .h = 3},
    }};
    mb.publish(a2, kW, kH, UpscaleMode::Sharp2x, small, san4::HostStatus{},
               row);
    slot = mb.acquire();
    CHECK(slot != nullptr && slot->gen == 3u &&
          slot->mode == UpscaleMode::Sharp2x);
    CHECK(slot->argb == a2);
    FrameMailbox::changed_rects(*slot, uploaded, rects);
    blit_rects(slot->argb, kW, rects, texture);
    uploaded = slot->gen;
    CHECK(texture == a2);
  }

  // changed_rects merges equal x-spans of consecutive tile rows into one
  // rect: a full-width band, a two-column strip next to a one-tile notch.
  {
    FrameMailbox mb;
    auto f = frame_of(0x10u);
    mb.publish(f, kW, kH, UpscaleMode::Sharp2x, {}, san4::HostStatus{}, row);
    CHECK(mb.acquire() != nullptr);
    const int b = 32;  // tile block at scale 2
    const std::array<UpscaleDirtyRect, 3> dirty{{
        // Tile rows 2..5, full width.
        {.has_changes = true, .resized = false, .x = 0, .y = 2 * b, .w = kW,
         .h = 4 * b},
        // Tile columns 3..4 of tile rows 10..13.
        {.has_changes = true, .resized = false, .x = 3 * b, .y = 10 * b,
         .w = 2 * b, .h = 4 * b},
        // Tile column 7 of tile row 12 only.
        {.has_changes = true, .resized = false, .x = 7 * b, .y = 12 * b,
         .w = 1, .h = 1},
    }};
    mb.publish(f, kW, kH, UpscaleMode::Sharp2x, dirty, san4::HostStatus{}, row);
    const FrameMailbox::Slot* slot = mb.acquire();
    CHECK(slot != nullptr);
    std::vector<UpscaleDirtyRect> rects;
    const std::uint64_t covered =
        FrameMailbox::changed_rects(*slot, slot->gen - 1u, rects);
    auto has = [&](int x, int y, int w, int h) {
      return std::any_of(rects.begin(), rects.end(),
                         [&](const UpscaleDirtyRect& r) {
                           return r.has_changes && r.x == x && r.y == y &&
                                  r.w == w && r.h == h;
                         });
    };
    CHECK(rects.size() == 3u);
    CHECK(has(0, 2 * b, kW, 4 * b));
    CHECK(has(3 * b, 10 * b, 2 * b, 4 * b));
    CHECK(has(7 * b, 12 * b, b, b));
    CHECK(covered == static_cast<std::uint64_t>(kW) * 4u * b +
                         static_cast<std::uint64_t>(2 * b) * 4u * b +
                         static_cast<std::uint64_t>(b) * b);
    // A full-frame change (first acquire) is a single rect, including the
    // half-height last tile row.
    CHECK(FrameMailbox::changed_rects(*slot, 0u, rects) ==
          static_cast<std::uint64_t>(kW) * kH);
    CHECK(rects.size() == 1u && has(0, 0, kW, kH));
  }
}

void test_present_tracker_decisions() {
  using san4::PresentTracker;
  using san4::UpscaleMode;
  // Texture (re)creation: missing texture, size or mode mismatch.
  CHECK(PresentTracker::needs_new_texture(false, 1600, 1200,
                                          UpscaleMode::Sharp2x, 1600, 1200,
                                          UpscaleMode::Sharp2x));
  CHECK(!PresentTracker::needs_new_texture(true, 1600, 1200,
                                           UpscaleMode::Sharp2x, 1600, 1200,
                                           UpscaleMode::Sharp2x));
  CHECK(PresentTracker::needs_new_texture(true, 1600, 1200,
                                          UpscaleMode::Sharp2x, 3200, 2400,
                                          UpscaleMode::Xbr4x));
  CHECK(PresentTracker::needs_new_texture(true, 1600, 1200,
                                          UpscaleMode::Sharp2x, 1600, 1200,
                                          UpscaleMode::Xbr2x));

  PresentTracker t;
  // Start-up: nothing uploaded, a redraw pending.
  CHECK(t.uploaded_gen() == 0u && t.needs_redraw());
  CHECK(t.finish_upload(1u, 100u));
  t.presented();
  CHECK(!t.needs_redraw() && t.uploaded_gen() == 1u);
  // No change and no pending redraw: skip the present.
  CHECK(!t.finish_upload(2u, 0u));
  CHECK(t.uploaded_gen() == 2u);
  // A pending redraw (expose, resize) presents without new pixels.
  t.request_redraw();
  CHECK(t.finish_upload(3u, 0u));
  t.presented();
  CHECK(!t.finish_upload(4u, 0u));
  // A replaced or reset texture invalidates every upload and redraws.
  t.texture_replaced();
  CHECK(t.uploaded_gen() == 0u && t.needs_redraw());
  CHECK(t.finish_upload(5u, 0u));
  t.presented();

  // A failed texture for a non-Nearest mode requests the Nearest fallback
  // once, however many slots keep failing until it is applied.
  CHECK(!t.take_scale_fallback(UpscaleMode::Nearest));
  CHECK(t.take_scale_fallback(UpscaleMode::Xbr4x));
  CHECK(!t.take_scale_fallback(UpscaleMode::Xbr4x));
  CHECK(!t.take_scale_fallback(UpscaleMode::Xbr2x));
}

void test_emu_thread_exception_and_stop() {
  using Clock = std::chrono::steady_clock;
  // A throwing loop body: finished() turns true, a consumer blocked in
  // wait_for_fresh_until wakes, stop() rethrows once, a second stop() is a
  // no-op.
  {
    san4::FrameMailbox mb;
    san4::EmuThread emu;
    std::atomic<bool> go{false};
    emu.start(
        [&](const std::atomic<bool>&) {
          while (!go.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
          throw std::runtime_error("emu boom");
        },
        mb);
    CHECK(emu.running());
    CHECK(!emu.finished());
    std::thread trigger([&]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      go.store(true);
    });
    const auto t0 = Clock::now();
    const bool fresh = mb.wait_for_fresh_until(t0 + std::chrono::seconds(10));
    const auto waited = Clock::now() - t0;
    trigger.join();
    CHECK(!fresh);
    CHECK(waited < std::chrono::seconds(5));
    CHECK(wait_until([&]() { return emu.finished(); }, 1000));
    std::string what;
    try {
      emu.stop();
    } catch (const std::runtime_error& ex) {
      what = ex.what();
    }
    CHECK(what == "emu boom");
    CHECK(!emu.running());
    bool second_threw = false;
    try {
      emu.stop();
    } catch (...) {
      second_threw = true;
    }
    CHECK(!second_threw);
  }
  // shutdown() reaches a generic body through the stop flag.
  {
    san4::FrameMailbox mb;
    san4::EmuThread emu;
    std::atomic<bool> saw_stop{false};
    emu.start(
        [&](const std::atomic<bool>& stop) {
          while (!stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
          saw_stop.store(true);
        },
        mb);
    emu.shutdown();
    CHECK(saw_stop.load());
    CHECK(emu.finished() && !emu.running());
    emu.stop();
  }
}


namespace san4 {

// Test-only access to SdlHost internals (declared a friend in sdl_host.h).
struct SdlHostTestPeer {
  static bool init_sdl(SdlHost& host) { return host.init_sdl(); }
  static void process_events(SdlHost& host, bool& running) {
    host.process_sdl_events(running);
  }
  static X86Machine& machine(SdlHost& host) { return host.machine_; }
  static PresentTracker& present(SdlHost& host) { return host.present_; }
  static bool has_texture(const SdlHost& host) {
    return host.texture_ != nullptr;
  }
  static SdlHost::Viewport clamp_initial_window_size(int win_w, int win_h,
                                                     int usable_w,
                                                     int usable_h) {
    return SdlHost::clamp_initial_window_size(win_w, win_h, usable_w, usable_h);
  }
  static void map_window_to_dos(int wx, int wy, int ww, int wh, int rw, int rh,
                                int active_w, int active_h, int& out_dx,
                                int& out_dy) {
    SdlHost::map_window_to_dos(wx, wy, ww, wh, rw, rh, active_w, active_h,
                               out_dx, out_dy);
  }
  static void map_dos_to_window(int dx, int dy, int ww, int wh, int rw, int rh,
                                int active_w, int active_h, int& out_wx,
                                int& out_wy) {
    SdlHost::map_dos_to_window(dx, dy, ww, wh, rw, rh, active_w, active_h,
                               out_wx, out_wy);
  }
  static void clear_tex_dims(SdlHost& host) {
    host.tex_w_ = 0;
    host.tex_h_ = 0;
  }
  static void handle_render_reset(SdlHost& host, bool device_reset) {
    host.handle_render_reset(device_reset);
  }
  static void open_audio_device(SdlHost& host, bool verbose_log) {
    host.open_audio_device(verbose_log);
  }
  static void start_emu_scale_churn(SdlHost& host) {
    host.emu_thread_.start(
        [&host](const std::atomic<bool>& stop) {
          static constexpr UpscaleMode kModes[] = {
              UpscaleMode::Nearest, UpscaleMode::Sharp2x, UpscaleMode::Xbr2x};
          int idx = 0;
          while (!stop.load(std::memory_order_acquire)) {
            host.upscaler_.set_mode(kModes[idx % 3]);
            ++idx;
            std::this_thread::yield();
          }
        },
        host.mailbox_);
  }
  static void stop_emu(SdlHost& host) { host.emu_thread_.stop(); }
  static void shutdown_sdl(SdlHost& host) { host.shutdown_sdl(); }
  static void audio_callback(void* userdata, std::uint8_t* stream, int len) {
    SdlHost::sdl_audio_callback(userdata, stream, len);
  }
  static std::uint64_t upload_rects(SdlHost& host, const std::uint32_t* argb,
                                    std::span<const UpscaleDirtyRect> rects) {
    return host.upload_rects(argb, rects);
  }
  static bool present_slot(SdlHost& host, const FrameMailbox::Slot& slot) {
    return host.present_slot(slot);
  }
  static void drain_host_events(SdlHost& host) { host.drain_host_events(); }
  static void window_to_dos_coords(const SdlHost& host, int wx, int wy,
                                   int& out_dx, int& out_dy) {
    host.window_to_dos_coords(wx, wy, out_dx, out_dy);
  }
  static void dos_to_window_coords(const SdlHost& host, int dx, int dy,
                                   int active_w, int active_h, int& out_wx,
                                   int& out_wy) {
    host.dos_to_window_coords(dx, dy, active_w, active_h, out_wx, out_wy);
  }
  static bool execute_headless_command(SdlHost& host,
                                       const std::vector<std::string>& parts,
                                       bool& running, bool& force_full) {
    return host.execute_headless_command(parts, running, force_full);
  }
  static bool recreate_renderer_after_reset(SdlHost& host) {
    return host.recreate_renderer_after_reset();
  }
  static EmuSession& session(SdlHost& host) { return host.session_; }
  static SDL_AudioDeviceID audio_dev(const SdlHost& host) {
    return host.audio_dev_;
  }
};

}  // namespace san4


// Headless runs use the dummy video driver (even with a display present) and
// ignore host input and SDL_QUIT: only the script drives the guest.
void test_sdl_host_headless_isolation() {
  const char* prev_env = std::getenv("SDL_VIDEODRIVER");
  const std::string saved = prev_env != nullptr ? prev_env : "";
#ifdef _WIN32
  _putenv_s("SDL_VIDEODRIVER", "");
#else
  unsetenv("SDL_VIDEODRIVER");
#endif
  struct RestoreEnv {
    const char* prev;
    const std::string& value;
    ~RestoreEnv() {
      if (prev != nullptr) {
#ifdef _WIN32
        _putenv_s("SDL_VIDEODRIVER", value.c_str());
#else
        setenv("SDL_VIDEODRIVER", value.c_str(), 1);
#endif
      }
    }
  } restore{prev_env, saved};

  san4::HostOptions opts;
  opts.headless_script = "quit";
  opts.deterministic = true;
  opts.threads = 2;
  opts.upscale_mode = san4::UpscaleMode::Nearest;
  san4::SdlHost host(opts);
  CHECK(san4::SdlHostTestPeer::init_sdl(host));
  CHECK(std::string_view(SDL_GetCurrentVideoDriver()) == "dummy");
  const char* no_sig = SDL_GetHint(SDL_HINT_NO_SIGNAL_HANDLERS);
  CHECK(no_sig != nullptr && std::string_view(no_sig) == "1");

  san4::X86Machine& m = san4::SdlHostTestPeer::machine(host);
  m.dos().clear_keys();
  m.mouse().set_position(123, 45);
  bool running = true;
  // Window creation queued window events; they only request a redraw.
  san4::SdlHostTestPeer::process_events(host, running);
  CHECK(running);
  san4::PresentTracker& present = san4::SdlHostTestPeer::present(host);
  present.finish_upload(7u, 1u);
  present.presented();

  SDL_Event ev{};
  ev.type = SDL_MOUSEMOTION;
  ev.motion.x = 700;
  ev.motion.y = 500;
  CHECK(SDL_PushEvent(&ev) == 1);
  ev = SDL_Event{};
  ev.type = SDL_MOUSEBUTTONDOWN;
  ev.button.button = SDL_BUTTON_LEFT;
  ev.button.x = 10;
  ev.button.y = 10;
  CHECK(SDL_PushEvent(&ev) == 1);
  ev = SDL_Event{};
  ev.type = SDL_KEYDOWN;
  ev.key.keysym.sym = SDLK_RETURN;
  CHECK(SDL_PushEvent(&ev) == 1);
  ev = SDL_Event{};
  ev.type = SDL_QUIT;
  CHECK(SDL_PushEvent(&ev) == 1);
  san4::SdlHostTestPeer::process_events(host, running);
  CHECK(running);
  CHECK(m.mouse().x() == 123 && m.mouse().y() == 45);
  CHECK(!m.dos().has_keys());
  CHECK(!present.needs_redraw() && present.uploaded_gen() == 7u);

  // A render-target reset recreates the texture and forces a full upload.
  ev = SDL_Event{};
  ev.type = SDL_RENDER_TARGETS_RESET;
  CHECK(SDL_PushEvent(&ev) == 1);
  san4::SdlHostTestPeer::process_events(host, running);
  CHECK(running);
  CHECK(san4::SdlHostTestPeer::has_texture(host));
  CHECK(present.uploaded_gen() == 0u && present.needs_redraw());

  // Window events still request a redraw.
  present.presented();
  ev = SDL_Event{};
  ev.type = SDL_WINDOWEVENT;
  ev.window.event = SDL_WINDOWEVENT_EXPOSED;
  CHECK(SDL_PushEvent(&ev) == 1);
  san4::SdlHostTestPeer::process_events(host, running);
  CHECK(running && present.needs_redraw());

  // [REVISE-1] While emu_thread_ is running and actively mutating upscaler_,
  // handle_render_reset(device_reset=true) with tex_w_=0/tex_h_=0 must never
  // race on upscaler_ (TSan gate).
  san4::SdlHostTestPeer::start_emu_scale_churn(host);
  for (int i = 0; i < 8; ++i) {
    san4::SdlHostTestPeer::clear_tex_dims(host);
    san4::SdlHostTestPeer::handle_render_reset(host, true);
  }
  san4::SdlHostTestPeer::stop_emu(host);

  // [REVISE-2] open_audio_device flushes synchronous enumeration
  // SDL_AUDIODEVICEADDED events so startup does not redundantly re-open audio.
  const char* prev_aud = std::getenv("SDL_AUDIODRIVER");
  const std::string saved_aud = prev_aud != nullptr ? prev_aud : "";
#ifdef _WIN32
  _putenv_s("SDL_AUDIODRIVER", "dummy");
#else
  setenv("SDL_AUDIODRIVER", "dummy", 1);
#endif
  SDL_Event added_ev{};
  added_ev.type = SDL_AUDIODEVICEADDED;
  SDL_PushEvent(&added_ev);
  san4::SdlHostTestPeer::open_audio_device(host, false);
  CHECK(SDL_HasEvent(SDL_AUDIODEVICEADDED) == SDL_FALSE);
#ifdef _WIN32
  _putenv_s("SDL_AUDIODRIVER", saved_aud.c_str());
#else
  if (prev_aud != nullptr) {
    setenv("SDL_AUDIODRIVER", saved_aud.c_str(), 1);
  } else {
    unsetenv("SDL_AUDIODRIVER");
  }
#endif
}

// Verifies cross-TU / cross-archive LTO invariants:
//   1. Inlined and indirect calls into target("avx2") + scalar SIMD kernels
//      produce bit-identical results while honoring runtime ScopedLevelOverride
//      transitions without LTO constant-propagation across atomic state.
//   2. Overlapping 8/16/32-bit unaligned guest memory mutations across
//      conventional RAM and EMS page-frame boundaries preserve exact x86
//      aliasing semantics when C++20 machine callbacks and C libx86emu are
//      linked under Full or Thin LTO.
void test_lto_cross_tu_simd_dispatch_and_aliasing_safety() {
  using san4::simd::GradeFn;
  using san4::simd::ScopedLevelOverride;
  using san4::simd::SimdLevel;

  using Blend4xFn = void (*)(std::uint32_t*, const std::uint32_t*, int,
                             const GradeFn*);
  using BlendBoxFn = void (*)(std::uint32_t*, const std::uint32_t*,
                              std::ptrdiff_t, int, int, const GradeFn*);
  using ActiveFn = SimdLevel (*)();

  volatile Blend4xFn indirect_4x =
      static_cast<Blend4xFn>(&san4::simd::blend_row_4x);
  volatile BlendBoxFn indirect_box =
      static_cast<BlendBoxFn>(&san4::simd::blend_row_box);
  volatile ActiveFn indirect_active = &san4::simd::active_level;

  TestRng rng(0x4C544F3231ULL);
  constexpr int kWidth = 37;
  constexpr int kDs = 2;
  constexpr std::ptrdiff_t kStride = kWidth * kDs + 3;
  std::vector<std::uint32_t> src4x(static_cast<std::size_t>(kWidth));
  std::vector<std::uint32_t> src_box(static_cast<std::size_t>(kStride * kDs));
  std::vector<std::uint32_t> base_dst(static_cast<std::size_t>(kWidth));
  for (auto& px : src4x) {
    px = random_hd_pixel(rng);
  }
  for (auto& px : src_box) {
    px = random_hd_pixel(rng);
  }
  for (auto& px : base_dst) {
    px = 0xFF000000u | (rng.next() & 0x00FFFFFFu);
  }

  const std::uint32_t salt = 0x5AC3u;
  const GradeFn grade{&test_grade, &salt};

  std::vector<std::uint32_t> expected_4x = base_dst;
  std::vector<std::uint32_t> expected_box = base_dst;
  for (int i = 0; i < kWidth; ++i) {
    ref_blend_pixel(expected_4x[static_cast<std::size_t>(i)],
                    src4x[static_cast<std::size_t>(i)], &grade);
    ref_blend_pixel(expected_box[static_cast<std::size_t>(i)],
                    ref_box_sample(src_box.data(), kStride, kDs, i), &grade);
  }

  const SimdLevel hw = san4::simd::detected_level();
  for (int iter = 0; iter < 16; ++iter) {
    const SimdLevel cap =
        (iter % 2 == 0) ? SimdLevel::Scalar : SimdLevel::Avx2;
    ScopedLevelOverride guard(cap);
    const SimdLevel want_active =
        (cap == SimdLevel::Scalar) ? SimdLevel::Scalar : hw;
    CHECK(san4::simd::active_level() == want_active);
    CHECK(indirect_active() == want_active);

    std::vector<std::uint32_t> inlined_4x = base_dst;
    std::vector<std::uint32_t> fptr_4x = base_dst;
    san4::simd::blend_row_4x(inlined_4x.data(), src4x.data(), kWidth, &grade);
    indirect_4x(fptr_4x.data(), src4x.data(), kWidth, &grade);
    CHECK(inlined_4x == expected_4x);
    CHECK(fptr_4x == expected_4x);

    std::vector<std::uint32_t> inlined_box = base_dst;
    std::vector<std::uint32_t> fptr_box = base_dst;
    san4::simd::blend_row_box(inlined_box.data(), src_box.data(), kStride, kDs,
                              kWidth, &grade);
    indirect_box(fptr_box.data(), src_box.data(), kStride, kDs, kWidth, &grade);
    CHECK(inlined_box == expected_box);
    CHECK(fptr_box == expected_box);
  }
  CHECK(san4::simd::active_level() == hw);

  // Overlapping 8/16/32-bit unaligned reads/writes across conventional RAM and
  // EMS page-frame memory executed through libx86emu.
  san4::X86Machine m;

  static constexpr std::uint8_t kAliasCode[] = {
      // Allocate 2 EMS pages and map logical 0 -> phys 0, logical 1 -> phys 1.
      0xB4, 0x43,                                      // mov ah, 0x43
      0xBB, 0x02, 0x00,                                // mov bx, 2
      0xCD, 0x67,                                      // int 0x67 -> DX = handle
      0xB8, 0x00, 0x44,                                // mov ax, 0x4400 (phys 0)
      0xBB, 0x00, 0x00,                                // mov bx, 0 (log 0)
      0xCD, 0x67,                                      // int 0x67
      0xB8, 0x01, 0x44,                                // mov ax, 0x4401 (phys 1)
      0xBB, 0x01, 0x00,                                // mov bx, 1 (log 1)
      0xCD, 0x67,                                      // int 0x67
      // Unaligned overlapping dword/word/byte mutations at 0x1800:0x0003.
      0xB8, 0x00, 0x18,                                // mov ax, 0x1800
      0x8E, 0xD8,                                      // mov ds, ax
      0x66, 0xC7, 0x06, 0x03, 0x00, 0xD4, 0xC3, 0xB2, 0xA1,  // mov dword [3], 0xA1B2C3D4
      0x81, 0x06, 0x04, 0x00, 0x22, 0x11,              // add word [4], 0x1122
      0x80, 0x36, 0x05, 0x00, 0x5A,                    // xor byte [5], 0x5A
      0x66, 0x8B, 0x06, 0x03, 0x00,                    // mov eax, [3]
      0x66, 0x89, 0x06, 0x10, 0x00,                    // mov [0x10], eax
      // Same overlapping sequence straddling EMS physical pages 0 & 1 at D000:3FFE.
      0xB8, 0x00, 0xD0,                                // mov ax, 0xD000
      0x8E, 0xD8,                                      // mov ds, ax
      0x66, 0xC7, 0x06, 0xFE, 0x3F, 0xD4, 0xC3, 0xB2, 0xA1,  // mov dword [0x3FFE], 0xA1B2C3D4
      0x81, 0x06, 0xFF, 0x3F, 0x22, 0x11,              // add word [0x3FFF], 0x1122
      0x80, 0x36, 0x00, 0x40, 0x5A,                    // xor byte [0x4000], 0x5A
      0x66, 0x8B, 0x0E, 0xFE, 0x3F,                    // mov ecx, [0x3FFE]
      0xB8, 0x00, 0x18,                                // mov ax, 0x1800
      0x8E, 0xD8,                                      // mov ds, ax
      0x66, 0x89, 0x0E, 0x14, 0x00,                    // mov [0x14], ecx
      // Park in an idle BDA keyboard poll so run_frame exits deterministically.
      0x31, 0xC0,                                      // xor ax, ax
      0x8E, 0xD8,                                      // mov ds, ax
      0x8B, 0x36, 0x1C, 0x04,                          // poll: mov si, [0x041C]
      0x3B, 0x36, 0x1A, 0x04,                          // cmp si, [0x041A]
      0x74, 0xF6,                                      // je poll
  };
  m.load_synthetic_code(0x2000u, 0x0100u, kAliasCode);
  m.run_frame(1.0 / 60.0, 0.020);

  // Reference calculation of the exact byte-level sequence:
  //   initial bytes [0..3] = {0xD4, 0xC3, 0xB2, 0xA1}
  //   word at [1..2] = 0xB2C3 + 0x1122 = 0xC3E5 -> bytes {0xD4, 0xE5, 0xC3, 0xA1}
  //   byte [2] ^= 0x5A -> 0xC3 ^ 0x5A = 0x99    -> dword = 0xA199E5D4
  constexpr std::uint32_t kExpectedAliasDword = 0xA199E5D4u;
  constexpr std::uint32_t kOutBase = (0x1800u << 4);
  CHECK(read_ram32(m, kOutBase + 0x10u) == kExpectedAliasDword);
  CHECK(read_ram32(m, kOutBase + 0x14u) == kExpectedAliasDword);
}

void test_win32_render_policy_and_unicode_png_paths() {
  CHECK(std::string_view(san4::preferred_render_driver(true)) == "direct3d11");
  CHECK(std::string_view(san4::preferred_render_driver(false)) == "opengles2");

  const std::string cjk_name = "三國志IV_測試";
  const std::filesystem::path rel_cjk = san4::util::utf8_to_path(cjk_name);
  CHECK(san4::util::path_to_utf8(rel_cjk) == cjk_name);

  ScopedTempDir tmp;
  const std::filesystem::path unicode_dir = tmp.path() / rel_cjk;
  std::filesystem::create_directories(unicode_dir);
  CHECK(san4::win32_platform::is_directory_writable(unicode_dir));

  // Verify in-memory libpng + std::filesystem::path I/O and loose HD pack
  // discovery inside a Traditional Chinese directory path.
  constexpr int kW = 8;
  constexpr int kH = 8;
  std::vector<std::uint32_t> argb(static_cast<std::size_t>(kW * kH));
  for (int i = 0; i < kW * kH; ++i) {
    argb[static_cast<std::size_t>(i)] =
        0xFF000000u | (static_cast<std::uint32_t>(i * 97 + 13) & 0x00FFFFFFu);
  }
  const std::filesystem::path png_path =
      unicode_dir / san4::util::utf8_to_path("img_2x2_0123456789abcdef.png");
  CHECK(san4::HdPackManager::save_argb_png(png_path, kW, kH, argb.data()));

  int loaded_w = 0;
  int loaded_h = 0;
  std::vector<std::uint32_t> loaded_px;
  CHECK(san4::HdPackManager::load_argb_png(png_path, loaded_w, loaded_h,
                                           loaded_px));
  CHECK(loaded_w == kW && loaded_h == kH);
  CHECK(loaded_px == argb);

  san4::HdPackManager pack;
  pack.set_pack_dir(unicode_dir);
  CHECK(pack.reload_pack() == 1u);
  CHECK(pack.loaded_texture_count() == 1u);

  // Verify CaseInsensitiveVfs inside the Traditional Chinese directory.
  const std::filesystem::path vfs_file = unicode_dir / "SAVEDAT.S4";
  {
    std::ofstream out(vfs_file, std::ios::binary);
    out << "SAN4_WIN11_UTF8_VFS";
  }
  san4::CaseInsensitiveVfs vfs(unicode_dir);
  auto resolved = vfs.resolve_for_read("savedat.s4");
  CHECK(resolved.has_value());
  CHECK(std::filesystem::equivalent(*resolved, vfs_file));
  const auto matches = vfs.find_matching("*.s4");
  CHECK(matches.size() == 1u && matches[0].dos_name == "SAVEDAT.S4");

  // Verify game directory candidate selection skips empty folders and prefers
  // a candidate directory containing SAN486.COM / MAIN.EXE.
  {
    ScopedTempDir layout;
    const std::filesystem::path empty_sub = layout.path() / "sub" / "SAN4";
    const std::filesystem::path real_parent_san4 = layout.path() / "SAN4";
    std::filesystem::create_directories(empty_sub);
    std::filesystem::create_directories(real_parent_san4);
    {
      std::ofstream marker(real_parent_san4 / "SAN486.COM", std::ios::binary);
      marker << "MZ";
    }
    const auto check = san4::resolve_and_verify_game_dir(
        "", layout.path() / "sub" / "san4.exe");
    CHECK(std::filesystem::equivalent(check.resolved_dir, real_parent_san4));
  }

  // Verify show_error_dialog is safe and non-blocking when SAN4_NO_ERROR_DIALOG=1.
#ifdef _WIN32
  _putenv_s("SAN4_NO_ERROR_DIALOG", "1");
#else
  setenv("SAN4_NO_ERROR_DIALOG", "1", 1);
#endif
  san4::win32_platform::show_error_dialog("SAN4 Unit Test", "Suppressed dialog");
}

void test_hd_pack_small_cache_compositing_and_bounds() {
  auto encode_interleaved_from_slots = [](int w, int h, int planes,
                                          const std::uint8_t* vga_slots) {
    const int cols = (w + 7) / 8;
    std::vector<std::uint8_t> out(
        static_cast<std::size_t>(cols * planes * h), 0u);
    for (int y = 0; y < h; ++y) {
      for (int col = 0; col < cols; ++col) {
        std::uint8_t pb[4]{0u, 0u, 0u, 0u};
        for (int bit = 0; bit < 8; ++bit) {
          const int x = col * 8 + bit;
          if (x >= w) break;
          const std::uint8_t slot =
              static_cast<std::uint8_t>(vga_slots[y * w + x] & 0x0Fu);
          const std::uint8_t gc = san4::kGrpSlotMap[slot];
          const unsigned sh = static_cast<unsigned>(7 - bit);
          for (int p = 0; p < planes; ++p) {
            pb[p] |= static_cast<std::uint8_t>(((gc >> p) & 1u) << sh);
          }
        }
        std::uint8_t* dst =
            out.data() + static_cast<std::size_t>((y * cols + col) * planes);
        for (int p = 0; p < planes; ++p) {
          dst[p] = pb[p];
        }
      }
    }
    return out;
  };

  auto format_pal_csv = [](const std::array<std::uint32_t, 16>& pal) {
    std::string s;
    char buf[16];
    for (int i = 0; i < 16; ++i) {
      if (i > 0) s.push_back(',');
      std::snprintf(buf, sizeof(buf), "%06X",
                    pal[static_cast<std::size_t>(i)] & 0x00FFFFFFu);
      s += buf;
    }
    return s;
  };

  ScopedTempDir tmp;
  const auto pack_dir = tmp.path() / "pack_small_cache";
  std::filesystem::create_directories(pack_dir / "mondata");
  std::filesystem::create_directories(pack_dir / "sprites" / "buttons");
  std::filesystem::create_directories(pack_dir / "tiles");

  // 1) Valid MONDATA parts + malformed MONDATA part (wrong dimensions).
  std::vector<std::uint8_t> mon_ok_slots(64u * 80u, san4::kGrpSlotMap[9]);
  mon_ok_slots[0] = san4::kGrpSlotMap[14];
  const std::uint64_t mon_ok_hash =
      san4::HdPackManager::hash_sprite_4bit(64, 80, mon_ok_slots.data());

  std::vector<std::uint8_t> mon_bad_slots(64u * 80u, san4::kGrpSlotMap[10]);
  mon_bad_slots[1] = san4::kGrpSlotMap[15];
  const std::uint64_t mon_bad_hash =
      san4::HdPackManager::hash_sprite_4bit(64, 80, mon_bad_slots.data());

  {
    const std::vector<std::uint32_t> top_px(256u * 144u, 0xFF113355u);
    const std::vector<std::uint32_t> bot_px(256u * 176u, 0xFF225577u);
    std::vector<std::uint32_t> eye_px(256u * 64u, 0x00000000u);
    std::fill_n(eye_px.data() + 256u * 4u, 256u * 4u, 0xFFAAEEFFu);
    std::vector<std::uint32_t> mouth_px(256u * 80u, 0x00000000u);
    std::fill_n(mouth_px.data() + 256u * 4u, 256u * 4u, 0xFFEE4455u);
    CHECK(san4::HdPackManager::save_argb_png(
        pack_dir / "mondata" / "t00_top_0.png", 256, 144, top_px.data()));
    CHECK(san4::HdPackManager::save_argb_png(
        pack_dir / "mondata" / "t00_bot_0.png", 256, 176, bot_px.data()));
    CHECK(san4::HdPackManager::save_argb_png(
        pack_dir / "mondata" / "t00_eye_0.png", 256, 64, eye_px.data()));
    CHECK(san4::HdPackManager::save_argb_png(
        pack_dir / "mondata" / "t00_mouth_0.png", 256, 80, mouth_px.data()));

    const std::vector<std::uint32_t> bad_top_px(64u * 64u, 0xFFFF0000u);
    CHECK(san4::HdPackManager::save_argb_png(
        pack_dir / "mondata" / "t01_top_0.png", 64, 64, bad_top_px.data()));

    san4::MondataIndexEntry entries[2]{};
    entries[0].hash64 = mon_ok_hash;
    entries[0].tmpl = 0u;
    entries[0].top = 0u;
    entries[0].bot = 0u;
    entries[0].eye = 0u;
    entries[0].mouth = 0u;
    entries[0].eye_y = 20u;
    entries[0].mouth_y = 40u;

    entries[1].hash64 = mon_bad_hash;
    entries[1].tmpl = 1u;
    entries[1].top = 0u;
    entries[1].bot = 0u;
    entries[1].eye = 0u;
    entries[1].mouth = 0u;
    entries[1].eye_y = 20u;
    entries[1].mouth_y = 40u;

    std::ofstream mfs(pack_dir / "mondata" / "mondata_index.bin",
                      std::ios::binary);
    mfs.write(reinterpret_cast<const char*>(entries), sizeof(entries));
  }

  // 2) Valid Button parts + malformed Button part.
  std::vector<std::uint8_t> btn_ok_slots(32u * 32u, san4::kGrpSlotMap[12]);
  btn_ok_slots[2] = san4::kGrpSlotMap[15];
  const std::uint64_t btn_ok_hash =
      san4::HdPackManager::hash_sprite_4bit(32, 32, btn_ok_slots.data());

  std::vector<std::uint8_t> btn_bad_slots(32u * 32u, san4::kGrpSlotMap[11]);
  btn_bad_slots[5] = san4::kGrpSlotMap[14];
  const std::uint64_t btn_bad_hash =
      san4::HdPackManager::hash_sprite_4bit(32, 32, btn_bad_slots.data());

  {
    const std::vector<std::uint32_t> base_px(128u * 128u, 0xFF304050u);
    std::vector<std::uint32_t> lbl_px(128u * 64u, 0x00000000u);
    std::fill_n(lbl_px.data() + 128u * 8u, 128u * 8u, 0xFFF0D060u);
    CHECK(san4::HdPackManager::save_argb_png(
        pack_dir / "sprites" / "buttons" / "base_00.png", 128, 128,
        base_px.data()));
    CHECK(san4::HdPackManager::save_argb_png(
        pack_dir / "sprites" / "buttons" / "label_00.png", 128, 64,
        lbl_px.data()));

    const std::vector<std::uint32_t> bad_base(32u * 32u, 0xFFFF00FFu);
    CHECK(san4::HdPackManager::save_argb_png(
        pack_dir / "sprites" / "buttons" / "base_01.png", 32, 32,
        bad_base.data()));

    san4::ButtonIndexEntry bentries[2]{};
    bentries[0].hash64 = btn_ok_hash;
    bentries[0].base_id = 0u;
    bentries[0].label_id = 0u;

    bentries[1].hash64 = btn_bad_hash;
    bentries[1].base_id = 1u;
    bentries[1].label_id = 0u;

    std::ofstream bfs(pack_dir / "sprites" / "buttons" / "button_index.bin",
                      std::ios::binary);
    bfs.write(reinterpret_cast<const char*>(bentries), sizeof(bentries));
  }

  // 3) Valid 16x16 tile (64x64 HD) + oversized manifest PNG (256x256 > 16*8).
  std::array<std::uint8_t, 256> tile_slots{};
  tile_slots.fill(san4::kGrpSlotMap[2]);
  tile_slots[0] = san4::kGrpSlotMap[5];
  const std::uint64_t tile_hash =
      san4::HdPackManager::hash_sprite_4bit(16, 16, tile_slots.data());

  std::array<std::uint8_t, 256> over_slots{};
  over_slots.fill(san4::kGrpSlotMap[3]);
  over_slots[1] = san4::kGrpSlotMap[6];
  const std::uint64_t over_hash =
      san4::HdPackManager::hash_sprite_4bit(16, 16, over_slots.data());

  const auto master_pal = san4::codecs::default_san4_master_palette();
  auto alt_pal = master_pal;
  alt_pal[2] = 0xFF108040u;
  alt_pal[5] = 0xFFA0E080u;
  const std::string master_csv = format_pal_csv(master_pal);
  const std::string alt_csv = format_pal_csv(alt_pal);

  {
    const std::vector<std::uint32_t> tile_px(64u * 64u, 0xFF207030u);
    CHECK(san4::HdPackManager::save_argb_png(
        pack_dir / "tiles" / "ok_tile.png", 64, 64, tile_px.data()));
    const std::vector<std::uint32_t> over_px(256u * 256u, 0xFF990099u);
    CHECK(san4::HdPackManager::save_argb_png(
        pack_dir / "tiles" / "oversize_tile.png", 256, 256, over_px.data()));

    std::ofstream mf(pack_dir / "hd_manifest.tsv", std::ios::binary);
    char hbuf[32];
    std::snprintf(hbuf, sizeof(hbuf), "%016llx",
                  static_cast<unsigned long long>(tile_hash));
    mf << hbuf << "\t16\t16\t2\ttiles/ok_tile.png\t" << alt_csv << "\n";
    std::snprintf(hbuf, sizeof(hbuf), "%016llx",
                  static_cast<unsigned long long>(over_hash));
    mf << hbuf << "\t16\t16\t2\ttiles/oversize_tile.png\t" << alt_csv << "\n";
  }

  san4::HdPackManager hd;
  hd.set_pack_dir(pack_dir);
  // Constrain cache to 64 KiB: MONDATA parts (144 KiB / 176 KiB) and composited
  // portrait (320 KiB) exceed max_cache_bytes, while button base (64 KiB) +
  // label (32 KiB) forces LRU eviction mid-composite!
  hd.set_max_cache_bytes(64u * 1024u);

  san4::VesaPlanarVideo vid;
  vid.reset_mode(0x0102u);
  {
    std::array<std::uint8_t, 48> master_brg{};
    for (int gc = 0; gc < 16; ++gc) {
      const std::uint32_t c = master_pal[san4::kGrpSlotMap[gc]];
      master_brg[static_cast<std::size_t>(gc * 3 + 0)] =
          static_cast<std::uint8_t>((c & 0xFFu) / 0x11u);
      master_brg[static_cast<std::size_t>(gc * 3 + 1)] =
          static_cast<std::uint8_t>(((c >> 16) & 0xFFu) / 0x11u);
      master_brg[static_cast<std::size_t>(gc * 3 + 2)] =
          static_cast<std::uint8_t>(((c >> 8) & 0xFFu) / 0x11u);
    }
    vid.begin_smooth_fade(0u, 16u, master_brg.data(), master_brg.data(),
                          san4::kGrpSlotMap.data(), 0.01);
    vid.finish_fade_immediately();
  }
  vid.latch_frame();
  vid.clear_hd_occlude_bits();
  const san4::GrpClipRect full_clip{0, 0, 799, 599};

  const auto mon_ok_raw =
      encode_interleaved_from_slots(64, 80, 3, mon_ok_slots.data());
  const auto btn_ok_raw =
      encode_interleaved_from_slots(32, 32, 3, btn_ok_slots.data());
  for (int iter = 0; iter < 3; ++iter) {
    hd.on_planar_blit_source(100, 20, 64, 80, 3, 1u, 0xFFu, 0u, 0u, full_clip,
                             mon_ok_raw, vid);
    CHECK(hd.cache_resident_bytes() <= hd.max_cache_bytes());
    hd.on_planar_blit_source(220, 20, 32, 32, 3, 1u, 0xFFu, 0u, 0u, full_clip,
                             btn_ok_raw, vid);
    CHECK(hd.cache_resident_bytes() <= hd.max_cache_bytes());
  }
  CHECK(hd.active_overlay_count() >= 2u);

  // Malformed parts and oversized (> 8x base) manifest PNGs must be rejected.
  const std::size_t overlays_before_bad = hd.active_overlay_count();
  const auto mon_bad_raw =
      encode_interleaved_from_slots(64, 80, 3, mon_bad_slots.data());
  hd.on_planar_blit_source(300, 20, 64, 80, 3, 1u, 0xFFu, 0u, 0u, full_clip,
                           mon_bad_raw, vid);
  const auto btn_bad_raw =
      encode_interleaved_from_slots(32, 32, 3, btn_bad_slots.data());
  hd.on_planar_blit_source(400, 20, 32, 32, 3, 1u, 0xFFu, 0u, 0u, full_clip,
                           btn_bad_raw, vid);
  const auto over_raw =
      encode_interleaved_from_slots(16, 16, 4, over_slots.data());
  hd.on_planar_blit_source(500, 20, 16, 16, 4, 0u, 0u, 0u, 0u, full_clip,
                           over_raw, vid);
  CHECK(hd.active_overlay_count() == overlays_before_bad);
  CHECK(hd.cache_resident_bytes() <= hd.max_cache_bytes());

  // Composite onto 4x frame and verify exact composited colors for MONDATA & button.
  std::vector<std::uint32_t> frame_4x(3200u * 2400u, 0xFF000000u);
  san4::UpscaleDirtyRect dirty{
      .has_changes = true, .x = 0, .y = 0, .w = 3200, .h = 2400};
  hd.composite_overlays(frame_4x, 3200, 2400, 4, vid, dirty);
  // MONDATA at (100, 20) -> 4x (400, 80): top=0xFF113355, eye at y=(20+20)*4+4 = 164 -> 0xFFAAEEFF
  CHECK(frame_4x[80u * 3200u + 400u] == 0xFF113355u);
  CHECK(frame_4x[164u * 3200u + 400u] == 0xFFAAEEFFu);
  // Button at (220, 20) -> 4x (880, 80): base=0xFF304050, label at y=80+64+8 = 152 -> 0xFFF0D060
  CHECK(frame_4x[80u * 3200u + 880u] == 0xFF304050u);
  CHECK(frame_4x[152u * 3200u + 880u] == 0xFFF0D060u);

  // [P2-1] F8 hot-reload keeps existing hd_ref_pal_id_ valid even if manifest
  // palette ordering changes or shrinks.
  const auto tile_raw =
      encode_interleaved_from_slots(16, 16, 4, tile_slots.data());
  hd.on_planar_blit_source(0, 0, 16, 16, 4, 0u, 0u, 0u, 0u, full_clip, tile_raw,
                           vid);
  CHECK(san4::HdPackManagerTestPeer::pal_id_valid_at(hd, 0, 0));
  {
    std::ofstream mf(pack_dir / "hd_manifest.tsv",
                     std::ios::binary | std::ios::trunc);
    mf << "deadbeef00000001\t16\t16\t2\ttiles/ok_tile.png\t" << master_csv
       << "\n";
  }
  hd.reload_pack();
  CHECK(san4::HdPackManagerTestPeer::pal_id_valid_at(hd, 0, 0));
  dirty = {.has_changes = true, .x = 0, .y = 0, .w = 64, .h = 64};
  hd.composite_overlays(frame_4x, 3200, 2400, 4, vid, dirty);

  // [P3 / F-09.5] Extreme int inputs in on_vram_copy_rect must never overflow.
  hd.on_vram_copy_rect(std::numeric_limits<int>::max() - 4,
                       std::numeric_limits<int>::max() - 4, 100, 100,
                       std::numeric_limits<int>::min() + 4,
                       std::numeric_limits<int>::min() + 4);
  hd.on_vram_copy_rect(-1000000000, -1000000000, 2000000000, 2000000000,
                       1000000000, 1000000000);
}

void test_hd_pack_path_sanitization_and_symlink_rejection() {
  std::filesystem::path rel;
  CHECK(san4::HdPackManager::sanitize_pack_relpath("tiles/tile_01.png", rel));
  CHECK(san4::HdPackManager::sanitize_pack_relpath("sprites/三國/icon.png", rel));

  static constexpr std::string_view kBadPaths[] = {
      "",
      "/etc/passwd",
      "tiles/",
      "tiles//a.png",
      "./tile.png",
      "tiles/../escape.png",
      "\\\\server\\share\\x.png",
      "D:\\folder\\x.png",
      "tiles/stream:ads.png",
      "CON.png",
      "PRN",
      "AUX.png",
      "NUL.png",
      "CLOCK$.png",
      "CONIN$.png",
      "CONOUT$.png",
      "sprites/com1.png",
      "sprites/COM9.tar.png",
      "sprites/lpt1.png",
      "sprites/LPT9.png",
      "tiles/bad_dot./a.png",
      "tiles/bad_space /a.png",
      "tiles/ctrl\x01.png",
      "tiles/del\x7f.png",
  };
  for (std::string_view bad : kBadPaths) {
    CHECK(!san4::HdPackManager::sanitize_pack_relpath(bad, rel));
  }

  ScopedTempDir tmp;
  const auto outside_dir = tmp.path() / "outside";
  const auto pack_dir = tmp.path() / "pack";
  std::filesystem::create_directories(outside_dir);
  std::filesystem::create_directories(pack_dir / "tiles");

  const std::vector<std::uint32_t> px(64u * 64u, 0xFF4488CCu);
  const auto outside_png = outside_dir / "secret.png";
  const auto inside_png = pack_dir / "tiles" / "real.png";
  CHECK(san4::HdPackManager::save_argb_png(outside_png, 64, 64, px.data()));
  CHECK(san4::HdPackManager::save_argb_png(inside_png, 64, 64, px.data()));

  std::filesystem::path full_out;
  std::uintmax_t sz_out = 0u;
  CHECK(san4::HdPackManager::sanitize_pack_relpath("tiles/real.png", rel));
  CHECK(san4::HdPackManager::verify_safe_pack_file(pack_dir, rel, full_out,
                                                   &sz_out));
  CHECK(sz_out > 0u);

  std::error_code sym_ec;
  std::filesystem::create_symlink(outside_png, pack_dir / "tiles" / "sym.png",
                                  sym_ec);
  if (!sym_ec) {
    CHECK(san4::HdPackManager::sanitize_pack_relpath("tiles/sym.png", rel));
    CHECK(!san4::HdPackManager::verify_safe_pack_file(pack_dir, rel, full_out));

    int lw = 0;
    int lh = 0;
    std::vector<std::uint32_t> lpx;
    CHECK(!san4::HdPackManager::load_argb_png(pack_dir / "tiles" / "sym.png", lw,
                                              lh, lpx));

    std::error_code dir_sym_ec;
    std::filesystem::create_directory_symlink(outside_dir, pack_dir / "symdir",
                                              dir_sym_ec);
    if (!dir_sym_ec) {
      CHECK(
          san4::HdPackManager::sanitize_pack_relpath("symdir/secret.png", rel));
      CHECK(
          !san4::HdPackManager::verify_safe_pack_file(pack_dir, rel, full_out));
    }

    std::filesystem::create_symlink(
        outside_png, pack_dir / "img_16x16_0123456789abcdef.png", sym_ec);
    san4::HdPackManager hd;
    hd.set_pack_dir(pack_dir);
    CHECK(hd.loaded_texture_count() == 0u);
  }
}

void test_dos_fstream_rw_sync_commit_delete_and_nested_dta() {
  ScopedTempDir tmp;
  const auto game_dir = tmp.path() / "game";
  const auto save_dir = tmp.path() / "save";
  std::filesystem::create_directories(game_dir);
  std::filesystem::create_directories(save_dir);

  // Create a 16 KiB test file (> 8 KiB libstdc++ filebuf buffer size).
  std::vector<std::uint8_t> expected(16384u);
  for (std::size_t i = 0; i < expected.size(); ++i) {
    expected[i] = static_cast<std::uint8_t>((i * 37u + 5u) & 0xFFu);
  }
  {
    std::ofstream ofs(save_dir / "RWFILE.BIN", std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(expected.data()),
              static_cast<std::streamsize>(expected.size()));
    std::ofstream(game_dir / "BASEONLY.DAT", std::ios::binary) << "immutable";
    std::ofstream(game_dir / "A1.A", std::ios::binary) << "a1";
    std::ofstream(game_dir / "A2.A", std::ios::binary) << "a2";
    std::ofstream(game_dir / "B1.B", std::ios::binary) << "b1";
    std::ofstream(game_dir / "B2.B", std::ios::binary) << "b2";
  }

  san4::MachineConfig cfg;
  cfg.game_dir = game_dir;
  cfg.save_dir = save_dir;
  san4::X86Machine m(cfg);
  x86emu_t* emu = m.emu();
  x86emu_set_seg_register(emu, emu->x86.R_DS_SEL, 0x2000u);

  std::memcpy(m.guest_ram() + 0x20000u, "RWFILE.BIN\0", 11u);
  std::memcpy(m.guest_ram() + 0x20020u, "BASEONLY.DAT\0", 13u);
  std::memcpy(m.guest_ram() + 0x20040u, "TEMP.SAV\0", 9u);
  std::memcpy(m.guest_ram() + 0x20060u, "*.A\0", 4u);
  std::memcpy(m.guest_ram() + 0x20070u, "*.B\0", 4u);

  // Open RWFILE.BIN read/write (AX=3D02h).
  emu->x86.R_AX = 0x3D02u;
  emu->x86.R_DX = 0x0000u;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u);
  const std::uint16_t fh = emu->x86.R_AX;

  // 1) Read 100 bytes (< 8 KB), then immediately write 64 bytes WITHOUT LSEEK!
  emu->x86.R_AH = 0x3Fu;
  emu->x86.R_BX = fh;
  emu->x86.R_CX = 100u;
  emu->x86.R_DX = 0x1000u;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u && emu->x86.R_AX == 100u);
  CHECK(std::memcmp(m.guest_ram() + 0x21000u, expected.data(), 100u) == 0);

  std::memset(m.guest_ram() + 0x20800u, 0xCC, 64u);
  std::fill_n(expected.data() + 100u, 64u, static_cast<std::uint8_t>(0xCCu));
  emu->x86.R_AH = 0x40u;
  emu->x86.R_BX = fh;
  emu->x86.R_CX = 64u;
  emu->x86.R_DX = 0x0800u;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u && emu->x86.R_AX == 64u);

  // 2) Immediately read 12,000 bytes (> 8 KB) WITHOUT LSEEK!
  emu->x86.R_AH = 0x3Fu;
  emu->x86.R_BX = fh;
  emu->x86.R_CX = 12000u;
  emu->x86.R_DX = 0x2000u;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u && emu->x86.R_AX == 12000u);
  CHECK(std::memcmp(m.guest_ram() + 0x22000u, expected.data() + 164u, 12000u) ==
        0);

  // 3) Immediately write 128 bytes (< 8 KB) at offset 12164, commit via AH=68h
  //    & AH=6Ah, then immediately read 200 bytes WITHOUT LSEEK!
  std::memset(m.guest_ram() + 0x20900u, 0x55, 128u);
  std::fill_n(expected.data() + 12164u, 128u, static_cast<std::uint8_t>(0x55u));
  emu->x86.R_AH = 0x40u;
  emu->x86.R_BX = fh;
  emu->x86.R_CX = 128u;
  emu->x86.R_DX = 0x0900u;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u && emu->x86.R_AX == 128u);

  emu->x86.R_AH = 0x68u;
  emu->x86.R_BX = fh;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u);

  emu->x86.R_AH = 0x6Au;
  emu->x86.R_BX = fh;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u);

  // Invalid handle commit must fail with CF=1, AX=6.
  emu->x86.R_AH = 0x68u;
  emu->x86.R_BX = 0x00FEu;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) != 0u && emu->x86.R_AX == 6u);

  emu->x86.R_AH = 0x3Fu;
  emu->x86.R_BX = fh;
  emu->x86.R_CX = 200u;
  emu->x86.R_DX = 0x6000u;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u && emu->x86.R_AX == 200u);
  CHECK(std::memcmp(m.guest_ram() + 0x26000u, expected.data() + 12292u, 200u) ==
        0);

  emu->x86.R_AH = 0x3Eu;
  emu->x86.R_BX = fh;
  m.dos().handle_int21(emu);

  std::vector<std::uint8_t> on_disk(16384u);
  {
    std::ifstream ifs(save_dir / "RWFILE.BIN", std::ios::binary);
    ifs.read(reinterpret_cast<char*>(on_disk.data()),
             static_cast<std::streamsize>(on_disk.size()));
    CHECK(ifs.gcount() == static_cast<std::streamsize>(expected.size()));
  }
  CHECK(on_disk == expected);

  // 4) Nested FindFirst/FindNext across two distinct DTAs (0x0200 and 0x0300).
  emu->x86.R_AH = 0x1Au;
  emu->x86.R_DX = 0x0200u;
  m.dos().handle_int21(emu);
  emu->x86.R_AH = 0x4Eu;
  emu->x86.R_CX = 0u;
  emu->x86.R_DX = 0x0060u;  // "*.A"
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u);
  CHECK(std::string(reinterpret_cast<const char*>(m.guest_ram() + 0x2021Eu)) ==
        "A1.A");

  // Switch to inner DTA 0x0300 and enumerate "*.B".
  emu->x86.R_AH = 0x1Au;
  emu->x86.R_DX = 0x0300u;
  m.dos().handle_int21(emu);
  emu->x86.R_AH = 0x4Eu;
  emu->x86.R_CX = 0u;
  emu->x86.R_DX = 0x0070u;  // "*.B"
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u);
  CHECK(std::string(reinterpret_cast<const char*>(m.guest_ram() + 0x2031Eu)) ==
        "B1.B");
  emu->x86.R_AH = 0x4Fu;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u);
  CHECK(std::string(reinterpret_cast<const char*>(m.guest_ram() + 0x2031Eu)) ==
        "B2.B");

  // Switch back to outer DTA 0x0200 and verify FindNext still yields "A2.A".
  emu->x86.R_AH = 0x1Au;
  emu->x86.R_DX = 0x0200u;
  m.dos().handle_int21(emu);
  emu->x86.R_AH = 0x4Fu;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u);
  CHECK(std::string(reinterpret_cast<const char*>(m.guest_ram() + 0x2021Eu)) ==
        "A2.A");

  // 5) AH=41h Delete: must NOT copy BASEONLY.DAT from game_dir into save_dir,
  //    and must cleanly close open handles before removing TEMP.SAV.
  emu->x86.R_AH = 0x41u;
  emu->x86.R_DX = 0x0020u;  // "BASEONLY.DAT"
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) != 0u);
  CHECK(std::filesystem::exists(game_dir / "BASEONLY.DAT"));
  CHECK(!std::filesystem::exists(save_dir / "BASEONLY.DAT"));

  emu->x86.R_AH = 0x3Cu;
  emu->x86.R_CX = 0u;
  emu->x86.R_DX = 0x0040u;  // "TEMP.SAV"
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u);
  const std::uint16_t temp_fh = emu->x86.R_AX;
  CHECK(std::filesystem::exists(save_dir / "TEMP.SAV"));

  // Delete TEMP.SAV while temp_fh is still open!
  emu->x86.R_AH = 0x41u;
  emu->x86.R_DX = 0x0040u;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u);
  CHECK(!std::filesystem::exists(save_dir / "TEMP.SAV"));

  emu->x86.R_AH = 0x40u;
  emu->x86.R_BX = temp_fh;
  emu->x86.R_CX = 1u;
  emu->x86.R_DX = 0x0800u;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) != 0u && emu->x86.R_AX == 6u);
}

void test_vfs_cow_readonly_saves_and_device_rejection() {
  ScopedTempDir tmp;
  const auto game_dir = tmp.path() / "ro_game";
  const auto save_dir = tmp.path() / "user_save";
  std::filesystem::create_directories(game_dir);
  std::filesystem::create_directories(save_dir);

  const auto ro_nb = game_dir / "NBDATA.S4";
  const auto ro_sav = game_dir / "SAVEDAT.S4";
  {
    std::ofstream(ro_nb, std::ios::binary) << "ORIG_NBDATA";
    std::ofstream(ro_sav, std::ios::binary) << "ORIG_SAVEDAT";
  }
  std::error_code perm_ec;
  std::filesystem::permissions(
      ro_nb,
      std::filesystem::perms::owner_write |
          std::filesystem::perms::group_write |
          std::filesystem::perms::others_write,
      std::filesystem::perm_options::remove, perm_ec);
  std::filesystem::permissions(
      ro_sav,
      std::filesystem::perms::owner_write |
          std::filesystem::perms::group_write |
          std::filesystem::perms::others_write,
      std::filesystem::perm_options::remove, perm_ec);

  struct RestorePerms {
    std::filesystem::path a;
    std::filesystem::path b;
    ~RestorePerms() {
      std::error_code ec;
      std::filesystem::permissions(a, std::filesystem::perms::owner_write,
                                   std::filesystem::perm_options::add, ec);
      std::filesystem::permissions(b, std::filesystem::perms::owner_write,
                                   std::filesystem::perm_options::add, ec);
    }
  } restore_perms{ro_nb, ro_sav};

  // Directory appears writable by itself, yet read-only NBDATA.S4 / SAVEDAT.S4
  // must make is_game_dir_writable(game_dir) return false!
  CHECK(!san4::win32_platform::is_game_dir_writable(game_dir));

#ifndef _WIN32
  const bool running_as_root = (::geteuid() == 0);
#else
  const bool running_as_root = false;
#endif
  if (!running_as_root) {
    // Without a separate save_dir, opening read-only SAVEDAT.S4 / NBDATA.S4 for
    // read/write MUST fail cleanly with AX=5 (Access Denied), never downgrading
    // silently to read-only!
    san4::MachineConfig no_save_cfg;
    no_save_cfg.game_dir = game_dir;
    san4::X86Machine m_ro(no_save_cfg);
    x86emu_t* emu = m_ro.emu();
    x86emu_set_seg_register(emu, emu->x86.R_DS_SEL, 0x2000u);
    std::memcpy(m_ro.guest_ram() + 0x20000u, "NBDATA.S4\0", 10u);
    std::memcpy(m_ro.guest_ram() + 0x20020u, "SAVEDAT.S4\0", 11u);

    emu->x86.R_AX = 0x3D02u;
    emu->x86.R_DX = 0x0000u;
    m_ro.dos().handle_int21(emu);
    CHECK((emu->x86.R_FLG & FB_CF) != 0u && emu->x86.R_AX == 5u);

    emu->x86.R_AX = 0x3D02u;
    emu->x86.R_DX = 0x0020u;
    m_ro.dos().handle_int21(emu);
    CHECK((emu->x86.R_FLG & FB_CF) != 0u && emu->x86.R_AX == 5u);
  }

  // With save_dir, CoW copy of 0444 NBDATA.S4 / SAVEDAT.S4 adds owner_write so
  // read/write open and mutation succeed cleanly!
  {
    san4::MachineConfig cow_cfg;
    cow_cfg.game_dir = game_dir;
    cow_cfg.save_dir = save_dir;
    san4::X86Machine m_cow(cow_cfg);
    x86emu_t* emu = m_cow.emu();
    x86emu_set_seg_register(emu, emu->x86.R_DS_SEL, 0x2000u);
    std::memcpy(m_cow.guest_ram() + 0x20000u, "NBDATA.S4\0", 10u);
    std::memcpy(m_cow.guest_ram() + 0x20020u, "SAVEDAT.S4\0", 11u);
    std::memcpy(m_cow.guest_ram() + 0x20040u, "MOD_", 4u);

    for (std::uint16_t name_off : {0x0000u, 0x0020u}) {
      emu->x86.R_AX = 0x3D02u;
      emu->x86.R_DX = name_off;
      m_cow.dos().handle_int21(emu);
      CHECK((emu->x86.R_FLG & FB_CF) == 0u);
      const std::uint16_t fh = emu->x86.R_AX;

      emu->x86.R_AH = 0x40u;
      emu->x86.R_BX = fh;
      emu->x86.R_CX = 4u;
      emu->x86.R_DX = 0x0040u;
      m_cow.dos().handle_int21(emu);
      CHECK((emu->x86.R_FLG & FB_CF) == 0u && emu->x86.R_AX == 4u);

      emu->x86.R_AH = 0x3Eu;
      emu->x86.R_BX = fh;
      m_cow.dos().handle_int21(emu);
    }

    std::string cow_nb;
    std::getline(std::ifstream(save_dir / "NBDATA.S4"), cow_nb);
    CHECK(cow_nb == "MOD__NBDATA");
    std::string orig_nb;
    std::getline(std::ifstream(ro_nb), orig_nb);
    CHECK(orig_nb == "ORIG_NBDATA");
  }

  // Verify user save directory auto-selection via synthetic environment path.
  {
    const auto fake_appdata = tmp.path() / "appdata_root";
    std::filesystem::create_directories(fake_appdata);
#ifdef _WIN32
    const char* prev = std::getenv("LOCALAPPDATA");
    const std::string saved = prev != nullptr ? prev : "";
    _putenv_s("LOCALAPPDATA", san4::util::path_to_utf8(fake_appdata).c_str());
    const auto chosen =
        san4::win32_platform::select_user_save_dir(tmp.path() / "bin");
    _putenv_s("LOCALAPPDATA", saved.c_str());
    CHECK(std::filesystem::equivalent(chosen,
                                      fake_appdata / "SAN4" / "saves"));
#else
    const char* prev = std::getenv("XDG_DATA_HOME");
    const std::string saved = prev != nullptr ? prev : "";
    setenv("XDG_DATA_HOME", san4::util::path_to_utf8(fake_appdata).c_str(), 1);
    const auto chosen =
        san4::win32_platform::select_user_save_dir(tmp.path() / "bin");
    if (prev != nullptr) {
      setenv("XDG_DATA_HOME", saved.c_str(), 1);
    } else {
      unsetenv("XDG_DATA_HOME");
    }
    CHECK(std::filesystem::equivalent(chosen,
                                      fake_appdata / "san4" / "saves"));
#endif
  }

  // [P1 / F-05] Win32 reserved devices, NTFS ADS, wildcards, and trailing dots/spaces.
  san4::CaseInsensitiveVfs vfs(game_dir, save_dir);
  CHECK(san4::CaseInsensitiveVfs::is_nul_or_con_device("NUL"));
  CHECK(san4::CaseInsensitiveVfs::is_nul_or_con_device("C:\\DIR\\nul.txt"));
  CHECK(!san4::CaseInsensitiveVfs::is_nul_or_con_device("COM1.TXT"));
  CHECK(san4::CaseInsensitiveVfs::is_rejected_device("COM1.TXT"));
  CHECK(san4::CaseInsensitiveVfs::is_rejected_device("SUB\\LPT9.PRN"));
  CHECK(!vfs.resolve_for_write("NUL", true).has_value());

  static constexpr std::string_view kBadDosPaths[] = {
      "COM1",
      "COM1.TXT",
      "com9.log",
      "SUB\\LPT1",
      "lpt9.prn",
      "PRN",
      "AUX.S4",
      "CLOCK$",
      "CONIN$",
      "CONOUT$",
      "SAVEDAT.S4:ads",
      "C:SAVEDAT.S4:evil",
      "BAD*NAME.S4",
      "BAD?NAME.S4",
      "BAD|NAME.S4",
      "BAD<NAME.S4",
      "BAD>NAME.S4",
      "BAD\"NAME.S4",
      "TRAIL.",
      "TRAIL ",
      "CTRL\x07.S4",
  };
  for (std::string_view bad : kBadDosPaths) {
    CHECK(!vfs.resolve_for_read(bad).has_value());
    CHECK(!vfs.resolve_for_write(bad, true).has_value());
  }
  CHECK(!std::filesystem::exists(save_dir / "COM1.TXT"));
}

void test_frame_mailbox_current_acquired_and_host_helpers() {
  san4::FrameMailbox box;
  CHECK(box.current_acquired() == nullptr);
  CHECK(box.acquire() == nullptr);

  std::vector<std::uint32_t> px1(16u * 16u, 0xFF112233u);
  const san4::UpscaleDirtyRect full16{
      .has_changes = true, .x = 0, .y = 0, .w = 16, .h = 16};
  box.publish(px1, 16, 16, san4::UpscaleMode::Nearest,
              std::span<const san4::UpscaleDirtyRect>(&full16, 1),
              san4::HostStatus{}, san4::FrameProfileRow{});

  const auto* acq1 = box.acquire();
  CHECK(acq1 != nullptr && acq1->gen == 1u);
  CHECK(box.current_acquired() == acq1);
  CHECK(box.acquire() == nullptr);
  // Even after idle acquire() returns nullptr, current_acquired() still points
  // to the consumer-owned slot for D3D11 target/device reset re-upload!
  CHECK(box.current_acquired() == acq1 &&
        box.current_acquired()->argb[0] == 0xFF112233u);

  // Producer publishes two new frames while consumer has not yet called acquire():
  // consumer's current_acquired() slot must remain untouched at gen 1!
  for (int i = 0; i < 2; ++i) {
    std::vector<std::uint32_t> pxn(
        16u * 16u, 0xFF445566u + static_cast<std::uint32_t>(i));
    box.publish(pxn, 16, 16, san4::UpscaleMode::Nearest,
                std::span<const san4::UpscaleDirtyRect>(&full16, 1),
                san4::HostStatus{}, san4::FrameProfileRow{});
  }
  CHECK(box.current_acquired() == acq1 && box.current_acquired()->gen == 1u &&
        box.current_acquired()->argb[0] == 0xFF112233u);

  const auto* acq3 = box.acquire();
  CHECK(acq3 != nullptr && acq3->gen == 3u && acq3->argb[0] == 0xFF445567u);
  CHECK(box.current_acquired() == acq3);

  // [P1-1] Initial 4:3 window clamping on 1920x1080 usable bounds (e.g. 1920x1040).
  const auto clamped_1080p =
      san4::SdlHostTestPeer::clamp_initial_window_size(1600, 1200, 1920, 1040);
  CHECK(clamped_1080p.w >= 640 && clamped_1080p.h >= 480);
  CHECK(clamped_1080p.w <= 1920 - 32 && clamped_1080p.h <= 1040 - 64);
  CHECK(std::abs(clamped_1080p.w * 3 - clamped_1080p.h * 4) <= 4);

  const auto unclamped_1440p =
      san4::SdlHostTestPeer::clamp_initial_window_size(1600, 1200, 2560, 1380);
  CHECK(unclamped_1440p.w == 1600 && unclamped_1440p.h == 1200);

  // [P1-1] Wayland 150% fractional DPI coordinate round-trip (logical 1067x800,
  // physical renderer 1600x1200, active DOS 800x600).
  for (const auto [dx, dy] :
       {std::pair{0, 0}, std::pair{400, 300}, std::pair{799, 599}}) {
    int wx = 0;
    int wy = 0;
    int rdx = -1;
    int rdy = -1;
    san4::SdlHostTestPeer::map_dos_to_window(dx, dy, 1067, 800, 1600, 1200, 800,
                                             600, wx, wy);
    san4::SdlHostTestPeer::map_window_to_dos(wx, wy, 1067, 800, 1600, 1200, 800,
                                             600, rdx, rdy);
    CHECK(std::abs(rdx - dx) <= 1 && std::abs(rdy - dy) <= 1);
  }

  // [P1-5] HostCmd::set_drain_audio toggles EmuSession::drain_audio().
  san4::X86Machine m;
  san4::PixelUpscaler up;
  san4::EmuSession sess(m, up);
  CHECK(!sess.drain_audio());
  sess.apply(san4::HostCmd::set_drain_audio(true));
  CHECK(sess.drain_audio());
  sess.apply(san4::HostCmd::set_drain_audio(false));
  CHECK(!sess.drain_audio());
}

void test_machine_rom_protection_noperm_vram_and_ubsan_shifts() {
  ScopedTempDir tmp;
  const auto data_file = tmp.path() / "PAYLOAD.BIN";
  {
    std::ofstream ofs(data_file, std::ios::binary);
    ofs.write("EMS_OK_!", 8);
  }

  san4::MachineConfig cfg;
  cfg.game_dir = tmp.path();
  san4::X86Machine m(cfg);
  x86emu_t* emu = m.emu();

  std::array<std::uint8_t, 16> rom_snap{};
  std::memcpy(rom_snap.data(), m.guest_ram() + 0xF0000u, rom_snap.size());

  // 1) 32-bit straddle write at 0xEFFFE writes 0xEFFFE..0xEFFFF while keeping
  //    BIOS ROM at 0xF0000..0xF0001 read-only!
  x86emu_write_dword(emu, 0xEFFFEu, 0xDEADBEEFu);
  CHECK(m.guest_ram()[0xEFFFEu] == 0xEFu);
  CHECK(m.guest_ram()[0xEFFFFu] == 0xBEu);
  CHECK(std::memcmp(m.guest_ram() + 0xF0000u, rom_snap.data(),
                    rom_snap.size()) == 0);

  // Direct byte/word write into 0xF0000 must also be ignored.
  x86emu_write_word(emu, 0xF0000u, 0x1234u);
  x86emu_write_byte_noperm(emu, 0xF0002u, 0x99u);
  CHECK(std::memcmp(m.guest_ram() + 0xF0000u, rom_snap.data(),
                    rom_snap.size()) == 0);

  // 2) x86emu_write_byte_noperm at 0xA0000 routes to planar VRAM!
  m.video().reset_mode(0x0102u);
  m.video().port_out8(0x03C4u, 0x02u);
  m.video().port_out8(0x03C5u, 0x0Fu);
  x86emu_write_byte_noperm(emu, 0xA0000u, 0x80u);
  m.video().latch_frame();
  CHECK(m.video().latched_vram()[0] == 15u);

  // 3) DosKernel AH=3Fh and VBE AX=4F00h respect read-only BIOS ROM and route
  //    [0xD0000, 0xDFFFF] through mapped EMS pages!
  emu->x86.R_AH = 0x43u;
  emu->x86.R_BX = 1u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0u);
  const std::uint16_t ems_handle = emu->x86.R_DX;

  emu->x86.R_AX = 0x4400u;  // physical page 0 -> D000:0000
  emu->x86.R_BX = 0u;       // logical page 0
  emu->x86.R_DX = ems_handle;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0u);

  std::memcpy(m.guest_ram() + 0x20000u, "PAYLOAD.BIN\0", 12u);
  x86emu_set_seg_register(emu, emu->x86.R_DS_SEL, 0x2000u);
  emu->x86.R_AX = 0x3D00u;
  emu->x86.R_DX = 0x0000u;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u);
  const std::uint16_t fh = emu->x86.R_AX;

  // Attempt AH=3Fh into F000:0000 -> BIOS ROM stays intact!
  x86emu_set_seg_register(emu, emu->x86.R_DS_SEL, 0xF000u);
  emu->x86.R_AH = 0x3Fu;
  emu->x86.R_BX = fh;
  emu->x86.R_CX = 8u;
  emu->x86.R_DX = 0x0000u;
  m.dos().handle_int21(emu);
  CHECK(std::memcmp(m.guest_ram() + 0xF0000u, rom_snap.data(),
                    rom_snap.size()) == 0);

  // Rewind and read AH=3Fh into EMS D000:0000 -> populates mapped EMS page!
  emu->x86.R_AX = 0x4200u;
  emu->x86.R_BX = fh;
  emu->x86.R_CX = 0u;
  emu->x86.R_DX = 0u;
  m.dos().handle_int21(emu);

  x86emu_set_seg_register(emu, emu->x86.R_DS_SEL, 0xD000u);
  emu->x86.R_AH = 0x3Fu;
  emu->x86.R_BX = fh;
  emu->x86.R_CX = 8u;
  emu->x86.R_DX = 0x0000u;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u && emu->x86.R_AX == 8u);
  for (int i = 0; i < 8; ++i) {
    CHECK(x86emu_read_byte(emu, 0xD0000u + static_cast<unsigned>(i)) ==
          static_cast<std::uint8_t>("EMS_OK_!"[i]));
  }

  // VBE AX=4F00h into F000:0000 preserves ROM; into D000:0100 writes "VESA"!
  const std::span<std::uint8_t> ram_span(m.guest_ram(),
                                         san4::X86Machine::kGuestRamBytes);
  x86emu_set_seg_register(emu, emu->x86.R_ES_SEL, 0xF000u);
  std::uint16_t es_val = emu->x86.R_ES;
  emu->x86.R_AX = 0x4F00u;
  emu->x86.R_DI = 0x0000u;
  m.video().handle_int10(emu->x86.R_AX, emu->x86.R_BX, emu->x86.R_CX,
                         emu->x86.R_DX, es_val, emu->x86.R_BP, emu->x86.R_DI,
                         ram_span, 0xF000u, 0x1000u, 0x0500u, 0x0510u, emu);
  CHECK(std::memcmp(m.guest_ram() + 0xF0000u, rom_snap.data(),
                    rom_snap.size()) == 0);

  x86emu_set_seg_register(emu, emu->x86.R_ES_SEL, 0xD000u);
  es_val = emu->x86.R_ES;
  emu->x86.R_AX = 0x4F00u;
  emu->x86.R_DI = 0x0100u;
  m.video().handle_int10(emu->x86.R_AX, emu->x86.R_BX, emu->x86.R_CX,
                         emu->x86.R_DX, es_val, emu->x86.R_BP, emu->x86.R_DI,
                         ram_span, 0xF000u, 0x1000u, 0x0500u, 0x0510u, emu);
  CHECK(emu->x86.R_AX == 0x004Fu);
  CHECK(x86emu_read_byte(emu, 0xD0100u) == 'V' &&
        x86emu_read_byte(emu, 0xD0101u) == 'E' &&
        x86emu_read_byte(emu, 0xD0102u) == 'S' &&
        x86emu_read_byte(emu, 0xD0103u) == 'A');

  // 4) Execute synthetic x86 shifts/rotates/bit-ops/SIB negative index to prove
  //    100% architectural accuracy and zero UBSan signed-shift diagnostics:
  //      - stc; mov ax, 0x8403; rcr ax, 1 -> ax = 0xC201, CF = 1
  //      - stc; mov ax, 0x8001; rcl ax, 1 -> ax = 0x0003, CF = 1
  //      - mov eax, 0x80000001; rol eax, 1 -> 0x00000003; ror eax, 1 -> 0x80000001
  //      - bts/btr/btc bit 31 on eax; shld/shrd 16 bits; 32-bit SIB [ebx + esi*4] with esi = -2
  static constexpr std::uint8_t kShiftCode[] = {
      0xB8, 0x00, 0x18,                                // mov ax, 0x1800
      0x8E, 0xD8,                                      // mov ds, ax
      0xF9,                                            // stc (CF=1)
      0xB8, 0x03, 0x84,                                // mov ax, 0x8403
      0xD1, 0xD8,                                      // rcr ax, 1 -> 0xC201, CF=1
      0xA3, 0x00, 0x00,                                // mov [0x0000], ax
      0xF9,                                            // stc
      0xB8, 0x01, 0x80,                                // mov ax, 0x8001
      0xD1, 0xD0,                                      // rcl ax, 1 -> 0x0003, CF=1
      0xA3, 0x02, 0x00,                                // mov [0x0002], ax
      0xB8, 0x03, 0x84,                                // mov ax, 0x8403
      0xB1, 0x02,                                      // mov cl, 2
      0xF8,                                            // clc (CF=0)
      0xD3, 0xD8,                                      // rcr ax, cl -> 0xA100
      0xA3, 0x04, 0x00,                                // mov [0x0004], ax
      0x66, 0x31, 0xC0,                                // xor eax, eax
      0x66, 0x0F, 0xBA, 0xE8, 0x1F,                    // bts eax, 31 -> 0x80000000
      0x66, 0x0F, 0xBA, 0xF8, 0x1F,                    // btc eax, 31 -> 0x00000000 (CF=1)
      0x66, 0x0F, 0xBA, 0xE8, 0x1F,                    // bts eax, 31 -> 0x80000000
      0x66, 0xBB, 0x78, 0x56, 0x34, 0x12,              // mov ebx, 0x12345678
      0x66, 0x0F, 0xA4, 0xD8, 0x10,                    // shld eax, ebx, 16 -> 0x00001234
      0x66, 0xA3, 0x08, 0x00,                          // mov [0x0008], eax
      // Negative SIB scaled index: [ebx + esi*4] with ebx = 0x28, esi = -2 -> [0x20]
      0x66, 0xC7, 0x06, 0x20, 0x00, 0xFE, 0xCA, 0xEF, 0xBE,  // mov dword [0x20], 0xBEEFCAFE
      0x66, 0xBB, 0x28, 0x00, 0x00, 0x00,              // mov ebx, 0x28
      0x66, 0xBE, 0xFE, 0xFF, 0xFF, 0xFF,              // mov esi, -2
      0x67, 0x66, 0x8B, 0x04, 0xB3,                    // mov eax, [ebx + esi*4]
      0x66, 0xA3, 0x0C, 0x00,                          // mov [0x000C], eax
      // Park in idle BDA keyboard poll
      0x31, 0xC0,                                      // xor ax, ax
      0x8E, 0xD8,                                      // mov ds, ax
      0x8B, 0x36, 0x1C, 0x04,                          // poll: mov si, [0x041C]
      0x3B, 0x36, 0x1A, 0x04,                          // cmp si, [0x041A]
      0x74, 0xF6,                                      // je poll
  };
  m.load_synthetic_code(0x2200u, 0x0100u, kShiftCode);
  m.run_frame(1.0 / 60.0, 0.020);

  constexpr std::uint32_t kBase = (0x1800u << 4);
  CHECK(static_cast<std::uint16_t>(read_ram32(m, kBase + 0x00u) & 0xFFFFu) ==
        0xC201u);
  CHECK(static_cast<std::uint16_t>(read_ram32(m, kBase + 0x02u) & 0xFFFFu) ==
        0x0003u);
  CHECK(static_cast<std::uint16_t>(read_ram32(m, kBase + 0x04u) & 0xFFFFu) ==
        0xA100u);
  CHECK(read_ram32(m, kBase + 0x08u) == 0x00001234u);
  CHECK(read_ram32(m, kBase + 0x0Cu) == 0xBEEFCAFEu);
}

namespace {

class ScopedEnvVar {
 public:
  ScopedEnvVar(const char* name, const char* value) : name_(name) {
    if (const char* cur = std::getenv(name)) {
      had_prev_ = true;
      prev_ = cur;
    }
    set_raw(name, value);
  }
  ~ScopedEnvVar() {
    set_raw(name_.c_str(), had_prev_ ? prev_.c_str() : nullptr);
  }
  ScopedEnvVar(const ScopedEnvVar&) = delete;
  ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

 private:
  static void set_raw(const char* name, const char* val) {
#ifdef _WIN32
    _putenv_s(name, val != nullptr ? val : "");
    const std::filesystem::path wname = san4::util::utf8_to_path(name);
    const std::filesystem::path wval =
        san4::util::utf8_to_path(val != nullptr ? val : "");
    _wputenv_s(wname.c_str(), wval.c_str());
#else
    if (val != nullptr) {
      setenv(name, val, 1);
    } else {
      unsetenv(name);
    }
#endif
  }

  std::string name_;
  std::string prev_;
  bool had_prev_ = false;
};

class ScopedCurrentDir {
 public:
  explicit ScopedCurrentDir(const std::filesystem::path& next) {
    std::error_code ec;
    prev_ = std::filesystem::current_path(ec);
    std::filesystem::current_path(next, ec);
  }
  ~ScopedCurrentDir() {
    if (!prev_.empty()) {
      std::error_code ec;
      std::filesystem::current_path(prev_, ec);
    }
  }
  ScopedCurrentDir(const ScopedCurrentDir&) = delete;
  ScopedCurrentDir& operator=(const ScopedCurrentDir&) = delete;

 private:
  std::filesystem::path prev_;
};

void write_binary_file_checked(const std::filesystem::path& p,
                               std::span<const std::uint8_t> bytes) {
  std::error_code ec;
  if (p.has_parent_path()) {
    std::filesystem::create_directories(p.parent_path(), ec);
  }
  std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
  if (!bytes.empty()) {
    ofs.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }
}

std::vector<std::uint8_t> build_synthetic_ls11(
    const std::vector<std::vector<std::uint8_t>>& records) {
  std::vector<std::uint8_t> out(0x0110u, 0u);
  out[0] = 'L';
  out[1] = 'S';
  out[2] = '1';
  out[3] = '1';
  for (int i = 0; i < 256; ++i) {
    out[0x0010u + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
  }
  const std::size_t idx_bytes = records.size() * 12u + 4u;
  out.resize(0x0110u + idx_bytes, 0u);

  auto put_be32 = [&](std::size_t off, std::uint32_t v) {
    out[off + 0u] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
    out[off + 1u] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    out[off + 2u] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    out[off + 3u] = static_cast<std::uint8_t>(v & 0xFFu);
  };

  for (std::size_t i = 0; i < records.size(); ++i) {
    const auto file_off = static_cast<std::uint32_t>(out.size());
    const auto len = static_cast<std::uint32_t>(records[i].size());
    const std::size_t idx_pos = 0x0110u + i * 12u;
    put_be32(idx_pos + 0u, len);
    put_be32(idx_pos + 4u, len);
    put_be32(idx_pos + 8u, file_off);
    out.insert(out.end(), records[i].begin(), records[i].end());
  }
  return out;
}

std::vector<std::uint8_t> build_synthetic_npk_image(
    bool use_npk16_magic, bool use_default_pc98_pal, int w, int h,
    std::uint16_t lit_word0, std::uint16_t lit_word1) {
  std::vector<std::uint8_t> out(48u, 0u);
  if (use_npk16_magic) {
    std::memcpy(out.data(), "NPK16\0", 6u);
  } else {
    std::memcpy(out.data(), "NPK016", 6u);
  }
  auto put_le16 = [&](std::size_t off, std::uint16_t v) {
    out[off + 0u] = static_cast<std::uint8_t>(v & 0xFFu);
    out[off + 1u] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
  };
  put_le16(6u, 4u);
  put_le16(8u, 640u);
  put_le16(10u, 400u);
  put_le16(12u, static_cast<std::uint16_t>(w));
  put_le16(14u, static_cast<std::uint16_t>(h));

  static constexpr std::array<std::uint16_t, 16> kDefPc98 = {
      0x000u, 0x00Fu, 0x0F0u, 0x0FFu, 0xF00u, 0xF0Fu, 0xFF0u, 0xFFFu,
      0x777u, 0x00Au, 0x0A0u, 0x0AAu, 0xA00u, 0xA0Au, 0xAA0u, 0xAAAu,
  };
  for (std::size_t c = 0; c < 16u; ++c) {
    const std::uint16_t grb =
        use_default_pc98_pal
            ? kDefPc98[c]
            : static_cast<std::uint16_t>(((c * 0x123u) + 0x246u) & 0x0FFFu);
    put_le16(16u + c * 2u, grb);
  }

  // Row 0: literal word0 (flag=0), horizontal repeat cx=words_per_row-1 (flag=1)
  // Rows 1..h-1: literal word1 on col 0 (flag=0), vertical repeat from row above
  // (flag=1, m=0x80 | (cx-1)) when words_per_row >= 2!
  const int words_per_row = w / 4;
  std::uint8_t cur_flags = 0u;
  int bit_idx = 0;
  std::size_t flag_pos = 0u;
  auto emit_flag = [&](bool cf) {
    if (bit_idx == 0) {
      flag_pos = out.size();
      out.push_back(0u);
      cur_flags = 0u;
    }
    if (cf) {
      cur_flags = static_cast<std::uint8_t>(cur_flags | (1u << bit_idx));
    }
    out[flag_pos] = cur_flags;
    bit_idx = (bit_idx + 1) & 7;
  };
  auto emit_u16 = [&](std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
  };

  for (int y = 0; y < h; ++y) {
    emit_flag(false);
    emit_u16((y == 0) ? lit_word0 : lit_word1);
    if (words_per_row > 1) {
      emit_flag(true);
      const auto rem = static_cast<std::uint8_t>(
          std::min(words_per_row - 1, 32) - 1);
      // Row 0 copies horizontally (bit7=0, step=1); later rows copy vertically
      // from previous row (bit7=1, step=1 -> 200 words back).
      out.push_back(static_cast<std::uint8_t>((y == 0 ? 0x00u : 0x80u) | rem));
    }
  }
  return out;
}

void test_runtime_cli_parsing_discovery_and_execution() {
  ScopedEnvVar no_dlg("SAN4_NO_ERROR_DIALOG", "1");
  ScopedEnvVar clr_game("SAN4_GAME_DIR", nullptr);
  ScopedEnvVar vid_drv("SDL_VIDEODRIVER", "dummy");
  ScopedEnvVar aud_drv("SDL_AUDIODRIVER", "dummy");

  // 1) Help & valid flag parsing (both --flag val and --flag=val forms).
  {
    san4::ParsedRuntimeCli parsed;
    std::ostringstream out, err;
    CHECK(san4::parse_runtime_cli({"san4-linux", "--help"}, "san4-linux",
                                  parsed, out, err) == 0);
    CHECK(parsed.show_help);
    CHECK(out.str().find("--headless-script") != std::string::npos);

    out.str("");
    CHECK(san4::parse_runtime_cli({"san4-linux", "-h"}, "san4-linux", parsed,
                                  out, err) == 0);
    CHECK(parsed.show_help);

    const std::vector<std::string> full_args = {
        "san4-linux",
        "--game-dir", "/tmp/g",
        "--save-dir=/tmp/s",
        "--scale", "xbr2x",
        "--window-scale=3",
        "--speed", "4",
        "--hd-pack=/tmp/hd",
        "--dump-sprites", "/tmp/dump",
        "--skip-intro",
        "--fullscreen",
        "--windowed",
        "--no-mouse-warp",
        "--headless-script=frames:1;quit",
        "--deterministic",
        "--renderer=cpu",
        "--profile-frames", "/tmp/prof.tsv",
        "--threads=8",
    };
    CHECK(san4::parse_runtime_cli(full_args, "san4-linux", parsed, out, err) ==
          0);
    CHECK(!parsed.show_help);
    CHECK(parsed.cli_game_dir == "/tmp/g");
    CHECK(parsed.explicit_scale);
    CHECK(parsed.opts.upscale_mode == san4::UpscaleMode::Xbr2x);
    CHECK(parsed.opts.window_scale == 3);
    CHECK(parsed.opts.speed_multiplier == 4);
    CHECK(!parsed.opts.fullscreen);
    CHECK(!parsed.opts.mouse_warp);
    CHECK(parsed.opts.skip_intro);
    CHECK(parsed.opts.deterministic);
    CHECK(parsed.opts.renderer == san4::RendererPreference::Cpu);
    CHECK(parsed.opts.threads == 8u);

    // --no-hd-pack clears any earlier --hd-pack.
    CHECK(san4::parse_runtime_cli(
              {"san4-linux", "--hd-pack=/tmp/hd", "--no-hd-pack"}, "san4-linux",
              parsed, out, err) == 0);
    CHECK(parsed.no_hd_pack && parsed.opts.hd_pack_dir.empty());
  }

  // 2) Exhaustive error branches in parse_runtime_cli.
  {
    static const std::vector<std::vector<std::string>> kBadArgs = {
        {"san4", "--game-dir"},
        {"san4", "--game-dir="},
        {"san4", "--save-dir"},
        {"san4", "--scale"},
        {"san4", "--scale=bogus"},
        {"san4", "--window-scale"},
        {"san4", "--window-scale=0"},
        {"san4", "--window-scale=9"},
        {"san4", "--speed"},
        {"san4", "--speed=3"},
        {"san4", "--hd-pack"},
        {"san4", "--dump-sprites"},
        {"san4", "--headless-script"},
        {"san4", "--deterministic"},
        {"san4", "--renderer"},
        {"san4", "--renderer=vulkan"},
        {"san4", "--profile-frames"},
        {"san4", "--threads"},
        {"san4", "--threads=-1"},
        {"san4", "--threads=65"},
        {"san4", "--unknown-flag"},
    };
    for (const auto& bad : kBadArgs) {
      san4::ParsedRuntimeCli parsed;
      std::ostringstream out, err;
      CHECK(san4::parse_runtime_cli(bad, "san4", parsed, out, err) == 2);
      CHECK(!err.str().empty());
    }
  }

  // 3) BYOG manifest verification failure formatting (>8 errors, wrong size,
  //    corrupt SHA-256, san4_game_dir.txt hint file, SAN4_GAME_DIR env).
  {
    ScopedTempDir tmp;
    const auto bad_game = tmp.path() / "bad_san4";
    std::filesystem::create_directories(bad_game);
    // Write corrupt EDMUSIC.S4 (exact 8706 size, wrong SHA-256) and wrong-sized
    // mutable NBDATA.S4 (100 != 17496).
    const std::vector<std::uint8_t> zeros_ed(8706u, 0xAAu);
    const std::vector<std::uint8_t> short_nb(100u, 0x00u);
    write_binary_file_checked(bad_game / "EDMUSIC.S4", zeros_ed);
    write_binary_file_checked(bad_game / "NBDATA.S4", short_nb);
    write_binary_file_checked(bad_game / "SAN486.COM", short_nb);

    const auto bin_dir = tmp.path() / "bin";
    std::filesystem::create_directories(bin_dir);
    {
      std::ofstream hf(bin_dir / "san4_game_dir.txt", std::ios::binary);
      hf << san4::util::path_to_utf8(bad_game) << "  \r\n";
    }

    std::ostringstream out, err;
    const int rc = san4::run_runtime_cli(
        {"san4-linux"}, "san4-linux", bin_dir / "san4-linux", out, err, true);
    CHECK(rc == 1);
    CHECK(err.str().find("SHA-256 mismatch on EDMUSIC.S4") != std::string::npos);
    CHECK(err.str().find("more issue(s)") != std::string::npos);

    // No game directory anywhere -> clean error message (hermetic wrt cwd & env).
    ScopedTempDir empty_root;
    const auto isolated_cwd = empty_root.path() / "sub" / "cwd";
    std::filesystem::create_directories(isolated_cwd);
    ScopedCurrentDir cwd_guard(isolated_cwd);
    ScopedEnvVar no_game_env("SAN4_GAME_DIR", nullptr);
    ScopedEnvVar no_xdg("XDG_DATA_HOME",
                        san4::util::path_to_utf8(empty_root.path() / "no_xdg").c_str());
    ScopedEnvVar no_home("HOME",
                         san4::util::path_to_utf8(empty_root.path() / "no_home").c_str());
    ScopedEnvVar no_local_app(
        "LOCALAPPDATA",
        san4::util::path_to_utf8(empty_root.path() / "no_local").c_str());
    ScopedEnvVar no_appdata(
        "APPDATA",
        san4::util::path_to_utf8(empty_root.path() / "no_roam").c_str());
    const auto check_none = san4::resolve_and_verify_game_dir(
        "", isolated_cwd / "bin" / "san4-linux");
    CHECK(!check_none.ok);
  }

  // 4) Full synthetic runtime execution via run_runtime_cli (both headless &
  //    interactive auto-discovery of hd_pack + read-only save_dir fallback).
  {
    ScopedTempDir tmp;
    const auto app_root = tmp.path() / "app";
    const auto exe_dir = app_root / "bin";
    const auto game_dir = app_root / "SAN4";
    const auto hd_dir = app_root / "hd_pack";
    const auto xdg_dir = tmp.path() / "xdg_data";
    std::filesystem::create_directories(exe_dir);
    std::filesystem::create_directories(game_dir);
    std::filesystem::create_directories(hd_dir);
    std::filesystem::create_directories(xdg_dir);

    // Minimal DOS .COM stub: `mov ax, 0x4C00; int 0x21`
    static constexpr std::uint8_t kExitCom[] = {0xB8, 0x00, 0x4C, 0xCD, 0x21};
    write_binary_file_checked(game_dir / "SAN486.COM", kExitCom);
    // Read-only SAVEDAT.S4 so is_game_dir_writable returns false and triggers
    // select_user_save_dir()!
    write_binary_file_checked(game_dir / "SAVEDAT.S4", kExitCom);
    std::error_code perm_ec;
    std::filesystem::permissions(game_dir / "SAVEDAT.S4",
                                 std::filesystem::perms::owner_read,
                                 std::filesystem::perm_options::replace,
                                 perm_ec);

    {
      std::ofstream mf(hd_dir / "hd_manifest.tsv", std::ios::binary);
      mf << "# synthetic manifest\n";
    }

    ScopedEnvVar xdg_env("XDG_DATA_HOME",
                         san4::util::path_to_utf8(xdg_dir).c_str());
    ScopedEnvVar local_app_env("LOCALAPPDATA",
                               san4::util::path_to_utf8(xdg_dir).c_str());

    // Headless execution
    std::ostringstream out, err;
    CHECK(san4::run_runtime_cli(
              {"san4-linux",
               "--game-dir=" + san4::util::path_to_utf8(game_dir),
               "--headless-script=frames:1;quit"},
              "san4-linux", exe_dir / "san4-linux", out, err, false) == 0);

    // Interactive execution (parent hd_pack discovery + read-only save fallback)
    out.str("");
    err.str("");
    CHECK(san4::run_runtime_cli(
              {"san4-linux",
               "--game-dir=" + san4::util::path_to_utf8(game_dir)},
              "san4-linux", exe_dir / "san4-linux", out, err, false) == 0);

    std::filesystem::permissions(
        game_dir / "SAVEDAT.S4",
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::add, perm_ec);
  }
}

void test_hdpack_generator_cli_and_synthetic_pipeline() {
  // 1) Compressed LS11 varint + LZSS back-reference & corrupt stream checks.
  {
    std::vector<std::uint8_t> comp_bits;
    int bit_used = 0;
    auto push_bit = [&](std::uint32_t b) {
      if (bit_used == 0) {
        comp_bits.push_back(0u);
      }
      if (b != 0u) {
        comp_bits.back() = static_cast<std::uint8_t>(
            comp_bits.back() | (1u << (7 - bit_used)));
      }
      bit_used = (bit_used + 1) & 7;
    };
    auto push_varint = [&](std::uint32_t val) {
      unsigned cx = 1u;
      while (true) {
        const std::uint32_t base = (1u << cx) - 2u;
        const std::uint32_t span = (1u << cx);
        if (val >= base && val - base < span) {
          for (unsigned i = 0; i + 1u < cx; ++i) push_bit(1u);
          push_bit(0u);
          const std::uint32_t di = val - base;
          for (unsigned i = 0; i < cx; ++i) {
            push_bit((di >> (cx - 1u - i)) & 1u);
          }
          break;
        }
        ++cx;
      }
    };
    // Emit 4 literal bytes (0, 1, 2, 3) then LZSS copy dist=4 (code=260),
    // len=4 (len_code=1 -> 1+3=4) => 8 uncompressed bytes {0,1,2,3,0,1,2,3}.
    push_varint(0u);
    push_varint(1u);
    push_varint(2u);
    push_varint(3u);
    push_varint(260u);
    push_varint(1u);

    std::vector<std::uint8_t> ls11(0x0110u + 16u, 0u);
    ls11[0] = 'L';
    ls11[1] = 'S';
    ls11[2] = '1';
    ls11[3] = '1';
    for (int i = 0; i < 256; ++i) {
      ls11[0x0010u + static_cast<std::size_t>(i)] =
          static_cast<std::uint8_t>(0xA0u + (i & 0x0Fu));
    }
    auto put_be32 = [&](std::size_t off, std::uint32_t v) {
      ls11[off + 0u] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
      ls11[off + 1u] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
      ls11[off + 2u] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
      ls11[off + 3u] = static_cast<std::uint8_t>(v & 0xFFu);
    };
    const auto file_off = static_cast<std::uint32_t>(ls11.size());
    put_be32(0x0110u, static_cast<std::uint32_t>(comp_bits.size()));
    put_be32(0x0114u, 8u);
    put_be32(0x0118u, file_off);
    ls11.insert(ls11.end(), comp_bits.begin(), comp_bits.end());

    std::vector<std::vector<std::uint8_t>> recs;
    CHECK(san4::codecs::decompress_ls11_archive(ls11, recs));
    CHECK(recs.size() == 1u && recs[0].size() == 8u);
    CHECK(recs[0][0] == 0xA0u && recs[0][3] == 0xA3u &&
          recs[0][4] == 0xA0u && recs[0][7] == 0xA3u);

    // Corrupt varint (>24 leading 1s) must fail cleanly.
    std::fill(ls11.begin() + file_off, ls11.end(), 0xFFu);
    ls11.resize(file_off + 6u, 0xFFu);
    put_be32(0x0110u, 6u);
    CHECK(!san4::codecs::decompress_ls11_archive(ls11, recs));
  }

  // 2) CLI validation & missing archive error handling.
  {
    std::ostringstream out, err;
    CHECK(san4::hd_gen::run_hdpack_gen_cli({"gen", "--help"}, "gen", {}, out,
                                           err, true) == 0);
    CHECK(out.str().find("--out-dir") != std::string::npos);

    static const std::vector<std::vector<std::string>> kBadCli = {
        {"gen"},
        {"gen", "--game-dir"},
        {"gen", "--game-dir="},
        {"gen", "--out-dir"},
        {"gen", "--out-dir="},
        {"gen", "--jobs"},
        {"gen", "--jobs=0", "--out-dir=/tmp/x"},
        {"gen", "--jobs=129", "--out-dir=/tmp/x"},
        {"gen", "--unknown"},
    };
    for (const auto& bad : kBadCli) {
      out.str("");
      err.str("");
      CHECK(san4::hd_gen::run_hdpack_gen_cli(bad, "gen", {}, out, err, true) ==
            2);
    }

    ScopedTempDir tmp;
    err.str("");
    CHECK(san4::hd_gen::run_hdpack_gen_cli(
              {"gen", "--out-dir=" + san4::util::path_to_utf8(tmp.path() / "o")},
              "gen", {}, out, err, false) == 2);
    CHECK(san4::hd_gen::run_hdpack_gen_cli(
              {"gen", "--game-dir=" + san4::util::path_to_utf8(tmp.path()),
               "--out-dir=" + san4::util::path_to_utf8(tmp.path() / "o")},
              "gen", {}, out, err, true) == 1);
    CHECK(san4::hd_gen::generate_hd_pack_from_dir(
              tmp.path(), tmp.path() / "o", 2, out, err) == 1);
  }

  // 3) Full 15-stage multi-threaded synthetic HD pack generation & round-trip
  //    HdPackManager loading!
  ScopedTempDir tmp;
  const auto synth_game = tmp.path() / "synth_san4";
  const auto out_pack = tmp.path() / "out_hd_pack";
  std::filesystem::create_directories(synth_game);

  // 3a) Synthetic MAIN.EXE (0x4A160 bytes: 4-bit BRG palettes + eye/mouth Y).
  {
    std::vector<std::uint8_t> main_exe(0x4A160u, 0u);
    for (std::size_t i = 0; i < 48u; ++i) {
      main_exe[0x47732u + i] = static_cast<std::uint8_t>((i * 3u + 2u) & 0x0Fu);
    }
    for (std::size_t i = 0; i < 24u; ++i) {
      main_exe[0x478C2u + i] = static_cast<std::uint8_t>((i + 4u) & 0x0Fu);
      main_exe[0x47922u + i] = static_cast<std::uint8_t>((i + 6u) & 0x0Fu);
      main_exe[0x39C24u + i] = static_cast<std::uint8_t>((i + 1u) & 0x0Fu);
    }
    for (std::size_t i = 0; i < 16u; ++i) {
      main_exe[0x4A13Au + i] = 20u;
      main_exe[0x4A14Au + i] = 44u;
    }
    write_binary_file_checked(synth_game / "MAIN.EXE", main_exe);
  }

  // 3b) Synthetic kaodata.s4 (lowercase filename tests find_case_insensitive_file!).
  //     2 portraits (8x8) exercising 0xFE 0x01 literal, 0xFE N repeat, 0xFE 0x00
  //     256-repeat, copy_up=false/true, nib<8, 8<=nib<12, and nib>=12.
  {
    std::vector<std::uint8_t> kao(0x07F8u, 0u);
    auto append_kao = [&](std::size_t idx, std::span<const std::uint8_t> stream) {
      const std::size_t rel = kao.size() - 0x07F8u;
      std::uint8_t* ent = kao.data() + idx * 6u;
      ent[0] = static_cast<std::uint8_t>(rel & 0xFFu);
      ent[1] = static_cast<std::uint8_t>((rel >> 8) & 0xFFu);
      ent[2] = static_cast<std::uint8_t>((rel >> 16) & 0xFFu);
      const auto clen = static_cast<std::uint16_t>(4u + stream.size());
      ent[4] = static_cast<std::uint8_t>(clen & 0xFFu);
      ent[5] = static_cast<std::uint8_t>((clen >> 8) & 0xFFu);
      kao.push_back(8u);
      kao.push_back(0u);
      kao.push_back(8u);
      kao.push_back(0u);
      kao.insert(kao.end(), stream.begin(), stream.end());
    };
    static constexpr std::uint8_t kKaoStream0[] = {
        0x01, 0x23, 0x89, 0xFE, 0x01, 0xC2, 0xFE, 0x04, 0x95, 0xFE, 0x00,
    };
    static constexpr std::uint8_t kKaoStream1[] = {
        0x82, 0x45, 0xA3, 0xD1, 0x16, 0xFE, 0x03,
    };
    append_kao(0u, kKaoStream0);
    append_kao(1u, kKaoStream1);
    write_binary_file_checked(synth_game / "kaodata.s4", kao);
  }

  // 3c) Synthetic MONDATA.S4 (16 templates x 0x2E80 bytes = 190,464 bytes).
  {
    std::vector<std::uint8_t> mon(16u * 0x2E80u, 0u);
    for (std::size_t i = 0; i < mon.size(); ++i) {
      mon[i] = static_cast<std::uint8_t>((i * 13u + (i >> 9)) & 0xFFu);
    }
    write_binary_file_checked(synth_game / "MONDATA.S4", mon);
  }

  // 3d) 10 NPK containers (including OPENDAT.S4 48-byte scene palette block).
  {
    static constexpr std::array<std::string_view, 10> kNpkNames = {
        "OPENDAT.S4", "ENDDAT.S4",  "PACKGRP.S4", "PACKGRP2.S4", "PACKGRP3.S4",
        "HIBACK.S4",  "LOGO.S4",    "REVGRP.S4",  "HEGRP.S4",    "PREGRP.S4",
    };
    for (std::size_t idx = 0; idx < kNpkNames.size(); ++idx) {
      std::vector<std::uint8_t> container;
      if (kNpkNames[idx] == "HEGRP.S4") {
        // HEGRP.S4 has sprite sheet in [0..0x225F] before NPK images!
        container.resize(0x2260u, 0u);
        for (std::size_t b = 0; b < container.size(); ++b) {
          container[b] = static_cast<std::uint8_t>((b * 7u + 3u) & 0xFFu);
        }
      } else if (kNpkNames[idx] == "OPENDAT.S4") {
        // Prepend a valid 48-byte [B,R,G] scene palette block (values in 0..15).
        for (std::size_t b = 0; b < 48u; ++b) {
          container.push_back(static_cast<std::uint8_t>((b % 14u) + 1u));
        }
      }
      const bool use_npk16 = (idx % 2u == 1u);
      const bool def_pal = (kNpkNames[idx] != "PACKGRP.S4");
      const auto npk = build_synthetic_npk_image(
          use_npk16, def_pal, 8, 4,
          static_cast<std::uint16_t>(0x1234u + idx * 0x111u),
          static_cast<std::uint16_t>(0x5678u + idx * 0x222u));
      container.insert(container.end(), npk.begin(), npk.end());
      write_binary_file_checked(synth_game / kNpkNames[idx], container);
    }
  }

  // Helper to fill planar tile bytes with both opaque and masked (game_color=0)
  // 16x16 patterns while keeping duplicate hashes low for fast test execution.
  auto make_planar_buf = [](std::size_t len, std::uint8_t seed) {
    std::vector<std::uint8_t> buf(len, 0u);
    for (std::size_t off = 0; off + 128u <= len; off += 128u) {
      const bool masked_variant = (((off / 128u) & 1u) != 0u);
      for (std::size_t b = 0; b < 128u; ++b) {
        buf[off + b] = masked_variant
                           ? static_cast<std::uint8_t>((b & 1u) ? (seed | 0x55u) : 0x00u)
                           : static_cast<std::uint8_t>(seed | 0xFFu);
      }
    }
    return buf;
  };

  // 3e) LS11 tile & sprite archives: MAINCG, TOSHICG, HEXGRP, HKGRP, HUNIT, HIGRP, GRAPH, HWGRP.
  write_binary_file_checked(
      synth_game / "MAINCG.S4",
      build_synthetic_ls11({make_planar_buf(0xFF40u, 0x10u)}));
  write_binary_file_checked(
      synth_game / "TOSHICG.S4",
      build_synthetic_ls11({make_planar_buf(195u * 128u, 0x20u)}));
  write_binary_file_checked(
      synth_game / "HEXGRP.S4",
      build_synthetic_ls11({make_planar_buf(156u * 128u, 0x30u)}));
  {
    std::vector<std::vector<std::uint8_t>> hk_recs(8u, make_planar_buf(128u, 0x40u));
    write_binary_file_checked(synth_game / "HKGRP.S4",
                              build_synthetic_ls11(hk_recs));
  }
  {
    std::vector<std::vector<std::uint8_t>> hu_recs = {
        make_planar_buf(128u, 0x50u),
        make_planar_buf(128u, 0x54u),
        make_planar_buf(0x4440u, 0x58u),
    };
    write_binary_file_checked(synth_game / "HUNIT.S4",
                              build_synthetic_ls11(hu_recs));
  }
  {
    std::vector<std::vector<std::uint8_t>> hi_recs(14u);
    for (std::size_t i = 0; i < 13u; ++i) {
      hi_recs[i] = make_planar_buf(4608u, 0x60u);
    }
    hi_recs[13] = make_planar_buf(0x2000u, 0x64u);
    write_binary_file_checked(synth_game / "HIGRP.S4",
                              build_synthetic_ls11(hi_recs));
  }
  {
    auto graph_r0 = make_planar_buf(0x5A34u, 0x70u);
    for (std::size_t i = 0; i < graph_r0.size(); ++i) {
      graph_r0[i] = static_cast<std::uint8_t>((i * 19u + 5u) & 0xFFu);
    }
    write_binary_file_checked(synth_game / "GRAPH.S4",
                              build_synthetic_ls11({graph_r0}));
  }
  write_binary_file_checked(synth_game / "HWGRP.S4",
                            make_planar_buf(0x3E00u, 0x82u));

  // Execute full generator pipeline across 16 worker threads!
  std::ostringstream gen_out, gen_err;
  const int gen_rc = san4::hd_gen::run_hdpack_gen_cli(
      {"san4-hdpack-gen",
       "--game-dir=" + san4::util::path_to_utf8(synth_game),
       "--out-dir=" + san4::util::path_to_utf8(out_pack),
       "--jobs=16"},
      "san4-hdpack-gen", {}, gen_out, gen_err, false);
  CHECK(gen_rc == 0);
  CHECK(std::filesystem::is_regular_file(out_pack / "hd_manifest.tsv"));
  CHECK(std::filesystem::file_size(out_pack / "mondata" / "mondata_index.bin") ==
        8192u * sizeof(san4::MondataIndexEntry));
  CHECK(std::filesystem::file_size(out_pack / "sprites" / "buttons" /
                                   "button_index.bin") ==
        1407u * sizeof(san4::ButtonIndexEntry));
  CHECK(std::filesystem::file_size(out_pack / "tiles" / "tile_dict.bin") > 0u);

  // Round-trip: load generated pack into HdPackManager and verify assets index cleanly!
  san4::HdPackManager hd;
  hd.set_pack_dir(out_pack);
  CHECK(hd.loaded_texture_count() > 30u);
}

void test_sdl_host_headless_script_and_profiler_end_to_end() {
  ScopedEnvVar vid_drv("SDL_VIDEODRIVER", "dummy");
  ScopedEnvVar aud_drv("SDL_AUDIODRIVER", "dummy");

  ScopedTempDir tmp;
  const auto game_dir = tmp.path() / "game";
  const auto hd_dir = tmp.path() / "hd";
  std::filesystem::create_directories(game_dir);
  std::filesystem::create_directories(hd_dir);
  {
    std::ofstream mf(hd_dir / "hd_manifest.tsv", std::ios::binary);
    mf << "# empty\n";
  }

  // Synthetic 16-bit SAN486.COM:
  //   mov ax, 0x4F02; mov bx, 0x0102; int 0x10          ; set VESA 800x600
  //   mov ax, 0xA000; mov es, ax; mov byte [es:0], 0xFF ; dirty VRAM
  //   mov dx, 0x0388; mov al, 0x20; out dx, al          ; AdLib reg write
  //   inc dx; mov al, 0x01; out dx, al
  //   mov ah, 0x00; int 0xF1                            ; GRP600C census call
  // loop_poll:
  //   mov ax, 0x0003; int 0x33                          ; mouse status poll
  //   mov ah, 0x01; int 0x16                            ; kbhit peek
  //   jz loop_poll
  //   mov ah, 0x00; int 0x16                            ; consume key
  //   cmp al, 'q'; jne loop_poll
  //   mov ax, 0x4C00; int 0x21                          ; exit 0
  static constexpr std::uint8_t kHeadlessGuestCom[] = {
      0xB8, 0x02, 0x4F, 0xBB, 0x02, 0x01, 0xCD, 0x10,
      0xB8, 0x00, 0xA0, 0x8E, 0xC0, 0x26, 0xC6, 0x06, 0x00, 0x00, 0xFF,
      0xBA, 0x88, 0x03, 0xB0, 0x20, 0xEE, 0x42, 0xB0, 0x01, 0xEE,
      0xB4, 0x00, 0xCD, 0xF1,
      0xB8, 0x03, 0x00, 0xCD, 0x33,
      0xB4, 0x01, 0xCD, 0x16,
      0x74, 0xF5,
      0xB4, 0x00, 0xCD, 0x16,
      0x3C, 'q', 0x75, 0xED,
      0xB8, 0x00, 0x4C, 0xCD, 0x21,
  };
  write_binary_file_checked(game_dir / "SAN486.COM", kHeadlessGuestCom);

  const auto shot_png = tmp.path() / "shot.png";
  const auto prof_tsv = tmp.path() / "profile.tsv";

  san4::HostOptions opts;
  opts.game_dir = game_dir;
  opts.hd_pack_dir = hd_dir;
  opts.upscale_mode = san4::UpscaleMode::Nearest;
  opts.explicit_scale = true;
  opts.deterministic = true;
  opts.threads = 2;
  opts.profile_frames_path = prof_tsv;
  opts.headless_script =
      "frames:2;wait_idle:4;wait_ms:20;move:120:140;click:150:160;"
      "rclick:170:180;tap:190:200;wheel:2;wheel:-2;shift_wheel:1;"
      "shift_wheel:-1;key:enter;key:esc;key:space;key:up;key:down;"
      "key:left;key:right;key:pgup;key:pgdn;key:1;speed:2;speed:4;speed:1;"
      "scale:sharp2x;scale:scale2x;scale:xbr2x;scale:nearest;"
      "reload_hd;reload_hd:" +
      san4::util::path_to_utf8(hd_dir) + ";screenshot:" +
      san4::util::path_to_utf8(shot_png) +
      ";reset_audio_peak;stats;key:q;quit";

  {
    san4::SdlHost host(opts);
    CHECK(host.run() == 0);
  }
  int sw = 0;
  int sh = 0;
  std::vector<std::uint32_t> spx;
  CHECK(san4::HdPackManager::load_argb_png(shot_png, sw, sh, spx));
  CHECK(sw == 800 && sh == 600);
  CHECK(std::filesystem::file_size(prof_tsv) > 64u);

  // Invalid headless script commands must return 2 cleanly.
  opts.profile_frames_path.clear();
  opts.headless_script = "bogus_op:123";
  {
    san4::SdlHost bad_host(opts);
    CHECK(bad_host.run() == 2);

    bool running = true;
    bool force_full = false;
    static const std::vector<std::vector<std::string>> kBadCmds = {
        {"frames"},
        {"frames", "0"},
        {"wait_idle", "0"},
        {"wait_idle", "1", "2"},
        {"wait_ms"},
        {"wait_ms", "-1"},
        {"move", "10"},
        {"click", "bad", "10"},
        {"tap", "10"},
        {"wheel"},
        {"key"},
        {"key", "invalidkey"},
        {"speed", "3"},
        {"scale", "badmode"},
        {"screenshot"},
        {"reset_audio_peak", "extra"},
        {"stats", "extra"},
        {"quit", "extra"},
    };
    for (const auto& cmd : kBadCmds) {
      CHECK(!san4::SdlHostTestPeer::execute_headless_command(
          bad_host, cmd, running, force_full));
    }
  }
}

void test_sdl_host_interactive_events_audio_and_present_slot() {
  ScopedEnvVar vid_drv("SDL_VIDEODRIVER", "dummy");
  ScopedEnvVar aud_drv("SDL_AUDIODRIVER", "dummy");

  ScopedTempDir tmp;
  const auto game_dir = tmp.path() / "game";
  const auto dump_dir = tmp.path() / "dump";
  const auto prof_tsv = tmp.path() / "interactive_prof.tsv";
  std::filesystem::create_directories(game_dir);

  // 1) Interactive SdlHost::run() end-to-end: guest warps cursor (AX=0004h),
  //    defines 16x16 graphics cursor (AX=0009h), yields 2 BIOS polls so main
  //    thread acquires & presents a FrameMailbox slot, and exits cleanly!
  {
    static constexpr std::uint8_t kInteractiveGuestCom[] = {
        0xB8, 0x02, 0x4F, 0xBB, 0x02, 0x01, 0xCD, 0x10,  // VESA 0x0102
        0xB8, 0x04, 0x00, 0xB9, 0x90, 0x01, 0xBA, 0x2C, 0x01, 0xCD, 0x33,  // warp (400,300)
        0x8C, 0xC8, 0x8E, 0xC0,                          // push cs -> es
        0xB8, 0x09, 0x00, 0xBB, 0x02, 0x00, 0xB9, 0x03, 0x00,
        0xBA, 0x40, 0x01, 0xCD, 0x33,                    // cursor mask at CS:0140
        0xBF, 0x06, 0x00,                                // 6 idle polls (~3 frames)
        0xB8, 0x03, 0x00, 0xCD, 0x33,
        0x4F, 0x75, 0xF8,
        0xB8, 0x00, 0x4C, 0xCD, 0x21,                    // exit 0
    };
    std::vector<std::uint8_t> com_buf(0x0100u, 0x90u);
    std::memcpy(com_buf.data(), kInteractiveGuestCom,
                sizeof(kInteractiveGuestCom));
    for (std::size_t i = 0; i < 32u; ++i) {
      com_buf[0x40u + i] = 0x00u;         // AND mask = 0 (opaque)
      com_buf[0x60u + i] = (i & 1u) ? 0xFFu : 0xAAu;  // XOR mask
    }
    write_binary_file_checked(game_dir / "SAN486.COM", com_buf);

    san4::HostOptions opts;
    opts.game_dir = game_dir;
    opts.upscale_mode = san4::UpscaleMode::Nearest;
    opts.explicit_scale = true;
    opts.window_scale = 1;
    opts.threads = 2;
    opts.profile_frames_path = prof_tsv;
    san4::SdlHost host(opts);
    CHECK(host.run() == 0);
    CHECK(std::filesystem::exists(prof_tsv));
  }

  // 2) Deterministic unit exercise of interactive event dispatch, coordinate
  //    mapping, audio callback, sparse vs full-frame texture upload, and resets.
  {
    const auto hd_dir = tmp.path() / "hd";
    std::filesystem::create_directories(hd_dir);
    san4::HostOptions opts;
    opts.game_dir = game_dir;
    opts.hd_pack_dir = hd_dir;
    opts.dump_sprites_dir = dump_dir;
    opts.window_scale = 2;
    opts.threads = 2;
    san4::SdlHost host(opts);

    // Exercise sdl_audio_callback directly BEFORE init_sdl() spawns SDL's
    // background audio consumer thread (respecting Opl2Audio's SPSC contract!).
    san4::X86Machine& m = san4::SdlHostTestPeer::machine(host);
    m.audio().advance_seconds(0.060);
    std::vector<std::uint8_t> pcm(2300u * 4u + 3u, 0x7Fu);
    san4::SdlHostTestPeer::audio_callback(nullptr, pcm.data(), 64);
    san4::SdlHostTestPeer::audio_callback(&host, nullptr, 64);
    san4::SdlHostTestPeer::audio_callback(&host, pcm.data(), 0);
    san4::SdlHostTestPeer::audio_callback(
        &host, pcm.data(), static_cast<int>(pcm.size()));
    CHECK(pcm.back() == 0u);  // trailing odd bytes zero-filled!

    CHECK(san4::SdlHostTestPeer::init_sdl(host));

    // Coordinate helpers (including degenerate zero-dimension fallbacks).
    const auto clamped_big =
        san4::SdlHostTestPeer::clamp_initial_window_size(3200, 2400, 1920, 1080);
    CHECK(clamped_big.w <= 1920 && clamped_big.h <= 1080);
    const auto clamped_zero =
        san4::SdlHostTestPeer::clamp_initial_window_size(1600, 1200, 0, 0);
    CHECK(clamped_zero.w == 1600 && clamped_zero.h == 1200);

    int dx = 0, dy = 0, wx = 0, wy = 0;
    san4::SdlHostTestPeer::map_window_to_dos(400, 300, 0, 0, 0, 0, 800, 600, dx,
                                             dy);
    CHECK(dx == 400 && dy == 300);
    san4::SdlHostTestPeer::map_dos_to_window(400, 300, 0, 0, 0, 0, 800, 600, wx,
                                             wy);
    CHECK(wx >= 399 && wx <= 401 && wy >= 299 && wy <= 301);
    san4::SdlHostTestPeer::dos_to_window_coords(host, 200, 150, 800, 600, wx, wy);
    san4::SdlHostTestPeer::window_to_dos_coords(host, wx, wy, dx, dy);
    CHECK(std::abs(dx - 200) <= 2 && std::abs(dy - 150) <= 2);

    // Upload rects: sparse (<60%) vs dense (>60%) full-frame upload + present_slot.
    std::vector<std::uint32_t> argb(1600u * 1200u, 0xFF112233u);
    const san4::UpscaleDirtyRect small_r{
        .has_changes = true, .x = 0, .y = 0, .w = 32, .h = 32};
    CHECK(san4::SdlHostTestPeer::upload_rects(
              host, argb.data(),
              std::span<const san4::UpscaleDirtyRect>(&small_r, 1u)) ==
          32u * 32u);
    const san4::UpscaleDirtyRect big_r{
        .has_changes = true, .x = 0, .y = 0, .w = 1600, .h = 900};
    CHECK(san4::SdlHostTestPeer::upload_rects(
              host, argb.data(),
              std::span<const san4::UpscaleDirtyRect>(&big_r, 1u)) ==
          1600u * 1200u);

    san4::FrameMailbox::Slot slot;
    slot.gen = 1u;
    slot.w = 800;
    slot.h = 600;
    slot.mode = san4::UpscaleMode::Nearest;
    slot.argb.assign(800u * 600u, 0xFF334455u);
    slot.status.mode = san4::UpscaleMode::Nearest;
    slot.status.speed_multiplier = 2;
    slot.status.hd_loaded = 5u;
    slot.status.hd_dumping = true;
    slot.tile_gen.fill(1u);
    CHECK(san4::SdlHostTestPeer::present_slot(host, slot));

    // Push synthetic SDL events covering hotkeys, keys, mouse, wheel, window,
    // audio hotplug, render resets, and SDL_QUIT!
    auto push_key_ev = [](Uint32 type, SDL_Keycode sym, Uint16 mod = 0) {
      SDL_Event e{};
      e.type = type;
      e.key.keysym.sym = sym;
      e.key.keysym.mod = mod;
      SDL_PushEvent(&e);
    };
    static constexpr SDL_Keycode kTestKeys[] = {
        SDLK_TAB,      SDLK_F4,       SDLK_F5,     SDLK_F8,     SDLK_F9,
        SDLK_F11,      SDLK_w,        SDLK_a,      SDLK_s,      SDLK_d,
        SDLK_RETURN,   SDLK_ESCAPE,   SDLK_SPACE,  SDLK_BACKSPACE,
        SDLK_UP,       SDLK_DOWN,     SDLK_LEFT,   SDLK_RIGHT,  SDLK_PAGEUP,
        SDLK_PAGEDOWN, SDLK_HOME,     SDLK_END,    SDLK_F1,     SDLK_F2,
        SDLK_F3,       SDLK_F6,       SDLK_F7,     SDLK_F10,    SDLK_0,
        SDLK_5,        SDLK_MINUS,    SDLK_EQUALS, SDLK_LEFTBRACKET,
        SDLK_RIGHTBRACKET, SDLK_SEMICOLON, SDLK_QUOTE, SDLK_BACKQUOTE,
        SDLK_BACKSLASH, SDLK_COMMA,   SDLK_PERIOD, SDLK_SLASH,
    };
    for (SDL_Keycode k : kTestKeys) {
      push_key_ev(SDL_KEYDOWN, k, 0);
    }
    push_key_ev(SDL_KEYDOWN, SDLK_RETURN, KMOD_ALT);
    for (char ch = 'a'; ch <= 'z'; ++ch) {
      push_key_ev(SDL_KEYDOWN, static_cast<SDL_Keycode>(ch), KMOD_SHIFT);
    }
    push_key_ev(SDL_KEYUP, SDLK_TAB, 0);

    {
      SDL_Event e{};
      e.type = SDL_MOUSEMOTION;
      e.motion.x = 120;
      e.motion.y = 90;
      SDL_PushEvent(&e);

      for (Uint8 btn : {SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, SDL_BUTTON_MIDDLE}) {
        e = SDL_Event{};
        e.type = SDL_MOUSEBUTTONDOWN;
        e.button.button = btn;
        e.button.x = 100;
        e.button.y = 100;
        SDL_PushEvent(&e);
        e.type = SDL_MOUSEBUTTONUP;
        SDL_PushEvent(&e);
      }

      e = SDL_Event{};
      e.type = SDL_MOUSEWHEEL;
      e.wheel.y = 2;
      e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
      SDL_PushEvent(&e);
      e.wheel.y = 2;
      e.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
      SDL_PushEvent(&e);

      e = SDL_Event{};
      e.type = SDL_WINDOWEVENT;
      e.window.event = SDL_WINDOWEVENT_RESIZED;
      SDL_PushEvent(&e);
      e.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
      SDL_PushEvent(&e);

      if (const auto adev = san4::SdlHostTestPeer::audio_dev(host); adev != 0u) {
        e = SDL_Event{};
        e.type = SDL_AUDIODEVICEREMOVED;
        e.adevice.iscapture = 0;
        e.adevice.which = adev;
        SDL_PushEvent(&e);

        e = SDL_Event{};
        e.type = SDL_AUDIODEVICEADDED;
        e.adevice.iscapture = 0;
        SDL_PushEvent(&e);
      }

      e = SDL_Event{};
      e.type = SDL_RENDER_TARGETS_RESET;
      SDL_PushEvent(&e);
      e.type = SDL_RENDER_DEVICE_RESET;
      SDL_PushEvent(&e);

      e = SDL_Event{};
      e.type = SDL_QUIT;
      SDL_PushEvent(&e);
    }

    bool running = true;
    san4::SdlHostTestPeer::process_events(host, running);
    CHECK(!running);
    CHECK(san4::SdlHostTestPeer::recreate_renderer_after_reset(host));
  }
}

void test_audio_pit_ch2_speaker_opl3_and_underrun_decay() {
  san4::Opl2Audio audio;
  CHECK(audio.handles_port(0x0388u) && audio.handles_port(0x0222u) &&
        audio.handles_port(0x0042u) && audio.handles_port(0x0061u) &&
        !audio.handles_port(0x03DAu));

  // 1) PIT channel 2 control modes (non-ch2 ignored, rl=1 LSB, rl=2 MSB, rl=3 LSB+MSB + latch).
  audio.on_pit_control(0x36u);  // ch0 ignored
  audio.on_pit_control(0x90u);  // ch2, rl=1 (LSB only)
  audio.port_out8(0x0042u, 0x00u);  // 0 -> 65536
  CHECK(audio.port_in8(0x0042u) == 0x00u);
  audio.port_out8(0x0042u, 0x34u);
  CHECK(audio.port_in8(0x0042u) == 0x34u);

  audio.on_pit_control(0xA0u);  // ch2, rl=2 (MSB only)
  audio.port_out8(0x0042u, 0x00u);
  CHECK(audio.port_in8(0x0042u) == 0x00u);
  audio.port_out8(0x0042u, 0x12u);
  CHECK(audio.port_in8(0x0042u) == 0x12u);

  audio.on_pit_control(0xB6u);  // ch2, rl=3 (LSB then MSB)
  audio.port_out8(0x0042u, 0x78u);
  audio.port_out8(0x0042u, 0x56u);
  audio.on_pit_control(0x80u);  // latch 0x5678
  audio.on_pit_control(0x80u);  // second latch while latched is ignored
  CHECK(audio.port_in8(0x0042u) == 0x78u);
  CHECK(audio.port_in8(0x0042u) == 0x56u);

  // 2) Port 0x61 refresh toggle + PC speaker soft-envelope ramp up & down.
  audio.port_out8(0x0061u, 0x03u);
  const std::uint8_t p61_a = audio.port_in8(0x0061u);
  const std::uint8_t p61_b = audio.port_in8(0x0061u);
  CHECK(((p61_a ^ p61_b) & 0x10u) == 0x10u);
  audio.reset_peak_abs_sample();
  audio.advance_seconds(0.015);
  audio.port_out8(0x0061u, 0x00u);
  audio.advance_seconds(0.010);
  CHECK(audio.peak_abs_sample() > 0);

  // 3) Sound Blaster mirror ports (0x220..0x223), Timer 2 expiry/mask/reset, & OPL3 stereo.
  audio.port_out8(0x0220u, 0x04u);
  audio.port_out8(0x0221u, 0x80u);  // IRQ reset
  CHECK(audio.port_in8(0x0220u) == 0x00u);

  // Masked Timer 1 & Timer 2 must not assert expiry bits.
  audio.port_out8(0x0220u, 0x02u);
  audio.port_out8(0x0221u, 0xFFu);
  audio.port_out8(0x0220u, 0x03u);
  audio.port_out8(0x0221u, 0xFFu);
  audio.port_out8(0x0220u, 0x04u);
  audio.port_out8(0x0221u, 0x63u);  // start T1+T2 with both masked (0x40|0x20)
  audio.advance_seconds(0.005);
  CHECK(audio.port_in8(0x0220u) == 0x00u);

  // Unmasked Timer 2 expiry sets 0xA0 (IRQ | T2); 0x80 resets it.
  audio.port_out8(0x0220u, 0x04u);
  audio.port_out8(0x0221u, 0x00u);
  audio.port_out8(0x0220u, 0x04u);
  audio.port_out8(0x0221u, 0x02u);  // start T2 unmasked
  audio.advance_seconds(0.002);
  CHECK((audio.port_in8(0x0222u) & 0xA0u) == 0xA0u);
  audio.port_out8(0x0388u, 0x04u);
  audio.port_out8(0x0389u, 0x80u);
  CHECK(audio.port_in8(0x0388u) == 0x00u);

  // Enable OPL3 mode via bank 1 register 0x105 = 0x01 and render stereo block.
  audio.port_out8(0x0222u, 0x05u);
  audio.port_out8(0x0223u, 0x01u);
  audio.advance_seconds(0.010);

  // 4) Non-positive / NaN advance guards, ring cap, and underrun smooth decay.
  const std::uint64_t before_frames = audio.total_rendered_frames();
  audio.advance_seconds(0.0);
  audio.advance_seconds(-0.5);
  audio.advance_seconds(std::numeric_limits<double>::quiet_NaN());
  CHECK(audio.total_rendered_frames() == before_frames);

  audio.advance_seconds(0.250);
  audio.advance_seconds(0.250);
  audio.advance_seconds(0.250);
  CHECK(audio.buffered_frames() <= san4::Opl2Audio::kMaxBufferedFrames);

  std::vector<std::int16_t> drain_buf(
      (san4::Opl2Audio::kMaxBufferedFrames + 128u) * 2u, 0);
  const std::uint32_t got = audio.pull_stereo_s16(
      drain_buf.data(), san4::Opl2Audio::kMaxBufferedFrames + 128u);
  CHECK(got <= san4::Opl2Audio::kMaxBufferedFrames);
  CHECK(audio.buffered_frames() == 0u);
}

void test_ems_4_0_complete_subfunctions_and_error_codes() {
  ScopedTempDir tmp;
  san4::MachineConfig cfg;
  cfg.game_dir = tmp.path();
  san4::X86Machine m(cfg);
  x86emu_t* emu = m.emu();
  x86emu_set_seg_register(emu, emu->x86.R_DS_SEL, 0x2000u);
  x86emu_set_seg_register(emu, emu->x86.R_ES_SEL, 0x2100u);

  // AH=40h Status, 41h Page Frame, 42h Page Counts, 46h Version
  emu->x86.R_AH = 0x40u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u);

  emu->x86.R_AH = 0x41u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u && emu->x86.R_BX == 0xD000u);

  emu->x86.R_AH = 0x42u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u && emu->x86.R_DX == 256u &&
        emu->x86.R_BX == 256u);

  emu->x86.R_AH = 0x46u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u && emu->x86.R_AL == 0x40u);

  // AH=43h error cases: 0 pages (0x89), > free pages (0x88), then allocate 2 pages.
  emu->x86.R_AH = 0x43u;
  emu->x86.R_BX = 0u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x89u);

  emu->x86.R_AH = 0x43u;
  emu->x86.R_BX = 300u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x88u);

  emu->x86.R_AH = 0x43u;
  emu->x86.R_BX = 2u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u);
  const std::uint16_t h1 = emu->x86.R_DX;

  // AH=44h errors (AL>=4 -> 0x8B, bad handle -> 0x83, bad page -> 0x8A) & valid map.
  emu->x86.R_AX = 0x4404u;
  emu->x86.R_DX = h1;
  emu->x86.R_BX = 0u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x8Bu);

  emu->x86.R_AX = 0x4400u;
  emu->x86.R_DX = 99u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x83u);

  emu->x86.R_AX = 0x4400u;
  emu->x86.R_DX = h1;
  emu->x86.R_BX = 5u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x8Au);

  emu->x86.R_AX = 0x4400u;
  emu->x86.R_DX = h1;
  emu->x86.R_BX = 0u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u);

  // AH=47h / 48h Save & Restore Page Map (+ duplicate 0x8D, missing 0x8E, bad handle 0x83).
  emu->x86.R_AH = 0x47u;
  emu->x86.R_DX = 99u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x83u);

  emu->x86.R_AH = 0x48u;
  emu->x86.R_DX = h1;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x8Eu);

  emu->x86.R_AH = 0x47u;
  emu->x86.R_DX = h1;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u);

  emu->x86.R_AH = 0x47u;
  emu->x86.R_DX = h1;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x8Du);

  emu->x86.R_AX = 0x4400u;
  emu->x86.R_DX = h1;
  emu->x86.R_BX = 0xFFFFu;  // unmap slot 0
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u);

  emu->x86.R_AH = 0x48u;
  emu->x86.R_DX = 99u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x83u);

  emu->x86.R_AH = 0x48u;
  emu->x86.R_DX = h1;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u);

  // AH=4Bh Handle Count, 4Ch Handle Pages, 4Dh All Handle Pages.
  emu->x86.R_AH = 0x4Bu;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u && emu->x86.R_BX >= 2u);

  emu->x86.R_AH = 0x4Cu;
  emu->x86.R_DX = 99u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x83u);

  emu->x86.R_AH = 0x4Cu;
  emu->x86.R_DX = h1;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u && emu->x86.R_BX == 2u);

  emu->x86.R_AH = 0x4Du;
  emu->x86.R_DI = 0x0000u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u && emu->x86.R_BX >= 2u);

  // AH=4Eh Get/Set Page Map (AL=03h size=16, AL=04h->0x8F, AL=00h/02h/01h + corrupt 0xA3).
  emu->x86.R_AX = 0x4E03u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u && emu->x86.R_AL == 16u);

  emu->x86.R_AX = 0x4E04u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x8Fu);

  emu->x86.R_AX = 0x4E00u;
  emu->x86.R_DI = 0x0100u;  // ES:0100
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u);

  std::memcpy(m.guest_ram() + 0x20200u, m.guest_ram() + 0x21100u, 16u);
  emu->x86.R_AX = 0x4E02u;
  emu->x86.R_SI = 0x0200u;
  emu->x86.R_DI = 0x0120u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u);

  // Corrupt entry (-5) -> 0xA3
  const std::int32_t bad_pg = -5;
  std::memcpy(m.guest_ram() + 0x20210u, &bad_pg, 4u);
  emu->x86.R_AX = 0x4E01u;
  emu->x86.R_SI = 0x0210u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0xA3u);

  // AH=50h Map Multiple Pages (AL=00h slot indices, AL=01h segment addresses, + errors).
  emu->x86.R_AX = 0x5002u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x8Fu);

  emu->x86.R_AX = 0x5000u;
  emu->x86.R_DX = 99u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x83u);

  auto write_u16 = [&](std::uint32_t lin, std::uint16_t v) {
    m.guest_ram()[lin + 0u] = static_cast<std::uint8_t>(v & 0xFFu);
    m.guest_ram()[lin + 1u] = static_cast<std::uint8_t>(v >> 8);
  };
  write_u16(0x20300u, 0u);
  write_u16(0x20302u, 0xD000u);
  write_u16(0x20304u, 1u);
  write_u16(0x20306u, 0xD400u);
  emu->x86.R_AX = 0x5001u;
  emu->x86.R_DX = h1;
  emu->x86.R_CX = 2u;
  emu->x86.R_SI = 0x0300u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u);

  write_u16(0x20302u, 0xD123u);  // misaligned segment -> 0x8B
  emu->x86.R_AX = 0x5001u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x8Bu);

  // AH=52h Handle Attribute, 53h Get/Set Handle Name, 58h Physical Address Array.
  emu->x86.R_AX = 0x5200u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u && emu->x86.R_AL == 0x00u);
  emu->x86.R_AX = 0x5201u;
  emu->x86.R_BL = 0x00u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u);
  emu->x86.R_AX = 0x5201u;
  emu->x86.R_BL = 0x01u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x91u);

  std::memcpy(m.guest_ram() + 0x20400u, "OVERLAY1", 8u);
  emu->x86.R_AX = 0x5301u;
  emu->x86.R_DX = h1;
  emu->x86.R_SI = 0x0400u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u);

  emu->x86.R_AX = 0x5300u;
  emu->x86.R_DX = h1;
  emu->x86.R_DI = 0x0420u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u);
  CHECK(std::memcmp(m.guest_ram() + 0x21420u, "OVERLAY1", 8u) == 0);

  emu->x86.R_AX = 0x5302u;
  emu->x86.R_DX = h1;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x8Fu);
  emu->x86.R_AX = 0x5300u;
  emu->x86.R_DX = 99u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x83u);

  emu->x86.R_AX = 0x5800u;
  emu->x86.R_DI = 0x0500u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u && emu->x86.R_CX == 4u);
  emu->x86.R_AX = 0x5801u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u && emu->x86.R_CX == 4u);
  emu->x86.R_AX = 0x5802u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x8Fu);

  // AH=57h Exchange (AL=01h) & error branches (length=0, >1MB 0x96, bad type 0x98,
  // off>=16K 0x95, >cap 0x93, overlapping exchange 0x97, overlapping EMS move 0x92).
  auto set_57_desc = [&](std::uint32_t len, std::uint8_t st, std::uint16_t sh,
                         std::uint16_t so, std::uint16_t ssp, std::uint8_t dt,
                         std::uint16_t dh, std::uint16_t doff,
                         std::uint16_t dsp) {
    std::uint8_t* d = m.guest_ram() + 0x20600u;
    std::memcpy(d + 0u, &len, 4u);
    d[4u] = st;
    write_u16(0x20605u, sh);
    write_u16(0x20607u, so);
    write_u16(0x20609u, ssp);
    d[11u] = dt;
    write_u16(0x2060Cu, dh);
    write_u16(0x2060Eu, doff);
    write_u16(0x20610u, dsp);
    emu->x86.R_SI = 0x0600u;
  };
  emu->x86.R_AX = 0x5702u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x8Fu);

  set_57_desc(0u, 0u, 0u, 0u, 0x2200u, 0u, 0u, 0x10u, 0x2200u);
  emu->x86.R_AX = 0x5700u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u);

  set_57_desc(0x200000u, 0u, 0u, 0u, 0x2200u, 0u, 0u, 0u, 0x2300u);
  emu->x86.R_AX = 0x5700u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x96u);

  set_57_desc(16u, 2u, 0u, 0u, 0x2200u, 0u, 0u, 0u, 0x2300u);
  emu->x86.R_AX = 0x5700u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x98u);

  set_57_desc(16u, 1u, h1, 0x4000u, 0u, 0u, 0u, 0u, 0x2300u);
  emu->x86.R_AX = 0x5700u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x95u);

  set_57_desc(16u, 1u, h1, 0x3FF8u, 1u, 0u, 0u, 0u, 0x2300u);
  emu->x86.R_AX = 0x5700u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x93u);

  // Non-overlapping exchange (AL=01h) swaps conventional [0x22000] <-> [0x22100].
  std::memcpy(m.guest_ram() + 0x22000u, "AAAAAAAA", 8u);
  std::memcpy(m.guest_ram() + 0x22100u, "BBBBBBBB", 8u);
  set_57_desc(8u, 0u, 0u, 0u, 0x2200u, 0u, 0u, 0x0100u, 0x2200u);
  emu->x86.R_AX = 0x5701u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u);
  CHECK(std::memcmp(m.guest_ram() + 0x22000u, "BBBBBBBB", 8u) == 0);
  CHECK(std::memcmp(m.guest_ram() + 0x22100u, "AAAAAAAA", 8u) == 0);

  // Overlapping exchange -> 0x97; overlapping EMS<->EMS move -> 0x92.
  set_57_desc(8u, 0u, 0u, 0u, 0x2200u, 0u, 0u, 4u, 0x2200u);
  emu->x86.R_AX = 0x5701u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x97u);

  set_57_desc(16u, 1u, h1, 0u, 0u, 1u, h1, 4u, 0u);
  emu->x86.R_AX = 0x5700u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x92u);

  // Exhaust remaining handles (62 more -> 0x85 on 64th), then deallocate all.
  std::vector<std::uint16_t> extra_handles;
  for (int i = 0; i < 62; ++i) {
    emu->x86.R_AH = 0x43u;
    emu->x86.R_BX = 1u;
    m.ems().handle_int67(emu);
    CHECK(emu->x86.R_AH == 0x00u);
    extra_handles.push_back(emu->x86.R_DX);
  }
  emu->x86.R_AH = 0x43u;
  emu->x86.R_BX = 1u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x85u);

  for (std::uint16_t eh : extra_handles) {
    emu->x86.R_AH = 0x45u;
    emu->x86.R_DX = eh;
    m.ems().handle_int67(emu);
    CHECK(emu->x86.R_AH == 0x00u);
  }
  emu->x86.R_AH = 0x45u;
  emu->x86.R_DX = 0u;  // OS handle 0 cannot be freed -> 0x83
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x83u);

  emu->x86.R_AH = 0x45u;
  emu->x86.R_DX = h1;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x00u);

  emu->x86.R_AH = 0x99u;
  m.ems().handle_int67(emu);
  CHECK(emu->x86.R_AH == 0x84u);
}

void test_mouse_hle_complete_int33_contract() {
  ScopedTempDir tmp;
  san4::MachineConfig cfg;
  cfg.game_dir = tmp.path();
  san4::X86Machine m(cfg);
  x86emu_t* emu = m.emu();
  bool poll = false;

  int cursor_notifications = 0;
  int warps = 0;
  int last_warp_x = -1;
  int last_warp_y = -1;
  m.mouse().set_cursor_callback(
      [&](const san4::DosCursorShape&, bool) { ++cursor_notifications; });
  m.mouse().set_warp_callback([&](int wx, int wy) {
    ++warps;
    last_warp_x = wx;
    last_warp_y = wy;
  });

  // AX=0021h software reset
  emu->x86.R_AX = 0x0021u;
  m.mouse().handle_int33(emu, poll);
  CHECK(emu->x86.R_AX == 0xFFFFu && emu->x86.R_BX == 3u && !poll);

  // Hide depth: reset() starts at 1 (DOS INT 33h spec: hidden until AX=0001h);
  // AX=0002h + AX=0010h raise depth to 3, so three AX=0001h calls reveal it.
  emu->x86.R_AX = 0x0002u;
  m.mouse().handle_int33(emu, poll);
  emu->x86.R_AX = 0x0010u;
  m.mouse().handle_int33(emu, poll);
  CHECK(!m.mouse().cursor_visible());
  const int before_show = cursor_notifications;
  emu->x86.R_AX = 0x0001u;
  m.mouse().handle_int33(emu, poll);
  CHECK(!m.mouse().cursor_visible());
  emu->x86.R_AX = 0x0001u;
  m.mouse().handle_int33(emu, poll);
  CHECK(!m.mouse().cursor_visible());
  emu->x86.R_AX = 0x0001u;
  m.mouse().handle_int33(emu, poll);
  CHECK(m.mouse().cursor_visible() && cursor_notifications == before_show + 1);

  // AX=0007h / 0008h swapped bounds & legacy 639/479 full-screen promotion.
  emu->x86.R_AX = 0x0007u;
  emu->x86.R_CX = 0u;
  emu->x86.R_DX = 639u;
  m.mouse().handle_int33(emu, poll);
  emu->x86.R_AX = 0x0008u;
  emu->x86.R_CX = 0u;
  emu->x86.R_DX = 479u;
  m.mouse().handle_int33(emu, poll);
  m.mouse().set_position(790, 590);
  CHECK(m.mouse().x() == 790 && m.mouse().y() == 590);

  emu->x86.R_AX = 0x0007u;
  emu->x86.R_CX = 500u;
  emu->x86.R_DX = 100u;  // reversed -> [100, 500]
  m.mouse().handle_int33(emu, poll);
  emu->x86.R_AX = 0x0008u;
  emu->x86.R_CX = 400u;
  emu->x86.R_DX = 50u;   // reversed -> [50, 400]
  m.mouse().handle_int33(emu, poll);
  CHECK(m.mouse().x() == 500 && m.mouse().y() == 400);

  // AX=0004h warp clamping
  emu->x86.R_AX = 0x0004u;
  emu->x86.R_CX = 250u;
  emu->x86.R_DX = 180u;
  m.mouse().handle_int33(emu, poll);
  CHECK(warps == 1 && last_warp_x == 250 && last_warp_y == 180);

  // AX=0005h / 0006h button press & release data (valid + out-of-range BX>=3).
  m.mouse().set_button(1, true);
  m.mouse().set_button(1, false);
  emu->x86.R_AX = 0x0005u;
  emu->x86.R_BX = 5u;
  m.mouse().handle_int33(emu, poll);
  CHECK(emu->x86.R_BX == 0u && emu->x86.R_CX == 0u);
  emu->x86.R_AX = 0x0005u;
  emu->x86.R_BX = 1u;
  m.mouse().handle_int33(emu, poll);
  CHECK(emu->x86.R_BX == 1u && emu->x86.R_CX == 250u && emu->x86.R_DX == 180u);

  emu->x86.R_AX = 0x0006u;
  emu->x86.R_BX = 5u;
  m.mouse().handle_int33(emu, poll);
  CHECK(emu->x86.R_BX == 0u);
  emu->x86.R_AX = 0x0006u;
  emu->x86.R_BX = 1u;
  m.mouse().handle_int33(emu, poll);
  CHECK(emu->x86.R_BX == 1u && emu->x86.R_CX == 250u && emu->x86.R_DX == 180u);

  // AX=0009h 16x16 graphics cursor: row 0 tests all 4 (AND, XOR) bit pairs!
  x86emu_set_seg_register(emu, emu->x86.R_ES_SEL, 0x2000u);
  // Bit 15: AND=1,XOR=0 (transparent); Bit 14: AND=0,XOR=0 (black);
  // Bit 13: AND=0,XOR=1 (white);       Bit 12: AND=1,XOR=1 (inverted)
  const std::uint16_t and0 = 0x9000u;
  const std::uint16_t xor0 = 0x3000u;
  std::memcpy(m.guest_ram() + 0x20000u, &and0, 2u);
  std::memcpy(m.guest_ram() + 0x20020u, &xor0, 2u);
  emu->x86.R_AX = 0x0009u;
  emu->x86.R_BX = 4u;
  emu->x86.R_CX = 5u;
  emu->x86.R_DX = 0x0000u;
  m.mouse().handle_int33(emu, poll);
  const auto& cs = m.mouse().cursor_shape();
  CHECK(cs.hot_x == 4 && cs.hot_y == 5);
  CHECK(cs.argb[0] == 0x00000000u);
  CHECK(cs.argb[1] == 0xFF000000u);
  CHECK(cs.argb[2] == 0xFFFFFFFFu);
  CHECK(cs.argb[3] == 0xFF202020u);

  // AX=000Bh mickey counters & stubbed functions (000Ch, 000Fh, 0012h, 001Ah, 0028h, 00FFh).
  m.mouse().set_position(260, 195);
  emu->x86.R_AX = 0x000Bu;
  m.mouse().handle_int33(emu, poll);
  CHECK(emu->x86.R_CX == 20u && emu->x86.R_DX == 30u);

  for (std::uint16_t fn :
       {0x000Cu, 0x000Fu, 0x0012u, 0x001Au, 0x0028u, 0x00FFu, 0x0099u}) {
    emu->x86.R_AX = fn;
    m.mouse().handle_int33(emu, poll);
    if (fn == 0x0028u) {
      CHECK(emu->x86.R_AX == 0x0000u);
    }
  }
}

void test_dos_kernel_console_ioctl_tsr_and_video_bios_ports() {
  ScopedTempDir tmp;
  const auto game_dir = tmp.path() / "game";
  const auto save_dir = tmp.path() / "save";
  const auto hd_dir = tmp.path() / "hd";
  std::filesystem::create_directories(game_dir);
  std::filesystem::create_directories(save_dir);
  std::filesystem::create_directories(hd_dir / "cg");

  {
    std::ofstream f(game_dir / "ATTRTEST.DAT", std::ios::binary);
    f << "hello";
  }

  san4::MachineConfig cfg;
  cfg.game_dir = game_dir;
  cfg.save_dir = save_dir;
  san4::X86Machine m(cfg);
  x86emu_t* emu = m.emu();
  x86emu_set_seg_register(emu, emu->x86.R_DS_SEL, 0x2000u);
  x86emu_set_seg_register(emu, emu->x86.R_ES_SEL, 0x2100u);

  // 1) Const X86Machine accessors (machine.h) & BIOS interrupts 11h, 12h, 13h, 15h, 1Ah.
  {
    const san4::X86Machine& cm = m;
    CHECK(cm.emu() != nullptr);
    CHECK(cm.video().active_width() > 0);
    CHECK(cm.audio().buffered_frames() == 0u);
    CHECK(cm.mouse().x() >= 0);
    CHECK(!cm.dos().program_exited());
    CHECK(cm.ems().free_pages() == 256u);
    CHECK(cm.hd_pack().loaded_texture_count() == 0u);
    CHECK(!cm.is_waiting_for_input());
  }
  {
    static constexpr std::uint8_t kBiosCheck[] = {
        0xCD, 0x11, 0xA3, 0x00, 0x00,        // int 11h -> [0]
        0xCD, 0x12, 0xA3, 0x02, 0x00,        // int 12h -> [2]
        0xB4, 0x00, 0xCD, 0x13,              // int 13h
        0xB8, 0x00, 0x50, 0xCD, 0x15,        // int 15h (CF=1, AH=86h)
        0xA3, 0x04, 0x00,
        0xB4, 0x00, 0xCD, 0x1A,              // int 1Ah
        0x89, 0x16, 0x06, 0x00,
        0xEB, 0xFE,
    };
    m.load_synthetic_code(0x2500u, 0x0100u, kBiosCheck);
    m.step_instructions(20u);
    CHECK(m.guest_ram()[0x25000u] == 0x26u);
    CHECK(m.guest_ram()[0x25002u] == (640u & 0xFFu));
    CHECK(m.guest_ram()[0x25005u] == 0x86u);
  }

  // 2) INT 16h keyboard services (00h/10h, 01h/11h peek_key, 02h/12h, 03h, 05h).
  m.dos().clear_keys();
  bool kpoll = false;
  emu->x86.R_IP = 0x0200u;
  emu->x86.R_AH = 0x00u;
  m.dos().handle_int16(emu, kpoll);
  CHECK(kpoll && emu->x86.R_IP == 0x01FEu);

  emu->x86.R_AH = 0x11u;
  m.dos().handle_int16(emu, kpoll);
  CHECK((emu->x86.R_FLG & FB_ZF) != 0u);

  emu->x86.R_AH = 0x05u;
  emu->x86.R_CL = 'K';
  emu->x86.R_CH = 0x25u;
  m.dos().handle_int16(emu, kpoll);

  emu->x86.R_AH = 0x01u;
  m.dos().handle_int16(emu, kpoll);
  CHECK((emu->x86.R_FLG & FB_ZF) == 0u && emu->x86.R_AL == 'K');

  emu->x86.R_AH = 0x10u;
  m.dos().handle_int16(emu, kpoll);
  CHECK(emu->x86.R_AL == 'K' && emu->x86.R_AH == 0x25u);

  m.dos().set_shift_flags(0x05u);
  emu->x86.R_AH = 0x12u;
  m.dos().handle_int16(emu, kpoll);
  CHECK(emu->x86.R_AL == 0x05u);
  emu->x86.R_AH = 0x03u;
  m.dos().handle_int16(emu, kpoll);

  // 3) INT 21h console I/O (01h, 02h, 06h, 07h, 08h, 09h, 0Ah buffered line, 0Bh, 0Ch).
  x86emu_set_seg_register(emu, emu->x86.R_DS_SEL, 0x2000u);
  x86emu_set_seg_register(emu, emu->x86.R_ES_SEL, 0x2100u);
  emu->x86.R_IP = 0x0300u;
  emu->x86.R_AH = 0x01u;
  m.dos().handle_int21(emu);
  CHECK(emu->x86.R_IP == 0x02FEu);

  m.dos().push_key('X', 0x2Du);
  emu->x86.R_AH = 0x07u;
  m.dos().handle_int21(emu);
  CHECK(emu->x86.R_AL == 'X');

  emu->x86.R_AH = 0x02u;
  emu->x86.R_DL = 'Z';
  m.dos().handle_int21(emu);
  CHECK(emu->x86.R_AL == 'Z');

  emu->x86.R_AH = 0x06u;
  emu->x86.R_DL = 0xFFu;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_ZF) != 0u);
  m.dos().push_key('Y', 0x15u);
  emu->x86.R_AH = 0x06u;
  emu->x86.R_DL = 0xFFu;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_ZF) == 0u && emu->x86.R_AL == 'Y');
  emu->x86.R_AH = 0x06u;
  emu->x86.R_DL = '!';
  m.dos().handle_int21(emu);
  CHECK(emu->x86.R_AL == '!');

  emu->x86.R_AH = 0x09u;
  m.dos().handle_int21(emu);
  CHECK(emu->x86.R_AL == '$');

  // AH=0Ah buffered input: "AB\bCD\r" with max_chars=5 (cap=4) -> "ACD\r" (len 3).
  m.guest_ram()[0x20100u] = 5u;
  for (char c : {'A', 'B', '\b', 'C', 'D', '\r'}) {
    m.dos().push_key(static_cast<std::uint8_t>(c), 0x1Eu);
  }
  emu->x86.R_AH = 0x0Bu;
  m.dos().handle_int21(emu);
  CHECK(emu->x86.R_AL == 0xFFu);

  emu->x86.R_AH = 0x0Au;
  emu->x86.R_DX = 0x0100u;
  m.dos().handle_int21(emu);
  CHECK(m.guest_ram()[0x20101u] == 3u);
  CHECK(std::memcmp(m.guest_ram() + 0x20102u, "ACD\r", 4u) == 0);

  m.dos().push_key('Q', 0x10u);
  emu->x86.R_AX = 0x0C06u;
  emu->x86.R_DL = 0xFFu;
  m.dos().handle_int21(emu);
  CHECK(emu->x86.R_AL == 0x00u);  // flushed before reading!

  // 4) INT 21h misc system, IOCTL, rename, truncate, MCB error paths, handle cap, & TSR.
  emu->x86.R_AH = 0x0Du;
  m.dos().handle_int21(emu);
  emu->x86.R_AH = 0x0Eu;
  emu->x86.R_DL = 2u;
  m.dos().handle_int21(emu);
  emu->x86.R_AH = 0x19u;
  m.dos().handle_int21(emu);
  CHECK(emu->x86.R_AL == 2u);

  emu->x86.R_AH = 0x1Au;
  emu->x86.R_DX = 0x0400u;
  m.dos().handle_int21(emu);
  emu->x86.R_AH = 0x2Fu;
  m.dos().handle_int21(emu);
  CHECK(emu->x86.R_ES == 0x2000u && emu->x86.R_BX == 0x0400u);

  emu->x86.R_AX = 0x2565u;
  emu->x86.R_DX = 0x1234u;
  m.dos().handle_int21(emu);
  emu->x86.R_AX = 0x3565u;
  m.dos().handle_int21(emu);
  CHECK(emu->x86.R_ES == 0x2000u && emu->x86.R_BX == 0x1234u);

  x86emu_set_seg_register(emu, emu->x86.R_ES_SEL, 0x2100u);
  emu->x86.R_AH = 0x2Au;
  m.dos().handle_int21(emu);
  CHECK(emu->x86.R_CX == 1995u);
  emu->x86.R_AH = 0x30u;
  m.dos().handle_int21(emu);
  CHECK(emu->x86.R_AL == 5u);
  emu->x86.R_AX = 0x3300u;
  m.dos().handle_int21(emu);
  CHECK(emu->x86.R_DL == 0u);
  emu->x86.R_AH = 0x36u;
  m.dos().handle_int21(emu);
  CHECK(emu->x86.R_AX == 64u);
  emu->x86.R_AX = 0x3800u;
  emu->x86.R_DX = 0x0500u;
  m.dos().handle_int21(emu);
  CHECK(emu->x86.R_BX == 1u);

  std::memcpy(m.guest_ram() + 0x20600u, "ATTRTEST.DAT\0", 13u);
  std::memcpy(m.guest_ram() + 0x20620u, "MISSING.DAT\0", 12u);
  std::memcpy(m.guest_ram() + 0x20640u, "NUL\0", 4u);
  std::memcpy(m.guest_ram() + 0x20660u, "REN_A.SAV\0", 10u);
  std::memcpy(m.guest_ram() + 0x21680u, "REN_B.SAV\0", 10u);

  emu->x86.R_AX = 0x4300u;
  emu->x86.R_DX = 0x0600u;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u && emu->x86.R_CX == 0x0020u);
  emu->x86.R_AX = 0x4300u;
  emu->x86.R_DX = 0x0620u;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) != 0u);

  // Open NUL device + IOCTL (44h AL=00h, 07h, 08h, invalid handle).
  emu->x86.R_AX = 0x3D02u;
  emu->x86.R_DX = 0x0640u;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u);
  const std::uint16_t nul_h = emu->x86.R_AX;

  emu->x86.R_AX = 0x4400u;
  emu->x86.R_BX = nul_h;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u && emu->x86.R_DX == 0x0080u);
  emu->x86.R_AX = 0x4400u;
  emu->x86.R_BX = 999u;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) != 0u && emu->x86.R_AX == 6u);
  emu->x86.R_AX = 0x4407u;
  m.dos().handle_int21(emu);
  CHECK(emu->x86.R_AL == 0xFFu);
  emu->x86.R_AX = 0x4408u;
  m.dos().handle_int21(emu);
  CHECK(emu->x86.R_AX == 1u);

  // Create REN_A.SAV, write 10 bytes, truncate to 4 via AH=40h CX=0, close, and rename -> REN_B.SAV!
  emu->x86.R_AX = 0x3C00u;
  emu->x86.R_CX = 0u;
  emu->x86.R_DX = 0x0660u;
  m.dos().handle_int21(emu);
  const std::uint16_t ren_h = emu->x86.R_AX;
  std::memcpy(m.guest_ram() + 0x20700u, "0123456789", 10u);
  emu->x86.R_AH = 0x40u;
  emu->x86.R_BX = ren_h;
  emu->x86.R_CX = 10u;
  emu->x86.R_DX = 0x0700u;
  m.dos().handle_int21(emu);
  emu->x86.R_AX = 0x4200u;
  emu->x86.R_BX = ren_h;
  emu->x86.R_CX = 0u;
  emu->x86.R_DX = 4u;
  m.dos().handle_int21(emu);
  emu->x86.R_AH = 0x40u;
  emu->x86.R_BX = ren_h;
  emu->x86.R_CX = 0u;  // truncate at pos 4!
  m.dos().handle_int21(emu);
  CHECK(std::filesystem::file_size(save_dir / "REN_A.SAV") == 4u);

  emu->x86.R_AH = 0x56u;
  emu->x86.R_DX = 0x0660u;
  emu->x86.R_DI = 0x0680u;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) == 0u);
  CHECK(std::filesystem::exists(save_dir / "REN_B.SAV"));

  // AH=47h, 49h/4Ah errors, 4Bh AL!=0, 50h/51h/62h, 57h, 58h UMB, 67h, INT 20h.
  emu->x86.R_AH = 0x47u;
  emu->x86.R_SI = 0x0750u;
  m.dos().handle_int21(emu);
  CHECK(m.guest_ram()[0x20750u] == 0u);

  emu->x86.R_AX = 0x4B03u;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) != 0u);

  emu->x86.R_AX = 0x5803u;
  emu->x86.R_BX = 1u;
  m.dos().handle_int21(emu);
  emu->x86.R_AX = 0x5802u;
  m.dos().handle_int21(emu);
  CHECK(emu->x86.R_AL == 1u);
  emu->x86.R_AX = 0x5899u;
  m.dos().handle_int21(emu);
  CHECK((emu->x86.R_FLG & FB_CF) != 0u);

  emu->x86.R_AH = 0x51u;
  m.dos().handle_int21(emu);
  const std::uint16_t orig_psp = emu->x86.R_BX;
  emu->x86.R_AH = 0x50u;
  emu->x86.R_BX = orig_psp;
  m.dos().handle_int21(emu);
  emu->x86.R_AX = 0x5700u;
  m.dos().handle_int21(emu);
  CHECK(emu->x86.R_DX == 0x2121u);
  emu->x86.R_AH = 0x67u;
  m.dos().handle_int21(emu);

  // 5) VesaPlanarVideo VGA ports, INT 10h palette/VBE subfunctions, & PIT ch0 modes 1/2/3.
  san4::VesaPlanarVideo& vid = m.video();
  vid.reset_mode(0x0102u);
  (void)vid.port_in8(0x03DAu);  // reset flip-flop
  vid.port_out8(0x03C0u, 0x02u);
  CHECK(vid.port_in8(0x03C0u) == 0x02u);
  vid.port_out8(0x03C0u, 0x15u);
  (void)vid.port_in8(0x03DAu);
  vid.port_out8(0x03C0u, 0x02u);
  CHECK(vid.port_in8(0x03C1u) == 0x15u);

  vid.port_out8(0x03C4u, 0x02u);
  vid.port_out8(0x03C5u, 0x0Fu);
  CHECK(vid.port_in8(0x03C4u) == 0x02u && vid.port_in8(0x03C5u) == 0x0Fu);
  vid.port_out8(0x03CEu, 0x05u);
  vid.port_out8(0x03CFu, 0x00u);
  CHECK(vid.port_in8(0x03CEu) == 0x05u && vid.port_in8(0x03CFu) == 0x00u);
  vid.port_out8(0x03C6u, 0xFFu);
  CHECK(vid.port_in8(0x03C6u) == 0xFFu);

  vid.port_out8(0x03C8u, 0x05u);
  CHECK(vid.port_in8(0x03C8u) == 0x05u);
  vid.port_out8(0x03C9u, 0x11u);
  vid.port_out8(0x03C9u, 0x22u);
  vid.port_out8(0x03C9u, 0x33u);
  vid.port_out8(0x03C7u, 0x05u);
  CHECK(vid.port_in8(0x03C9u) == 0x11u && vid.port_in8(0x03C9u) == 0x22u &&
        vid.port_in8(0x03C9u) == 0x33u);
  for (int i = 0; i < 5; ++i) {
    (void)vid.port_in8(0x03BAu);
  }
  CHECK(vid.consume_vsync_edge());
  CHECK(!vid.consume_vsync_edge());

  // INT 10h subfunctions: 4F03h, 1130h, 0Fh, 10h AL=00,01,02,07,08,09,10,12,15,17
  const std::span<std::uint8_t> ram_span(m.guest_ram(),
                                         san4::X86Machine::kGuestRamBytes);
  std::uint16_t ax = 0x4F03u, bx = 0u, cx = 0u, dx = 0u, es = 0x2100u, bp = 0u;
  vid.handle_int10(ax, bx, cx, dx, es, bp, 0u, ram_span, 0xF000u, 0xB000u,
                   0x0500u, 0x0510u, emu);
  CHECK(ax == 0x004Fu && bx == 0x0102u);

  ax = 0x1130u;
  vid.handle_int10(ax, bx, cx, dx, es, bp, 0u, ram_span, 0xF000u, 0xB000u,
                   0x0500u, 0x0510u, emu);
  CHECK(es == 0xF000u && bp == 0xB000u && cx == 16u);

  es = 0x2100u;
  ax = 0x0F00u;
  vid.handle_int10(ax, bx, cx, dx, es, bp, 0u, ram_span, 0xF000u, 0xB000u,
                   0x0500u, 0x0510u, emu);
  CHECK((ax >> 8) == 80u);

  ax = 0x1000u;
  bx = 0x2A03u;
  vid.handle_int10(ax, bx, cx, dx, es, bp, 0u, ram_span, 0xF000u, 0xB000u,
                   0x0500u, 0x0510u, emu);
  ax = 0x1007u;
  bx = 0x0003u;
  vid.handle_int10(ax, bx, cx, dx, es, bp, 0u, ram_span, 0xF000u, 0xB000u,
                   0x0500u, 0x0510u, emu);
  CHECK((bx >> 8) == 0x2Au);

  ax = 0x1001u;
  bx = 0x1700u;
  vid.handle_int10(ax, bx, cx, dx, es, bp, 0u, ram_span, 0xF000u, 0xB000u,
                   0x0500u, 0x0510u, emu);
  ax = 0x1008u;
  bx = 0u;
  vid.handle_int10(ax, bx, cx, dx, es, bp, 0u, ram_span, 0xF000u, 0xB000u,
                   0x0500u, 0x0510u, emu);
  CHECK((bx >> 8) == 0x17u);

  ax = 0x1009u;
  dx = 0x0000u;
  vid.handle_int10(ax, bx, cx, dx, es, bp, 0u, ram_span, 0xF000u, 0xB000u,
                   0x0500u, 0x0510u, emu);
  ax = 0x1002u;
  dx = 0x0000u;
  vid.handle_int10(ax, bx, cx, dx, es, bp, 0u, ram_span, 0xF000u, 0xB000u,
                   0x0500u, 0x0510u, emu);

  ax = 0x1010u;
  bx = 4u;
  dx = 0x1500u;
  cx = 0x2535u;
  vid.handle_int10(ax, bx, cx, dx, es, bp, 0u, ram_span, 0xF000u, 0xB000u,
                   0x0500u, 0x0510u, emu);
  ax = 0x1015u;
  bx = 4u;
  cx = 0u;
  dx = 0u;
  vid.handle_int10(ax, bx, cx, dx, es, bp, 0u, ram_span, 0xF000u, 0xB000u,
                   0x0500u, 0x0510u, emu);
  CHECK((dx >> 8) == 0x15u && cx == 0x2535u);

  ax = 0x1017u;
  bx = 0u;
  cx = 4u;
  dx = 0x0100u;
  vid.handle_int10(ax, bx, cx, dx, es, bp, 0u, ram_span, 0xF000u, 0xB000u,
                   0x0500u, 0x0510u, emu);
  ax = 0x1012u;
  bx = 0u;
  cx = 4u;
  dx = 0x0100u;
  vid.handle_int10(ax, bx, cx, dx, es, bp, 0u, ram_span, 0xF000u, 0xB000u,
                   0x0500u, 0x0510u, emu);
  vid.clear_all_glyphs();

  // 6) HdPackManager::on_npk_blit_complete (clipped dynamic CG + cache_hits) &
  //    utility helpers (parse_mode, ascii_lower, HostStatus::operator==, win32_platform).
  {
    const auto npk_full =
        build_synthetic_npk_image(false, true, 8, 4, 0x1122u, 0x3344u);
    const std::span<const std::uint8_t> npk_stream(npk_full.data() + 48u,
                                                   npk_full.size() - 48u);
    std::vector<std::uint8_t> npk_slots;
    CHECK(san4::codecs::decompress_npk_bitstream(npk_stream, 8, 4, npk_slots));
    const std::uint64_t npk_hash =
        san4::HdPackManager::hash_sprite_4bit(8, 4, npk_slots.data());
    const std::vector<std::uint32_t> cg_px(32u * 16u, 0xFF446688u);
    CHECK(san4::HdPackManager::save_argb_png(hd_dir / "cg" / "test_npk.png", 32,
                                             16, cg_px.data()));
    {
      std::ofstream mf(hd_dir / "hd_manifest.tsv", std::ios::binary);
      char hbuf[32];
      std::snprintf(hbuf, sizeof(hbuf), "%016llx",
                    static_cast<unsigned long long>(npk_hash));
      mf << hbuf
         << "\t8\t4\t3\tcg/test_npk.png\t"
            "000000,111111,222222,333333,444444,555555,666666,777777,"
            "888888,999999,aaaaaa,bbbbbb,cccccc,dddddd,eeeeee,ffffff\n";
    }
    san4::HdPackManager hd;
    hd.set_pack_dir(hd_dir);
    vid.latch_frame();
    // Clipped NPK blit hits decompress_npk_bitstream + register_dynamic_cg;
    // second call hits texture LRU cache (cache_hits() > 0)!
    hd.on_npk_blit_complete(0, 0, 8, 4, san4::GrpClipRect{0, 0, 5, 3},
                            npk_stream, vid);
    hd.on_npk_blit_complete(16, 0, 8, 4, san4::GrpClipRect{16, 0, 21, 3},
                            npk_stream, vid);
    CHECK(hd.cache_hits() >= 1u);
  }

  {
    san4::UpscaleMode pm = san4::UpscaleMode::Nearest;
    CHECK(san4::PixelUpscaler::parse_mode("nearest", pm) &&
          pm == san4::UpscaleMode::Nearest);
    CHECK(san4::PixelUpscaler::parse_mode("1x", pm) &&
          pm == san4::UpscaleMode::Nearest);
    CHECK(san4::PixelUpscaler::parse_mode("sharp2x", pm) &&
          pm == san4::UpscaleMode::Sharp2x);
    CHECK(san4::PixelUpscaler::parse_mode("scale2x", pm) &&
          pm == san4::UpscaleMode::Scale2x);
    CHECK(san4::PixelUpscaler::parse_mode("xbr2x", pm) &&
          pm == san4::UpscaleMode::Xbr2x);
    CHECK(san4::PixelUpscaler::parse_mode("xbr4x", pm) &&
          pm == san4::UpscaleMode::Xbr4x);
    CHECK(!san4::PixelUpscaler::parse_mode("invalid", pm));

    CHECK(san4::util::ascii_lower("AbC_123-XyZ") == "abc_123-xyz");
    san4::HostStatus hs1{}, hs2{};
    CHECK(hs1 == hs2);
    hs2.speed_multiplier = 4;
    CHECK(!(hs1 == hs2));

    ScopedEnvVar no_dlg("SAN4_NO_ERROR_DIALOG", "1");
    san4::win32_platform::init_process();
    san4::win32_platform::attach_parent_console_if_needed();
    san4::win32_platform::show_error_dialog("Test", "Suppressed");
    char arg0[] = "san4-test";
    char arg1[] = "--help";
    char* argv_test[] = {arg0, arg1, nullptr};
    const auto uargs = san4::win32_platform::get_utf8_args(2, argv_test);
    CHECK(!uargs.empty());
    CHECK(!san4::win32_platform::get_executable_path("san4-test").empty());
    CHECK(!san4::win32_platform::get_env_path("").has_value());
  }

  // INT 20h terminate on a booted stub .COM
  {
    static constexpr std::uint8_t kInt20Com[] = {0xCD, 0x20};
    write_binary_file_checked(game_dir / "EXIT20.COM", kInt20Com);
    CHECK(m.boot("EXIT20.COM"));
    m.step_instructions(4u);
    CHECK(m.exited());
  }
}

void test_machine_grp600c_hle_intercepts_pit_ch0_and_sync_loop() {
  ScopedTempDir tmp;
  const auto game_dir = tmp.path() / "game";
  const auto save_dir = tmp.path() / "save";
  const auto hd_dir = tmp.path() / "hd";
  const auto dump_dir = tmp.path() / "dump";
  std::filesystem::create_directories(game_dir);
  std::filesystem::create_directories(save_dir);
  std::filesystem::create_directories(hd_dir);
  std::filesystem::create_directories(dump_dir);

  san4::MachineConfig cfg;
  cfg.game_dir = game_dir;
  cfg.save_dir = save_dir;
  cfg.dump_sprites_dir = dump_dir;
  san4::X86Machine m(cfg);
  m.video().reset_mode(0x0102u);

  // 1) Speed multiplier cycle (1 -> 2 -> 4 -> 1) and invalid clamping.
  CHECK(m.cycle_base_speed_multiplier() == 2);
  CHECK(m.cycle_base_speed_multiplier() == 4);
  CHECK(m.cycle_base_speed_multiplier() == 1);
  m.set_base_speed_multiplier(99);
  CHECK(m.base_speed_multiplier() == 4);
  m.set_base_speed_multiplier(0);
  CHECK(m.base_speed_multiplier() == 1);

  // 2) Fallback INT F1h without GRP600C loaded (`grp600c_cs() == 0`):
  //    16x16 mono blit marks glyph tile; 32x16 planar multi-pass queues LegacyMultiPlane.
  {
    std::vector<std::uint8_t> no_grp_code = {
        0xB8, 0x00, 0x13,  // mov ax, 0x1300
        0xBB, 0x10, 0x00,  // mov bx, 16
        0xB9, 0x10, 0x00,  // mov cx, 16
        0xBE, 0x10, 0x00,  // mov si, 16
        0xBF, 0x10, 0x00,  // mov di, 16
        0xCD, 0xF1,        // int 0xF1 (marks 16x16 glyph)
        0xB8, 0x00, 0x0F,  // mov ax, 0x0F00
        0xBB, 0x40, 0x00,  // mov bx, 64
        0xB9, 0x40, 0x00,  // mov cx, 64
        0xBE, 0x20, 0x00,  // mov si, 32
        0xBF, 0x10, 0x00,  // mov di, 16
        0xCD, 0xF1,        // int 0xF1 (pass 1)
        0xCD, 0xF1,        // int 0xF1 (pass 2)
        0xB8, 0x00, 0x14,  // mov ax, 0x1400 (flushes LegacyMultiPlane)
        0xCD, 0xF1,
        0xEB, 0xFE,        // jmp $
    };
    m.load_synthetic_code(0x3000u, 0x0000u, no_grp_code, 0x3000u, 0x0FF0u);
    m.step_instructions(64u);
    CHECK(m.video().live_glyph_tiles()[san4::VesaPlanarVideo::kTilesX + 1u] != 0u);
  }

  // 3) PIT channel 0 access modes (LSB=1, MSB=2, LSB+MSB=3, latch=0) + port_in8
  //    routing for VGA (0x3DA), OPL2 (0x388), PIC (0x21), KBD (0x60), PC-98 (0x5F).
  {
    std::vector<std::uint8_t> io_code = {
        // Mode 1 (LSB only): out 0x43, 0x16; out 0x40, 0x80; in al, 0x40
        0xB0, 0x16, 0xE6, 0x43,
        0xB0, 0x80, 0xE6, 0x40,
        0xE4, 0x40,
        // Latch in mode 1: out 0x43, 0x00; in al, 0x40
        0xB0, 0x00, 0xE6, 0x43,
        0xE4, 0x40,
        // Mode 2 (MSB only): out 0x43, 0x26; out 0x40, 0x10; in al, 0x40
        0xB0, 0x26, 0xE6, 0x43,
        0xB0, 0x10, 0xE6, 0x40,
        0xE4, 0x40,
        // Latch in mode 2: out 0x43, 0x00; in al, 0x40
        0xB0, 0x00, 0xE6, 0x43,
        0xE4, 0x40,
        // Mode 3 (LSB+MSB): out 0x43, 0x36; out 0x40, 0x00; out 0x40, 0x20
        0xB0, 0x36, 0xE6, 0x43,
        0xB0, 0x00, 0xE6, 0x40,
        0xB0, 0x20, 0xE6, 0x40,
        // Unlatched 2-byte read, then latched 2-byte read:
        0xE4, 0x40, 0xE4, 0x40,
        0xB0, 0x00, 0xE6, 0x43,
        0xE4, 0x40, 0xE4, 0x40,
        // VGA 0x3DA, OPL2 0x388, PIC 0x21, KBD 0x60, PC-98 stub 0x5F:
        0xBA, 0xDA, 0x03, 0xEC,
        0xBA, 0x88, 0x03, 0xEC,
        0xE4, 0x21,
        0xE4, 0x60,
        0xE4, 0x5F,
        0xEB, 0xFE,
    };
    m.load_synthetic_code(0x3100u, 0x0000u, io_code, 0x3100u, 0x0FF0u);
    m.step_instructions(128u);
    CHECK(m.emu()->x86.R_AL == 0xFFu);
  }

  // 4) Install fake GRP600C TSR and exercise all HLE dispatch & flush hooks:
  //    AH=0x0F (PlanarBlit0F -> dumps 16x16 sprite -> reload as 4x HD texture!),
  //    AH=0x10 (ReadPlanar), AH=0x13 (MonoBlit13 16x16 & 32x16),
  //    AH=0x17 (CopyRect17), AH=0x18 (SwapRect18),
  //    AH=0x19 (BlitNpk19 unclipped & clipped), AH=0x16 (SmoothFade + FadeHold),
  //    and AH=0x00 (Init + finish fade).
  const std::uint16_t grp_cs = boot_fake_grp600c(game_dir, m);
  CHECK(grp_cs != 0u);
  m.set_grp_census_enabled(true);
  const std::uint32_t grp_lin = static_cast<std::uint32_t>(grp_cs) << 4;
  // Set active planes = 4, clip = (0, 0, 799, 599), slot_map = kGrpSlotMap.
  m.guest_ram()[grp_lin + san4::kGrpCsActivePlanesOffset] = 4u;
  m.guest_ram()[grp_lin + san4::kGrpCsClipX1Offset + 0u] = 0x1Fu;
  m.guest_ram()[grp_lin + san4::kGrpCsClipX1Offset + 1u] = 0x03u;
  m.guest_ram()[grp_lin + san4::kGrpCsClipY1Offset + 0u] = 0x57u;
  m.guest_ram()[grp_lin + san4::kGrpCsClipY1Offset + 1u] = 0x02u;
  std::memcpy(m.guest_ram() + grp_lin + san4::kGrpSlotMapOff,
              san4::kGrpSlotMap.data(), san4::kGrpSlotMap.size());

  // Populate non-zero 4-plane 16x16 source bitmap at 0x4000:0x0000 (128 bytes).
  for (std::size_t i = 0; i < 128u; ++i) {
    m.guest_ram()[0x40000u + i] = static_cast<std::uint8_t>((i * 29u + 7u) | 1u);
  }
  // Populate target fade RGB4 table at 0x4000:0x0200 (12 bytes for 4 colors).
  for (std::size_t i = 0; i < 12u; ++i) {
    m.guest_ram()[0x40200u + i] = static_cast<std::uint8_t>(15u - (i & 7u));
  }

  const auto lo = static_cast<std::uint8_t>(grp_cs & 0xFFu);
  const auto hi = static_cast<std::uint8_t>(grp_cs >> 8);
  auto emit_grp_call = [&](std::vector<std::uint8_t>& c, std::uint16_t ax,
                           std::uint16_t bx, std::uint16_t cx, std::uint16_t dx,
                           std::uint16_t si, std::uint16_t di, std::uint16_t bp) {
    auto mov16 = [&](std::uint8_t op, std::uint16_t v) {
      c.push_back(op);
      c.push_back(static_cast<std::uint8_t>(v & 0xFFu));
      c.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    mov16(0xB8, 0x4000u);
    c.push_back(0x8E);
    c.push_back(0xC0);  // mov es, ax
    mov16(0xB8, ax);
    mov16(0xBB, bx);
    mov16(0xB9, cx);
    mov16(0xBA, dx);
    mov16(0xBE, si);
    mov16(0xBF, di);
    mov16(0xBD, bp);
    c.insert(c.end(), {0x9A, 0x18, 0x00, lo, hi});  // lcall grp_cs:0x0018
  };

  // First pass: dump the 16x16 sprite via AH=0x0F, then promote it into hd_dir
  // as a 64x64 4x RGBA override so loaded_texture_count() == 1 and AH=0x0F
  // places an active HD region!
  {
    std::vector<std::uint8_t> dump_prog;
    emit_grp_call(dump_prog, 0x0F00u, 32u, 32u, 0u, 16u, 16u, 0x0000u);
    dump_prog.insert(dump_prog.end(), {0xEB, 0xFE});
    m.load_synthetic_code(0x3200u, 0x0000u, dump_prog, 0x3200u, 0x0FF0u);
    m.step_instructions(64u);
  }
  std::filesystem::path dumped_png;
  for (const auto& ent : std::filesystem::directory_iterator(dump_dir)) {
    if (ent.path().extension() == ".png") {
      dumped_png = ent.path();
      break;
    }
  }
  CHECK(!dumped_png.empty());
  std::filesystem::copy_file(
      dumped_png, hd_dir / dumped_png.filename(),
      std::filesystem::copy_options::overwrite_existing);
  m.hd_pack().set_pack_dir(hd_dir);
  CHECK(m.hd_pack().loaded_texture_count() == 1u);

  // Second pass: full GRP600C HLE pipeline with active HD pack!
  {
    std::vector<std::uint8_t> grp_prog;
    // AH=0x0F PlanarBlit 16x16 at (32, 32) -> active_overlay_count() > 0!
    emit_grp_call(grp_prog, 0x0F00u, 32u, 32u, 0u, 16u, 16u, 0x0000u);
    // AH=0x10 ReadPlanar 16x16 at (32, 32) (flushes PlanarBlit0F first!)
    emit_grp_call(grp_prog, 0x1000u, 32u, 32u, 0u, 16u, 16u, 0x0000u);
    // AH=0x13 MonoBlit 16x16 glyph at (48, 32) & 32x16 UI pattern at (80, 32)
    emit_grp_call(grp_prog, 0x1300u, 48u, 32u, 0u, 16u, 16u, 0x0000u);
    emit_grp_call(grp_prog, 0x1300u, 80u, 32u, 0u, 32u, 16u, 0x0000u);
    // AH=0x17 CopyRect 16x16 from (32, 32) to (64, 32) (w=DX=16, h=SI=16, dst=DI,BP)
    emit_grp_call(grp_prog, 0x1700u, 32u, 32u, 16u, 16u, 64u, 32u);
    // AH=0x18 SwapPlanarRect 16x16 at (32, 32) (w=DX=16, h=SI=16, ES:DI=0x4000:0x0300)
    emit_grp_call(grp_prog, 0x1800u, 32u, 32u, 16u, 16u, 0x0300u, 0u);
    // AH=0x19 BlitNpk unclipped at (128, 32) & clipped at (-4, 32)
    emit_grp_call(grp_prog, 0x1900u, 128u, 32u, 16u, 16u, 0x0000u, 0u);
    emit_grp_call(grp_prog, 0x1900u, static_cast<std::uint16_t>(-4), 32u, 16u,
                  16u, 0x0000u, 0u);
    // AH=0x16 SmoothFade (start=0, count=4, steps=2, vsyncs=2, ES:BX=0x4000:0x0200)
    emit_grp_call(grp_prog, 0x1600u, 0x0200u, 0x0204u, 2u, 0u, 0u, 0u);
    // Resumes here once fade completes: AH=0x14 SetPaletteSingle & AH=0x00 Init!
    emit_grp_call(grp_prog, 0x1400u, 0u, 0u, 0u, 0u, 0u, 0u);
    emit_grp_call(grp_prog, 0x0000u, 0u, 0u, 0u, 0u, 0u, 0u);
    grp_prog.insert(grp_prog.end(), {0xEB, 0xFE});

    m.load_synthetic_code(0x3300u, 0x0000u, grp_prog, 0x3300u, 0x0FF0u);
    m.run_frame(1.0 / 60.0, 0.005);
    CHECK(m.video().is_fading());
    for (int f = 0; f < 10 && m.video().is_fading(); ++f) {
      m.run_frame(0.025, 0.005);
    }
    m.run_frame(0.025, 0.005);
    CHECK(!m.video().is_fading());
    CHECK(m.grp_census() != nullptr);
    std::ostringstream census_ss;
    m.grp_census()->print(census_ss);
    CHECK(census_ss.str().find("GRP600C") != std::string::npos);
    m.set_grp_census_enabled(false);
    CHECK(m.grp_census() == nullptr);
  }

  // 5) Synchronous EmuSession::run_loop (3-argument overload) + SdlHost cursor callback.
  {
    ScopedTempDir host_tmp;
    const auto gdir = host_tmp.path() / "game";
    std::filesystem::create_directories(gdir);
    // `mov ax, 1; int 0x33; mov ax, 0x4C00; int 0x21` -> triggers cursor callback & exits!
    static constexpr std::uint8_t kCursorExitCom[] = {
        0xB8, 0x01, 0x00, 0xCD, 0x33, 0xB8, 0x00, 0x4C, 0xCD, 0x21};
    write_binary_file_checked(gdir / "SAN486.COM", kCursorExitCom);

    san4::MachineConfig sync_cfg;
    sync_cfg.game_dir = gdir;
    sync_cfg.save_dir = gdir;
    san4::X86Machine sync_m(sync_cfg);
    CHECK(sync_m.boot("SAN486.COM"));
    san4::PixelUpscaler sync_up;
    san4::EmuSession sync_session(sync_m, sync_up);
    san4::FrameMailbox sync_mb;
    san4::HostCommandQueue sync_cmds;
    san4::EmuSession::LoopOptions loop_opts;
    loop_opts.drain_audio = true;
    loop_opts.profiling = true;
    sync_session.run_loop(sync_mb, sync_cmds, loop_opts);
    CHECK(sync_m.exited());

    ScopedEnvVar dummy_vid("SDL_VIDEODRIVER", "dummy");
    ScopedEnvVar dummy_aud("SDL_AUDIODRIVER", "dummy");
    san4::HostOptions hopts;
    hopts.game_dir = gdir;
    hopts.save_dir = gdir;
    san4::SdlHost host(hopts);
    CHECK(host.run() == 0);
  }
}

}  // namespace

int main() {
  using SuiteFn = void (*)();
  static constexpr SuiteFn kSuites[] = {
      test_sha256_and_grp600c_signature,
      test_vga_planar_16bit_out_and_write_modes,
      test_ems_zero_copy_int67,
      test_mouse_hle_thunk_coords_and_tap_latch,
      test_pit_irq0_cli_gating_and_speed_timers,
      test_dos_kernel_mcb_exec_and_cjk_env,
      test_opl2_adlib_timer_and_fm_audio,
      test_frozen_vram_smooth_crossfade,
      test_upscaler_dither_glyph_guards_and_perf,
      test_hd_pack_png_and_occlusion_mask,
      test_vbe_probe_ems_move_mode12_and_bda_keys,
      test_multiplane_ah13_sprite_dump_and_hd_overlay,
      test_mcb_last_fit_strategy_allows_heap_sbrk_growth,
      test_cross_boundary_memio_and_32bit_bda_poll,
      test_smooth_fade_16slot_ega_grp_and_midfade_override,
      test_ems_overlap_wrap_and_map_validation,
      test_mz_exe_relocations_com_bounds_and_child_restore,
      test_vfs_traversal_cow_truncate_and_find_subdir,
      test_vga_read_write_modes_pel_mask_and_reset,
      test_offscreen_clipping_and_hd_pack_bounds,
      test_mouse_pit_readback_and_concurrency_stress,
      test_hd_pack_v2_codecs_upscaler_canvas_and_grading,
      test_thread_pool_inline_nesting_async_and_stress,
      test_simd_div255_exhaustive_and_blend_kernels,
      test_upscaler_tile_row_spans_and_shared_pool,
      test_hd_art_upscaler_srgb_lut_and_pool_parity,
      test_grp_census_profile_rows_and_frame_stats,
      test_normalize_dirty_rects_disjoint_cover,
      test_hd_composite_parallel_parity_and_spans,
      test_hd_lut_cache_lru_bound,
      test_hd_used_color_mask_identity_and_propagation,
      test_hd_tile_dict_exact_match_index,
      test_hd_async_dynamic_cg_results_and_staleness,
      test_hd_async_dynamic_cg_failures_and_lifetime,
      test_prepare_frame_blends_hd_repaints_once,
      test_simd_level_override,
      test_memio_conv_fast_path_bda_poll_and_vram,
      test_fmdrv_turbo_timer_budget,
      test_fmdrv_turbo_timer_rearm_mid_burst,
      test_memio_port_io_not_fast_pathed,
      test_deterministic_run_frame_and_dos_clock,
      test_grp_drawbox_tile_fill_clears_glyph_marks,
      test_render_policy_profile_vsync_and_scale,
      test_render_policy_edge_cases_and_scale_pinning,
      test_frame_mailbox_tile_mapping_and_partial_rows,
      test_frame_mailbox_concurrent_skips_and_resize,
      test_frame_mailbox_mode_aba_and_vertical_merge,
      test_present_tracker_decisions,
      test_host_command_and_event_queues,
      test_emu_session_command_semantics,
      test_emu_thread_end_to_end_synthetic,
      test_emu_thread_exception_and_stop,
      test_sdl_host_headless_isolation,
      test_lto_cross_tu_simd_dispatch_and_aliasing_safety,
      test_win32_render_policy_and_unicode_png_paths,
      test_hd_pack_small_cache_compositing_and_bounds,
      test_hd_pack_path_sanitization_and_symlink_rejection,
      test_dos_fstream_rw_sync_commit_delete_and_nested_dta,
      test_vfs_cow_readonly_saves_and_device_rejection,
      test_frame_mailbox_current_acquired_and_host_helpers,
      test_machine_rom_protection_noperm_vram_and_ubsan_shifts,
      test_runtime_cli_parsing_discovery_and_execution,
      test_hdpack_generator_cli_and_synthetic_pipeline,
      test_sdl_host_headless_script_and_profiler_end_to_end,
      test_sdl_host_interactive_events_audio_and_present_slot,
      test_audio_pit_ch2_speaker_opl3_and_underrun_decay,
      test_ems_4_0_complete_subfunctions_and_error_codes,
      test_mouse_hle_complete_int33_contract,
      test_dos_kernel_console_ioctl_tsr_and_video_bios_ports,
      test_machine_grp600c_hle_intercepts_pit_ch0_and_sync_loop,
  };
  static_assert(sizeof(kSuites) / sizeof(kSuites[0]) == 70u);
  // Invoke through a volatile pointer so LTO/ASan cannot inline all 70 suites'
  // X86Machine / VesaPlanarVideo stack frames into a single giant main() frame.
  for (SuiteFn suite : kSuites) {
    SuiteFn volatile run = suite;
    run();
  }

  if (g_failures != 0) {
    std::cerr << "[san4_tests] FAILED (" << g_failures << " error(s))\n";
    return 1;
  }
  std::cout << "[san4_tests] ALL 70 CONFORMANCE & BENCHMARK SUITES PASSED\n";
  return 0;
}
