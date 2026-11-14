#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "dos/ems.h"
#include "dos/vfs.h"

typedef struct x86emu_s x86emu_t;

namespace san4 {

struct BiosKey {
  std::uint8_t ascii = 0;
  std::uint8_t scancode = 0;
};

class DosKernel {
 public:
  static constexpr std::uint16_t kFirstMcbSeg = 0x0800u;
  static constexpr std::uint16_t kConventionalTopSeg = 0xA000u;
  static constexpr int kMaxFiles = 128;

  DosKernel(CaseInsensitiveVfs& vfs, EmsManager& ems);

  void attach(x86emu_t* emu, std::uint8_t* guest_ram, std::size_t ram_size);
  void set_skip_intro(bool skip) { skip_intro_ = skip; }
  // When enabled, INT 21h AH=2Ch derives the time of day from the BIOS tick
  // count at 0040:006C (as real DOS does) instead of the host clock, so guest
  // RNG seeds replay identically.
  void set_deterministic_clock(bool enabled) { deterministic_clock_ = enabled; }

  // Boots an executable (e.g. "SAN486.COM" or synthetic test ".COM") as the
  // root DOS process.
  bool boot_program(std::string_view dos_prog_name,
                    std::string_view cmd_tail = "");

  // Handles INT 20h, INT 21h, and INT 16h.
  void handle_int20(x86emu_t* emu);
  void handle_int21(x86emu_t* emu);
  void handle_int16(x86emu_t* emu, bool& out_was_poll);

  // Keyboard queue management for host SDL events & QoL shortcuts.
  // Synchronizes with BDA circular buffer (0040:001A..003D) for direct readers.
  void push_key(std::uint8_t ascii, std::uint8_t scancode);
  void set_shift_flags(std::uint8_t flags);
  bool has_keys() const;
  void clear_keys();
  void pump_bda_keyboard();
  bool bda_has_key() const;

  bool program_exited() const { return program_exited_; }
  std::uint8_t exit_code() const {
    return static_cast<std::uint8_t>(last_child_return_code_ & 0xFFu);
  }
  std::uint8_t allocation_strategy() const { return mcb_alloc_strategy_; }

  // Discovered code segment of GRP600C.EXE once installed via AH=31h (0 if none).
  std::uint16_t grp600c_cs() const { return grp600c_cs_; }

  std::uint64_t consume_file_activity() {
    std::uint64_t v = file_activity_;
    file_activity_ = 0;
    return v;
  }

  // Direct MCB helpers exposed for synthetic unit tests.
  std::optional<std::uint16_t> mcb_alloc(std::uint16_t paras,
                                         std::uint16_t owner_psp,
                                         std::uint16_t* out_max_free = nullptr);
  bool mcb_free(std::uint16_t block_seg);
  bool mcb_resize(std::uint16_t block_seg, std::uint16_t new_paras,
                  std::uint16_t* out_max_possible = nullptr);

 private:
  enum class HandleKind : std::uint8_t {
    Unused = 0,
    StdConsole,
    NulDevice,
    EmmDevice,
    DiskFile,
    MemoryFile,
  };

  struct FileSlot {
    enum class LastIo : std::uint8_t {
      None = 0,
      Read,
      Write,
    };
    HandleKind kind = HandleKind::Unused;
    LastIo last_io = LastIo::None;
    std::uint16_t owner_psp = 0;
    bool writable = false;
    const std::uint8_t* mem_data = nullptr;
    std::size_t mem_size = 0;
    std::size_t mem_pos = 0;
    std::filesystem::path host_path;
    std::fstream stream;
  };

  struct SavedGeneralRegs {
    std::uint32_t eax = 0;
    std::uint32_t ebx = 0;
    std::uint32_t ecx = 0;
    std::uint32_t edx = 0;
  };

  struct SavedSpecialRegs {
    std::uint32_t esp = 0;
    std::uint32_t ebp = 0;
    std::uint32_t esi = 0;
    std::uint32_t edi = 0;
    std::uint32_t eip = 0;
    std::uint32_t eflags = 0;
  };

  struct SavedProcessFrame {
    SavedGeneralRegs gen{};
    SavedSpecialRegs spc{};
    std::array<std::uint16_t, 6> seg_sels{};
    std::uint16_t psp_seg = 0;
    std::uint16_t dta_seg = 0;
    std::uint16_t dta_off = 0x0080u;
    std::uint8_t mcb_alloc_strategy = 0;
    std::uint8_t umb_link_state = 0;
    std::uint32_t ivt_1c = 0;
    std::uint32_t ivt_22 = 0;
    std::uint32_t ivt_23 = 0;
    std::uint32_t ivt_24 = 0;
    std::uint32_t ivt_3f = 0;
    std::unordered_map<std::uint32_t, std::vector<DosDirMatch>>
        active_find_matches;
  };

  struct LoadedImage {
    std::uint16_t child_psp = 0;
    std::uint16_t alloc_paras = 0;
    std::uint16_t init_cs = 0;
    std::uint16_t init_ip = 0;
    std::uint16_t init_ss = 0;
    std::uint16_t init_sp = 0;
  };

  void init_mcb_chain();
  void coalesce_free_mcbs();
  void free_mcbs_owned_by(std::uint16_t owner_psp);
  void close_files_owned_by(std::uint16_t owner_psp);
  void close_handles_for_host_path(const std::filesystem::path& path);

  std::uint8_t guest_read8(std::uint32_t addr) const;
  void guest_write8(std::uint32_t addr, std::uint8_t v);
  std::uint16_t guest_read16(std::uint32_t addr) const;
  void guest_write16(std::uint32_t addr, std::uint16_t v);
  void guest_read_block(std::uint32_t src, void* dst, std::size_t len) const;
  void guest_write_block(std::uint32_t dst, const void* src, std::size_t len);

  std::string read_asciiz(std::uint16_t seg, std::uint16_t off,
                          std::size_t max_len = 260) const;
  std::uint16_t build_environment_block(std::string_view upper_exe_name);
  bool load_com_image(const std::vector<std::uint8_t>& exe_bytes,
                      std::uint16_t max_free, LoadedImage& out);
  bool load_mz_exe_image(const std::vector<std::uint8_t>& exe_bytes,
                         std::uint16_t max_free, LoadedImage& out);
  bool launch_executable(x86emu_t* emu, std::string_view dos_path,
                         std::string_view raw_cmd_tail, bool is_root_process);
  void terminate_current_process(x86emu_t* emu, std::uint16_t return_code,
                                 bool keep_resident, std::uint16_t keep_paras);
  void write_dta_entry(const DosDirMatch& match, std::uint16_t search_idx);
  BiosKey peek_key();
  BiosKey pop_key();

  void int21_console_io(x86emu_t* emu, std::uint8_t ah, std::uint8_t al);
  void int21_open_or_create(x86emu_t* emu, bool create_mode, std::uint8_t al);
  void int21_read_file(x86emu_t* emu);
  void int21_write_file(x86emu_t* emu);
  void int21_seek_file(x86emu_t* emu, std::uint8_t al);
  void int21_ioctl(x86emu_t* emu, std::uint8_t al);
  void int21_exec(x86emu_t* emu, std::uint8_t al);
  void int21_find_first_next(x86emu_t* emu, std::uint8_t ah);
  void int21_memory_ops(x86emu_t* emu, std::uint8_t ah, std::uint8_t al);

  std::uint8_t ram_read8(std::uint32_t addr) const {
    return (addr < ram_size_) ? guest_ram_[addr] : 0u;
  }
  void ram_write8(std::uint32_t addr, std::uint8_t v) {
    if (addr < ram_size_) {
      guest_ram_[addr] = v;
    }
  }
  std::uint16_t ram_read16(std::uint32_t addr) const {
    return static_cast<std::uint16_t>(ram_read8(addr)) |
           (static_cast<std::uint16_t>(ram_read8(addr + 1u)) << 8);
  }
  void ram_write16(std::uint32_t addr, std::uint16_t v) {
    ram_write8(addr, static_cast<std::uint8_t>(v & 0xFFu));
    ram_write8(addr + 1u, static_cast<std::uint8_t>((v >> 8) & 0xFFu));
  }
  std::uint32_t ram_read32(std::uint32_t addr) const {
    return static_cast<std::uint32_t>(ram_read16(addr)) |
           (static_cast<std::uint32_t>(ram_read16(addr + 2u)) << 16);
  }
  void ram_write32(std::uint32_t addr, std::uint32_t v) {
    ram_write16(addr, static_cast<std::uint16_t>(v & 0xFFFFu));
    ram_write16(addr + 2u, static_cast<std::uint16_t>((v >> 16) & 0xFFFFu));
  }

  CaseInsensitiveVfs& vfs_;
  EmsManager& ems_;
  x86emu_t* emu_ = nullptr;
  std::uint8_t* guest_ram_ = nullptr;
  std::size_t ram_size_ = 0;

  bool skip_intro_ = false;
  bool deterministic_clock_ = false;
  bool program_exited_ = false;
  std::uint16_t current_psp_ = 0;
  std::uint16_t dta_seg_ = 0;
  std::uint16_t dta_off_ = 0x0080u;
  std::uint8_t current_drive_ = 2u;  // C:
  std::uint8_t mcb_alloc_strategy_ = 0u;  // 0=FirstFit, 1=BestFit, 2=LastFit
  std::uint8_t umb_link_state_ = 0u;
  std::uint16_t last_child_return_code_ = 0;
  std::uint16_t grp600c_cs_ = 0;

  std::array<FileSlot, kMaxFiles> files_{};
  std::vector<SavedProcessFrame> process_stack_;
  std::unordered_map<std::uint32_t, std::vector<DosDirMatch>>
      active_find_matches_;
  std::array<char, 65536> io_scratch_{};

  std::deque<BiosKey> key_queue_;
  std::uint8_t kbd_shift_flags_ = 0;
  std::uint64_t file_activity_ = 0;
};

}  // namespace san4
