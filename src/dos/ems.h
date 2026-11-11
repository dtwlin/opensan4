#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

typedef struct x86emu_s x86emu_t;

namespace san4 {

class EmsManager {
 public:
  static constexpr std::uint16_t kPageFrameSeg = 0xD000u;
  static constexpr std::uint16_t kDeviceHeaderSeg = 0xE000u;
  static constexpr std::uint16_t kTotalLogicalPages = 256u;  // 4 MB (16 KB/page)
  static constexpr std::size_t kEmsPageBytes = 16384u;
  static constexpr int kPhysSlots = 4;
  static constexpr int kMaxHandles = 64;

  EmsManager();

  // Attaches libx86emu instance, writes "EMMXXXX0" header at E000:000A, and
  // maps the initial 64 KB page frame at 0xD0000..0xDFFFF.
  void attach(x86emu_t* emu, std::uint8_t* guest_ram);

  // Handles INT 67h (LIM EMS 4.0).
  void handle_int67(x86emu_t* emu);

  // Marks currently allocated EMS handles as TSR-owned so reset_above_tsr()
  // preserves TSR handles across child stages if any.
  void mark_tsr_watermark();
  void reset_above_tsr();

  std::uint16_t free_pages() const;

 private:
  struct HandleEntry {
    bool allocated = false;
    bool tsr_owned = false;
    std::array<char, 8> name{};
    std::vector<std::uint16_t> pages;
    bool has_saved_map = false;
    std::array<std::int32_t, kPhysSlots> saved_phys_pages{-1, -1, -1, -1};
  };

  bool is_valid_pool_page_entry(std::int32_t pool_page) const;
  void remap_phys_slot(int phys_slot, std::int32_t pool_page);
  bool map_handle_page(int phys_slot, std::uint16_t handle,
                       std::uint16_t log_page);

  void handle_page_map_4eh(x86emu_t* emu);
  void handle_map_multiple_50h(x86emu_t* emu);
  void handle_move_exchange_57h(x86emu_t* emu);

  x86emu_t* emu_ = nullptr;
  alignas(4096) std::vector<std::uint8_t> pool_;
  alignas(4096) std::array<std::uint8_t, kPhysSlots * kEmsPageBytes>
      unmapped_frame_{};
  std::array<bool, kTotalLogicalPages> page_in_use_{};
  std::array<HandleEntry, kMaxHandles> handles_{};
  std::array<std::int32_t, kPhysSlots> active_pool_page_{-1, -1, -1, -1};
  std::vector<std::uint8_t> move_src_buf_;
  std::vector<std::uint8_t> move_dst_buf_;
};

}  // namespace san4
