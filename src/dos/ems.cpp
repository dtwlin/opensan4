#include "dos/ems.h"

#include <algorithm>
#include <cstring>

#include "x86emu_clean.h"

namespace san4 {

EmsManager::EmsManager()
    : pool_(static_cast<std::size_t>(kTotalLogicalPages) * kEmsPageBytes, 0u) {
  // Reserve handle 0 as standard OS system handle (0 pages).
  handles_[0].allocated = true;
  handles_[0].tsr_owned = true;
}

void EmsManager::attach(x86emu_t* emu, std::uint8_t* guest_ram) {
  emu_ = emu;
  if (guest_ram != nullptr) {
    const std::uint32_t hdr_base =
        static_cast<std::uint32_t>(kDeviceHeaderSeg) << 4;
    guest_ram[hdr_base + 0x00] = 0xCFu;  // IRET at E000:0000
    std::memcpy(guest_ram + hdr_base + 0x0A, "EMMXXXX0", 8);
  }
  for (int slot = 0; slot < kPhysSlots; ++slot) {
    remap_phys_slot(slot, -1);
  }
}

bool EmsManager::is_valid_pool_page_entry(std::int32_t pool_page) const {
  return pool_page == -1 ||
         (pool_page >= 0 &&
          pool_page < static_cast<std::int32_t>(kTotalLogicalPages) &&
          page_in_use_[static_cast<std::size_t>(pool_page)]);
}

void EmsManager::remap_phys_slot(int phys_slot, std::int32_t pool_page) {
  if (phys_slot < 0 || phys_slot >= kPhysSlots) {
    return;
  }
  const std::int32_t effective_page =
      is_valid_pool_page_entry(pool_page) ? pool_page : -1;
  active_pool_page_[phys_slot] = effective_page;
  if (emu_ == nullptr) {
    return;
  }
  std::uint8_t* base_ptr =
      (effective_page >= 0)
          ? (pool_.data() +
             static_cast<std::size_t>(effective_page) * kEmsPageBytes)
          : (unmapped_frame_.data() +
             static_cast<std::size_t>(phys_slot) * kEmsPageBytes);

  const unsigned base_4k_page =
      0xD0u + static_cast<unsigned>(phys_slot) * 4u;
  for (unsigned sub = 0; sub < 4u; ++sub) {
    const unsigned page_byte_off = (base_4k_page + sub) * 4096u;
    x86emu_set_page(emu_, page_byte_off, base_ptr + sub * 4096u);
    x86emu_set_perm(emu_, page_byte_off, page_byte_off + 4096u - 1u,
                    X86EMU_PERM_RWX | X86EMU_PERM_VALID);
  }
}

std::uint16_t EmsManager::free_pages() const {
  std::uint16_t cnt = 0;
  for (bool used : page_in_use_) {
    if (!used) {
      ++cnt;
    }
  }
  return cnt;
}

bool EmsManager::map_handle_page(int phys_slot, std::uint16_t handle,
                                 std::uint16_t log_page) {
  if (phys_slot < 0 || phys_slot >= kPhysSlots) {
    return false;
  }
  if (log_page == 0xFFFFu) {
    remap_phys_slot(phys_slot, -1);
    return true;
  }
  if (handle >= kMaxHandles || !handles_[handle].allocated) {
    return false;
  }
  if (log_page >= handles_[handle].pages.size()) {
    return false;
  }
  remap_phys_slot(phys_slot,
                  static_cast<std::int32_t>(handles_[handle].pages[log_page]));
  return true;
}

void EmsManager::mark_tsr_watermark() {
  for (auto& h : handles_) {
    if (h.allocated) {
      h.tsr_owned = true;
    }
  }
}

void EmsManager::reset_above_tsr() {
  for (int slot = 0; slot < kPhysSlots; ++slot) {
    remap_phys_slot(slot, -1);
  }
  for (int i = 1; i < kMaxHandles; ++i) {
    if (handles_[i].allocated && !handles_[i].tsr_owned) {
      for (std::uint16_t pg : handles_[i].pages) {
        if (pg < kTotalLogicalPages) {
          page_in_use_[pg] = false;
        }
      }
      handles_[i] = HandleEntry{};
    }
  }
}

void EmsManager::handle_page_map_4eh(x86emu_t* emu) {
  const std::uint8_t al = emu->x86.R_AL;
  if (al == 0x03u) {
    emu->x86.R_AL = 16u;
    emu->x86.R_AH = 0x00u;
    return;
  }
  if (al > 0x03u) {
    emu->x86.R_AH = 0x8Fu;
    return;
  }

  std::array<std::int32_t, kPhysSlots> next_pages{-1, -1, -1, -1};
  if (al == 0x01u || al == 0x02u) {
    const std::uint32_t src =
        (static_cast<std::uint32_t>(emu->x86.R_DS) << 4) + emu->x86.R_SI;
    for (int s = 0; s < kPhysSlots; ++s) {
      next_pages[s] = static_cast<std::int32_t>(
          x86emu_read_dword(emu, src + static_cast<unsigned>(s) * 4u));
      if (!is_valid_pool_page_entry(next_pages[s])) {
        emu->x86.R_AH = 0xA3u;  // Corrupted save area
        return;
      }
    }
  }

  if (al == 0x00u || al == 0x02u) {
    const std::uint32_t dst =
        (static_cast<std::uint32_t>(emu->x86.R_ES) << 4) + emu->x86.R_DI;
    for (int s = 0; s < kPhysSlots; ++s) {
      x86emu_write_dword(emu, dst + static_cast<unsigned>(s) * 4u,
                         static_cast<unsigned>(active_pool_page_[s]));
    }
  }

  if (al == 0x01u || al == 0x02u) {
    for (int s = 0; s < kPhysSlots; ++s) {
      remap_phys_slot(s, next_pages[s]);
    }
  }
  emu->x86.R_AH = 0x00u;
}

void EmsManager::handle_map_multiple_50h(x86emu_t* emu) {
  const std::uint8_t al = emu->x86.R_AL;
  if (al > 0x01u) {
    emu->x86.R_AH = 0x8Fu;
    return;
  }
  const std::uint16_t handle = emu->x86.R_DX;
  if (handle >= kMaxHandles || !handles_[handle].allocated) {
    emu->x86.R_AH = 0x83u;
    return;
  }
  const std::uint16_t count = emu->x86.R_CX;
  const std::uint32_t src =
      (static_cast<std::uint32_t>(emu->x86.R_DS) << 4) + emu->x86.R_SI;

  for (std::uint16_t i = 0; i < count; ++i) {
    const auto log_pg =
        static_cast<std::uint16_t>(x86emu_read_word(emu, src + i * 4u));
    const auto phys_id =
        static_cast<std::uint16_t>(x86emu_read_word(emu, src + i * 4u + 2u));

    int slot = -1;
    if (al == 0x00u) {
      if (phys_id >= static_cast<std::uint16_t>(kPhysSlots)) {
        emu->x86.R_AH = 0x8Bu;
        return;
      }
      slot = static_cast<int>(phys_id);
    } else {
      // Physical segment address (0xD000, 0xD400, 0xD800, 0xDC00)
      if (phys_id < kPageFrameSeg ||
          ((phys_id - kPageFrameSeg) & 0x03FFu) != 0u) {
        emu->x86.R_AH = 0x8Bu;
        return;
      }
      slot = static_cast<int>((phys_id - kPageFrameSeg) >> 10);
      if (slot < 0 || slot >= kPhysSlots) {
        emu->x86.R_AH = 0x8Bu;
        return;
      }
    }

    if (!map_handle_page(slot, handle, log_pg)) {
      emu->x86.R_AH = 0x8Au;
      return;
    }
  }
  emu->x86.R_AH = 0x00u;
}

void EmsManager::handle_move_exchange_57h(x86emu_t* emu) {
  const std::uint8_t al = emu->x86.R_AL;
  if (al > 0x01u) {
    emu->x86.R_AH = 0x8Fu;
    return;
  }
  const std::uint32_t desc =
      (static_cast<std::uint32_t>(emu->x86.R_DS) << 4) + emu->x86.R_SI;
  const std::uint32_t length = x86emu_read_dword(emu, desc + 0x00u);
  if (length == 0u) {
    emu->x86.R_AH = 0x00u;
    return;
  }
  if (length > 0x100000u) {
    emu->x86.R_AH = 0x96u;
    return;
  }

  const std::uint8_t src_type = x86emu_read_byte_noperm(emu, desc + 0x04u);
  const auto src_handle =
      static_cast<std::uint16_t>(x86emu_read_word(emu, desc + 0x05u));
  const auto src_off =
      static_cast<std::uint16_t>(x86emu_read_word(emu, desc + 0x07u));
  const auto src_seg_pg =
      static_cast<std::uint16_t>(x86emu_read_word(emu, desc + 0x09u));

  const std::uint8_t dst_type = x86emu_read_byte_noperm(emu, desc + 0x0Bu);
  const auto dst_handle =
      static_cast<std::uint16_t>(x86emu_read_word(emu, desc + 0x0Cu));
  const auto dst_off =
      static_cast<std::uint16_t>(x86emu_read_word(emu, desc + 0x0Eu));
  const auto dst_seg_pg =
      static_cast<std::uint16_t>(x86emu_read_word(emu, desc + 0x10u));

  if (src_type > 1u || dst_type > 1u) {
    emu->x86.R_AH = 0x98u;
    return;
  }

  auto check_endpoint = [&](std::uint8_t type, std::uint16_t h,
                            std::uint16_t seg_pg,
                            std::uint16_t off) -> std::uint8_t {
    if (type == 0u) {
      const std::uint64_t lin_start =
          (static_cast<std::uint64_t>(seg_pg) << 4) + off;
      if (lin_start + length > 0x100000u) {
        return 0xA2u;  // Attempt to wrap past 1 MiB conventional boundary
      }
      return 0x00u;
    }
    if (h >= kMaxHandles || !handles_[h].allocated) {
      return 0x83u;
    }
    if (off >= kEmsPageBytes) {
      return 0x95u;
    }
    const std::uint64_t end_off =
        static_cast<std::uint64_t>(seg_pg) * kEmsPageBytes + off + length;
    const std::uint64_t cap =
        static_cast<std::uint64_t>(handles_[h].pages.size()) * kEmsPageBytes;
    if (end_off > cap) {
      return 0x93u;
    }
    return 0x00u;
  };

  std::uint8_t err = check_endpoint(src_type, src_handle, src_seg_pg, src_off);
  if (err == 0x00u) {
    err = check_endpoint(dst_type, dst_handle, dst_seg_pg, dst_off);
  }
  if (err != 0x00u) {
    emu->x86.R_AH = err;
    return;
  }

  // Detect overlap across conventional<->conventional, EMS<->EMS, and
  // conventional page-frame (0xD0000..0xDFFFF)<->EMS regions.
  auto conv_ems_overlap = [&](std::uint32_t conv_lin, std::uint16_t ems_h,
                              std::uint64_t ems_pos) -> bool {
    const std::uint64_t conv_end = static_cast<std::uint64_t>(conv_lin) + length;
    const std::uint64_t frame_lo = std::max<std::uint64_t>(conv_lin, 0xD0000u);
    const std::uint64_t frame_hi = std::min<std::uint64_t>(conv_end, 0xE0000u);
    if (frame_lo >= frame_hi) {
      return false;
    }
    const std::uint64_t ems_end = ems_pos + length;
    const std::size_t first_lp = static_cast<std::size_t>(ems_pos / kEmsPageBytes);
    const std::size_t last_lp =
        static_cast<std::size_t>((ems_end - 1u) / kEmsPageBytes);
    for (int s = 0; s < kPhysSlots; ++s) {
      const std::int32_t mapped_pg = active_pool_page_[s];
      if (mapped_pg < 0) {
        continue;
      }
      const std::uint64_t slot_base =
          0xD0000u + static_cast<std::uint64_t>(s) * kEmsPageBytes;
      const std::uint64_t c0 = std::max(frame_lo, slot_base);
      const std::uint64_t c1 = std::min(frame_hi, slot_base + kEmsPageBytes);
      if (c0 >= c1) {
        continue;
      }
      const std::uint64_t sub_c0 = c0 - slot_base;
      const std::uint64_t sub_c1 = c1 - slot_base;
      for (std::size_t lp = first_lp; lp <= last_lp; ++lp) {
        if (handles_[ems_h].pages[lp] != static_cast<std::uint16_t>(mapped_pg)) {
          continue;
        }
        const std::uint64_t lp_base = lp * kEmsPageBytes;
        const std::uint64_t e0 = (ems_pos > lp_base) ? (ems_pos - lp_base) : 0u;
        const std::uint64_t e1 =
            std::min<std::uint64_t>(kEmsPageBytes, ems_end - lp_base);
        if (sub_c0 < e1 && e0 < sub_c1) {
          return true;
        }
      }
    }
    return false;
  };

  bool overlaps = false;
  if (src_type == 0u && dst_type == 0u) {
    const std::uint64_t src_lin =
        (static_cast<std::uint64_t>(src_seg_pg) << 4) + src_off;
    const std::uint64_t dst_lin =
        (static_cast<std::uint64_t>(dst_seg_pg) << 4) + dst_off;
    overlaps = (src_lin < dst_lin + length) && (dst_lin < src_lin + length);
  } else if (src_type == 1u && dst_type == 1u) {
    if (src_handle == dst_handle) {
      const std::uint64_t src_pos =
          static_cast<std::uint64_t>(src_seg_pg) * kEmsPageBytes + src_off;
      const std::uint64_t dst_pos =
          static_cast<std::uint64_t>(dst_seg_pg) * kEmsPageBytes + dst_off;
      overlaps = (src_pos < dst_pos + length) && (dst_pos < src_pos + length);
    }
  } else if (src_type == 0u && dst_type == 1u) {
    const std::uint32_t src_lin =
        (static_cast<std::uint32_t>(src_seg_pg) << 4) + src_off;
    const std::uint64_t dst_pos =
        static_cast<std::uint64_t>(dst_seg_pg) * kEmsPageBytes + dst_off;
    overlaps = conv_ems_overlap(src_lin, dst_handle, dst_pos);
  } else {
    const std::uint64_t src_pos =
        static_cast<std::uint64_t>(src_seg_pg) * kEmsPageBytes + src_off;
    const std::uint32_t dst_lin =
        (static_cast<std::uint32_t>(dst_seg_pg) << 4) + dst_off;
    overlaps = conv_ems_overlap(dst_lin, src_handle, src_pos);
  }

  if (overlaps && al == 0x01u) {
    emu->x86.R_AH = 0x97u;  // Overlapping exchange is invalid; abort cleanly
    return;
  }

  auto rd_byte = [&](std::uint8_t type, std::uint16_t h, std::uint16_t seg_pg,
                     std::uint16_t off, std::uint32_t i) -> std::uint8_t {
    if (type == 0u) {
      const std::uint32_t lin =
          (static_cast<std::uint32_t>(seg_pg) << 4) + off + i;
      return x86emu_read_byte_noperm(emu, lin);
    }
    const std::size_t pos =
        static_cast<std::size_t>(seg_pg) * kEmsPageBytes + off + i;
    const std::uint16_t pool_pg = handles_[h].pages[pos / kEmsPageBytes];
    return pool_[static_cast<std::size_t>(pool_pg) * kEmsPageBytes +
                 (pos % kEmsPageBytes)];
  };

  auto wr_byte = [&](std::uint8_t type, std::uint16_t h, std::uint16_t seg_pg,
                     std::uint16_t off, std::uint32_t i, std::uint8_t v) {
    if (type == 0u) {
      const std::uint32_t lin =
          (static_cast<std::uint32_t>(seg_pg) << 4) + off + i;
      x86emu_write_byte_noperm(emu, lin, v);
      return;
    }
    const std::size_t pos =
        static_cast<std::size_t>(seg_pg) * kEmsPageBytes + off + i;
    const std::uint16_t pool_pg = handles_[h].pages[pos / kEmsPageBytes];
    pool_[static_cast<std::size_t>(pool_pg) * kEmsPageBytes +
          (pos % kEmsPageBytes)] = v;
  };

  move_src_buf_.resize(length);
  for (std::uint32_t i = 0; i < length; ++i) {
    move_src_buf_[i] = rd_byte(src_type, src_handle, src_seg_pg, src_off, i);
  }
  if (al == 0x01u) {
    move_dst_buf_.resize(length);
    for (std::uint32_t i = 0; i < length; ++i) {
      move_dst_buf_[i] = rd_byte(dst_type, dst_handle, dst_seg_pg, dst_off, i);
    }
    for (std::uint32_t i = 0; i < length; ++i) {
      wr_byte(src_type, src_handle, src_seg_pg, src_off, i, move_dst_buf_[i]);
    }
  }
  for (std::uint32_t i = 0; i < length; ++i) {
    wr_byte(dst_type, dst_handle, dst_seg_pg, dst_off, i, move_src_buf_[i]);
  }
  emu->x86.R_AH = (al == 0x00u && overlaps) ? 0x92u : 0x00u;
}

void EmsManager::handle_int67(x86emu_t* emu) {
  const std::uint8_t ah = emu->x86.R_AH;
  const std::uint8_t al = emu->x86.R_AL;

  switch (ah) {
    case 0x40u:  // Get Status
      emu->x86.R_AH = 0x00u;
      break;

    case 0x41u:  // Get Page Frame Segment
      emu->x86.R_BX = kPageFrameSeg;
      emu->x86.R_AH = 0x00u;
      break;

    case 0x42u:  // Get Unallocated Page Count
      emu->x86.R_BX = free_pages();
      emu->x86.R_DX = kTotalLogicalPages;
      emu->x86.R_AH = 0x00u;
      break;

    case 0x43u: {  // Allocate Pages (BX = pages)
      const std::uint16_t req = emu->x86.R_BX;
      if (req == 0u) {
        emu->x86.R_AH = 0x89u;
        break;
      }
      if (req > free_pages()) {
        emu->x86.R_AH = 0x88u;
        break;
      }
      int h_idx = -1;
      for (int i = 1; i < kMaxHandles; ++i) {
        if (!handles_[i].allocated) {
          h_idx = i;
          break;
        }
      }
      if (h_idx < 0) {
        emu->x86.R_AH = 0x85u;
        break;
      }
      auto& h = handles_[h_idx];
      h = HandleEntry{};
      h.allocated = true;
      h.pages.reserve(req);
      for (std::uint16_t pg = 0; pg < kTotalLogicalPages && h.pages.size() < req;
           ++pg) {
        if (!page_in_use_[pg]) {
          page_in_use_[pg] = true;
          std::memset(pool_.data() + static_cast<std::size_t>(pg) * kEmsPageBytes,
                      0, kEmsPageBytes);
          h.pages.push_back(pg);
        }
      }
      emu->x86.R_DX = static_cast<std::uint16_t>(h_idx);
      emu->x86.R_AH = 0x00u;
      break;
    }

    case 0x44u: {  // Map/Unmap Handle Page
      if (al >= kPhysSlots) {
        emu->x86.R_AH = 0x8Bu;
        break;
      }
      const std::uint16_t handle = emu->x86.R_DX;
      const std::uint16_t log_pg = emu->x86.R_BX;
      if (handle >= kMaxHandles || !handles_[handle].allocated) {
        emu->x86.R_AH = 0x83u;
        break;
      }
      if (!map_handle_page(al, handle, log_pg)) {
        emu->x86.R_AH = 0x8Au;
        break;
      }
      emu->x86.R_AH = 0x00u;
      break;
    }

    case 0x45u: {  // Deallocate Pages
      const std::uint16_t handle = emu->x86.R_DX;
      if (handle == 0u || handle >= kMaxHandles || !handles_[handle].allocated) {
        emu->x86.R_AH = 0x83u;
        break;
      }
      for (std::uint16_t pg : handles_[handle].pages) {
        for (int s = 0; s < kPhysSlots; ++s) {
          if (active_pool_page_[s] == static_cast<std::int32_t>(pg)) {
            remap_phys_slot(s, -1);
          }
        }
        if (pg < kTotalLogicalPages) {
          page_in_use_[pg] = false;
        }
      }
      handles_[handle] = HandleEntry{};
      emu->x86.R_AH = 0x00u;
      break;
    }

    case 0x46u:  // Get Version (EMS 4.0)
      emu->x86.R_AL = 0x40u;
      emu->x86.R_AH = 0x00u;
      break;

    case 0x47u: {  // Save Page Map
      const std::uint16_t handle = emu->x86.R_DX;
      if (handle >= kMaxHandles || !handles_[handle].allocated) {
        emu->x86.R_AH = 0x83u;
        break;
      }
      if (handles_[handle].has_saved_map) {
        emu->x86.R_AH = 0x8Du;
        break;
      }
      handles_[handle].saved_phys_pages = active_pool_page_;
      handles_[handle].has_saved_map = true;
      emu->x86.R_AH = 0x00u;
      break;
    }

    case 0x48u: {  // Restore Page Map
      const std::uint16_t handle = emu->x86.R_DX;
      if (handle >= kMaxHandles || !handles_[handle].allocated) {
        emu->x86.R_AH = 0x83u;
        break;
      }
      if (!handles_[handle].has_saved_map) {
        emu->x86.R_AH = 0x8Eu;
        break;
      }
      for (int s = 0; s < kPhysSlots; ++s) {
        remap_phys_slot(s, handles_[handle].saved_phys_pages[s]);
      }
      handles_[handle].has_saved_map = false;
      emu->x86.R_AH = 0x00u;
      break;
    }

    case 0x4Bu: {  // Get Handle Count
      std::uint16_t cnt = 0;
      for (const auto& h : handles_) {
        if (h.allocated) {
          ++cnt;
        }
      }
      emu->x86.R_BX = cnt;
      emu->x86.R_AH = 0x00u;
      break;
    }

    case 0x4Cu: {  // Get Handle Pages
      const std::uint16_t handle = emu->x86.R_DX;
      if (handle >= kMaxHandles || !handles_[handle].allocated) {
        emu->x86.R_AH = 0x83u;
        break;
      }
      emu->x86.R_BX = static_cast<std::uint16_t>(handles_[handle].pages.size());
      emu->x86.R_AH = 0x00u;
      break;
    }

    case 0x4Du: {  // Get All Handle Pages -> ES:DI
      const std::uint32_t dst =
          (static_cast<std::uint32_t>(emu->x86.R_ES) << 4) + emu->x86.R_DI;
      std::uint16_t cnt = 0;
      for (std::uint16_t i = 0; i < kMaxHandles; ++i) {
        if (handles_[i].allocated) {
          x86emu_write_word(emu, dst + cnt * 4u + 0u, i);
          x86emu_write_word(
              emu, dst + cnt * 4u + 2u,
              static_cast<std::uint16_t>(handles_[i].pages.size()));
          ++cnt;
        }
      }
      emu->x86.R_BX = cnt;
      emu->x86.R_AH = 0x00u;
      break;
    }

    case 0x4Eu:
      handle_page_map_4eh(emu);
      break;

    case 0x50u:
      handle_map_multiple_50h(emu);
      break;

    case 0x52u: {  // Get/Set Handle Attribute (volatile handles supported)
      if (al == 0x00u || al == 0x02u) {
        emu->x86.R_AL = 0x00u;
        emu->x86.R_AH = 0x00u;
      } else if (al == 0x01u && emu->x86.R_BL == 0x00u) {
        emu->x86.R_AH = 0x00u;
      } else {
        emu->x86.R_AH = 0x91u;
      }
      break;
    }

    case 0x53u: {  // Get/Set Handle Name
      const std::uint16_t handle = emu->x86.R_DX;
      if (handle >= kMaxHandles || !handles_[handle].allocated) {
        emu->x86.R_AH = 0x83u;
        break;
      }
      if (al == 0x00u) {
        const std::uint32_t dst =
            (static_cast<std::uint32_t>(emu->x86.R_ES) << 4) + emu->x86.R_DI;
        for (unsigned i = 0; i < 8u; ++i) {
          x86emu_write_byte_noperm(
              emu, dst + i, static_cast<std::uint8_t>(handles_[handle].name[i]));
        }
        emu->x86.R_AH = 0x00u;
      } else if (al == 0x01u) {
        const std::uint32_t src =
            (static_cast<std::uint32_t>(emu->x86.R_DS) << 4) + emu->x86.R_SI;
        for (unsigned i = 0; i < 8u; ++i) {
          handles_[handle].name[i] =
              static_cast<char>(x86emu_read_byte_noperm(emu, src + i));
        }
        emu->x86.R_AH = 0x00u;
      } else {
        emu->x86.R_AH = 0x8Fu;
      }
      break;
    }

    case 0x57u:
      handle_move_exchange_57h(emu);
      break;

    case 0x58u: {  // Get Mappable Physical Address Array
      if (al == 0x00u) {
        const std::uint32_t dst =
            (static_cast<std::uint32_t>(emu->x86.R_ES) << 4) + emu->x86.R_DI;
        for (std::uint16_t s = 0; s < kPhysSlots; ++s) {
          x86emu_write_word(
              emu, dst + s * 4u + 0u,
              static_cast<unsigned>(kPageFrameSeg + s * 0x0400u));
          x86emu_write_word(emu, dst + s * 4u + 2u, s);
        }
      }
      emu->x86.R_CX = kPhysSlots;
      emu->x86.R_AH = (al <= 0x01u) ? 0x00u : 0x8Fu;
      break;
    }

    default:
      emu->x86.R_AH = 0x84u;
      break;
  }
}

}  // namespace san4
