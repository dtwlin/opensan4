#include "dos/dos_kernel.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "game_manifest.h"
#include "grp600c_defs.h"
#include "util/string_util.h"
#include "x86emu_clean.h"

namespace san4 {
namespace {

bool trace_dos_enabled() {
  static const bool enabled = (std::getenv("SAN4_TRACE") != nullptr);
  return enabled;
}

void set_cf(x86emu_t* emu, bool carry) {
  if (carry) {
    emu->x86.R_EFLG |= FB_CF;
  } else {
    emu->x86.R_EFLG &= ~static_cast<unsigned>(FB_CF);
  }
}

void set_zf(x86emu_t* emu, bool zero) {
  if (zero) {
    emu->x86.R_EFLG |= FB_ZF;
  } else {
    emu->x86.R_EFLG &= ~static_cast<unsigned>(FB_ZF);
  }
}

// Updates ZF in live R_EFLG (when `on_interrupt` returns 1, libx86emu does not
// push an IRET frame onto SS:SP; R_EFLG is returned directly to the guest).
void set_iret_zf(x86emu_t* emu, bool zero) { set_zf(emu, zero); }

std::uint16_t read_le16(const std::vector<std::uint8_t>& buf, std::size_t off) {
  return static_cast<std::uint16_t>(buf[off]) |
         (static_cast<std::uint16_t>(buf[off + 1u]) << 8);
}

}  // namespace

DosKernel::DosKernel(CaseInsensitiveVfs& vfs, EmsManager& ems)
    : vfs_(vfs), ems_(ems) {}

void DosKernel::attach(x86emu_t* emu, std::uint8_t* guest_ram,
                       std::size_t ram_size) {
  emu_ = emu;
  guest_ram_ = guest_ram;
  ram_size_ = ram_size;
  program_exited_ = false;
  process_stack_.clear();
  active_find_matches_.clear();
  grp600c_cs_ = 0;
  mcb_alloc_strategy_ = 0u;
  umb_link_state_ = 0u;

  for (auto& f : files_) {
    if (f.stream.is_open()) {
      f.stream.close();
    }
    f = FileSlot{};
  }
  // Standard handles 0..4 (stdin, stdout, stderr, stdaux, stdprn)
  for (int i = 0; i < 5; ++i) {
    files_[i].kind = HandleKind::StdConsole;
    files_[i].owner_psp = 0x0008u;
    files_[i].writable = (i != 0);
  }

  // Initialize BDA keyboard circular buffer (0040:001A..003D, bounds 0080/0082)
  if (guest_ram_ != nullptr && ram_size_ >= 0x0500u) {
    ram_write16(0x041Au, 0x001Eu);
    ram_write16(0x041Cu, 0x001Eu);
    ram_write16(0x0480u, 0x001Eu);
    ram_write16(0x0482u, 0x003Eu);
    ram_write8(0x0417u, kbd_shift_flags_);
  }
  pump_bda_keyboard();

  init_mcb_chain();
}

void DosKernel::init_mcb_chain() {
  const std::uint32_t mcb = static_cast<std::uint32_t>(kFirstMcbSeg) << 4;
  const std::uint16_t total_paras =
      static_cast<std::uint16_t>(kConventionalTopSeg - kFirstMcbSeg - 1u);
  ram_write8(mcb + 0u, 'Z');
  ram_write16(mcb + 1u, 0x0000u);  // Free
  ram_write16(mcb + 3u, total_paras);
  for (std::uint32_t i = 5; i < 16; ++i) {
    ram_write8(mcb + i, 0u);
  }
}

void DosKernel::coalesce_free_mcbs() {
  std::uint16_t cur = kFirstMcbSeg;
  for (int guard = 0; guard < 4096; ++guard) {
    const std::uint32_t addr = static_cast<std::uint32_t>(cur) << 4;
    const std::uint8_t tag = ram_read8(addr + 0u);
    if (tag != 'M' && tag != 'Z') {
      break;
    }
    const std::uint16_t owner = ram_read16(addr + 1u);
    std::uint16_t sz = ram_read16(addr + 3u);
    if (tag == 'Z') {
      break;
    }
    const std::uint16_t next = static_cast<std::uint16_t>(cur + 1u + sz);
    if (next >= kConventionalTopSeg) {
      break;
    }
    const std::uint32_t next_addr = static_cast<std::uint32_t>(next) << 4;
    const std::uint8_t next_tag = ram_read8(next_addr + 0u);
    const std::uint16_t next_owner = ram_read16(next_addr + 1u);
    const std::uint16_t next_sz = ram_read16(next_addr + 3u);
    if (owner == 0u && next_owner == 0u &&
        (next_tag == 'M' || next_tag == 'Z')) {
      sz = static_cast<std::uint16_t>(sz + 1u + next_sz);
      ram_write8(addr + 0u, next_tag);
      ram_write16(addr + 3u, sz);
      continue;  // Re-check merged block against following block
    }
    cur = next;
  }
}

std::optional<std::uint16_t> DosKernel::mcb_alloc(
    std::uint16_t paras, std::uint16_t owner_psp,
    std::uint16_t* out_max_free) {
  coalesce_free_mcbs();
  std::uint16_t max_free = 0;
  std::uint16_t cur = kFirstMcbSeg;
  const std::uint8_t mode =
      static_cast<std::uint8_t>(mcb_alloc_strategy_ & 0x3Fu);

  bool have_cand = false;
  std::uint16_t cand_cur = 0;
  std::uint16_t cand_sz = 0;
  std::uint8_t cand_tag = 'M';

  for (int guard = 0; guard < 4096; ++guard) {
    const std::uint32_t addr = static_cast<std::uint32_t>(cur) << 4;
    const std::uint8_t tag = ram_read8(addr + 0u);
    if (tag != 'M' && tag != 'Z') {
      break;
    }
    const std::uint16_t owner = ram_read16(addr + 1u);
    const std::uint16_t sz = ram_read16(addr + 3u);
    if (owner == 0u) {
      max_free = std::max(max_free, sz);
      if (sz >= paras) {
        if (!have_cand) {
          have_cand = true;
          cand_cur = cur;
          cand_sz = sz;
          cand_tag = tag;
        } else if (mode == 1u && sz < cand_sz) {
          // Best Fit: smallest suitable free MCB
          cand_cur = cur;
          cand_sz = sz;
          cand_tag = tag;
        } else if (mode >= 2u) {
          // Last Fit: highest suitable free MCB
          cand_cur = cur;
          cand_sz = sz;
          cand_tag = tag;
        }
      }
    }
    if (tag == 'Z') {
      break;
    }
    cur = static_cast<std::uint16_t>(cur + 1u + sz);
  }

  if (out_max_free) {
    *out_max_free = max_free;
  }
  if (!have_cand) {
    return std::nullopt;
  }

  const std::uint32_t cand_addr = static_cast<std::uint32_t>(cand_cur) << 4;
  if (mode >= 2u && cand_sz >= static_cast<std::uint32_t>(paras) + 1u) {
    // Last Fit (High): carve `paras` from the top end of `cand_cur` so lower
    // conventional heap blocks (e.g. MSC 7.0 sbrk below MOVE overlay buffer)
    // can grow upward via INT 21h AH=4Ah!
    const std::uint16_t rem = static_cast<std::uint16_t>(cand_sz - paras - 1u);
    const std::uint16_t alloc_mcb =
        static_cast<std::uint16_t>(cand_cur + 1u + rem);
    const std::uint32_t alloc_addr =
        static_cast<std::uint32_t>(alloc_mcb) << 4;

    ram_write8(cand_addr + 0u, 'M');
    ram_write16(cand_addr + 1u, 0x0000u);
    ram_write16(cand_addr + 3u, rem);

    ram_write8(alloc_addr + 0u, cand_tag);
    ram_write16(alloc_addr + 1u, owner_psp);
    ram_write16(alloc_addr + 3u, paras);
    for (std::uint32_t i = 5; i < 16; ++i) {
      ram_write8(alloc_addr + i, 0u);
    }
    return static_cast<std::uint16_t>(alloc_mcb + 1u);
  }

  ram_write16(cand_addr + 1u, owner_psp);
  if (cand_sz > static_cast<std::uint32_t>(paras) + 1u) {
    const std::uint16_t rem = static_cast<std::uint16_t>(cand_sz - paras - 1u);
    const std::uint16_t split_mcb =
        static_cast<std::uint16_t>(cand_cur + 1u + paras);
    const std::uint32_t split_addr =
        static_cast<std::uint32_t>(split_mcb) << 4;
    ram_write8(cand_addr + 0u, 'M');
    ram_write16(cand_addr + 3u, paras);

    ram_write8(split_addr + 0u, cand_tag);
    ram_write16(split_addr + 1u, 0x0000u);
    ram_write16(split_addr + 3u, rem);
  }
  return static_cast<std::uint16_t>(cand_cur + 1u);
}

bool DosKernel::mcb_free(std::uint16_t block_seg) {
  if (block_seg <= kFirstMcbSeg || block_seg >= kConventionalTopSeg) {
    return false;
  }
  const std::uint32_t mcb_addr =
      static_cast<std::uint32_t>(block_seg - 1u) << 4;
  const std::uint8_t tag = ram_read8(mcb_addr + 0u);
  if (tag != 'M' && tag != 'Z') {
    return false;
  }
  ram_write16(mcb_addr + 1u, 0x0000u);
  coalesce_free_mcbs();
  return true;
}

bool DosKernel::mcb_resize(std::uint16_t block_seg, std::uint16_t new_paras,
                           std::uint16_t* out_max_possible) {
  if (block_seg <= kFirstMcbSeg || block_seg >= kConventionalTopSeg) {
    if (out_max_possible) *out_max_possible = 0;
    return false;
  }
  coalesce_free_mcbs();
  const std::uint16_t mcb_seg = static_cast<std::uint16_t>(block_seg - 1u);
  const std::uint32_t addr = static_cast<std::uint32_t>(mcb_seg) << 4;
  std::uint8_t tag = ram_read8(addr + 0u);
  if (tag != 'M' && tag != 'Z') {
    if (out_max_possible) *out_max_possible = 0;
    return false;
  }
  std::uint16_t cur_sz = ram_read16(addr + 3u);
  std::uint16_t max_possible = cur_sz;

  // Check if immediate successor is free so block can grow into it
  if (tag == 'M') {
    const std::uint16_t next_seg =
        static_cast<std::uint16_t>(mcb_seg + 1u + cur_sz);
    if (next_seg < kConventionalTopSeg) {
      const std::uint32_t next_addr = static_cast<std::uint32_t>(next_seg) << 4;
      const std::uint8_t next_tag = ram_read8(next_addr + 0u);
      const std::uint16_t next_owner = ram_read16(next_addr + 1u);
      const std::uint16_t next_sz = ram_read16(next_addr + 3u);
      if (next_owner == 0u && (next_tag == 'M' || next_tag == 'Z')) {
        max_possible = static_cast<std::uint16_t>(cur_sz + 1u + next_sz);
        if (new_paras > cur_sz && new_paras <= max_possible) {
          tag = next_tag;
          cur_sz = max_possible;
          ram_write8(addr + 0u, tag);
          ram_write16(addr + 3u, cur_sz);
        }
      }
    }
  }

  if (out_max_possible) {
    *out_max_possible = max_possible;
  }
  if (new_paras > cur_sz) {
    return false;
  }

  if (cur_sz > static_cast<std::uint32_t>(new_paras) + 1u) {
    const std::uint16_t rem =
        static_cast<std::uint16_t>(cur_sz - new_paras - 1u);
    const std::uint16_t split_seg =
        static_cast<std::uint16_t>(mcb_seg + 1u + new_paras);
    const std::uint32_t split_addr = static_cast<std::uint32_t>(split_seg) << 4;
    ram_write8(addr + 0u, 'M');
    ram_write16(addr + 3u, new_paras);

    ram_write8(split_addr + 0u, tag);
    ram_write16(split_addr + 1u, 0x0000u);
    ram_write16(split_addr + 3u, rem);
    coalesce_free_mcbs();
  }

  // Keep PSP:[0x0002] top-of-memory field synchronized when resizing a PSP!
  if (block_seg == current_psp_) {
    const std::uint32_t psp_addr = static_cast<std::uint32_t>(block_seg) << 4;
    ram_write16(psp_addr + 0x02u,
                static_cast<std::uint16_t>(block_seg + ram_read16(addr + 3u)));
  }
  return true;
}

void DosKernel::free_mcbs_owned_by(std::uint16_t owner_psp) {
  if (owner_psp == 0u) {
    return;
  }
  std::uint16_t cur = kFirstMcbSeg;
  for (int guard = 0; guard < 4096; ++guard) {
    const std::uint32_t addr = static_cast<std::uint32_t>(cur) << 4;
    const std::uint8_t tag = ram_read8(addr + 0u);
    if (tag != 'M' && tag != 'Z') {
      break;
    }
    if (ram_read16(addr + 1u) == owner_psp) {
      ram_write16(addr + 1u, 0x0000u);
    }
    if (tag == 'Z') {
      break;
    }
    cur = static_cast<std::uint16_t>(cur + 1u + ram_read16(addr + 3u));
  }
  coalesce_free_mcbs();
}

void DosKernel::close_files_owned_by(std::uint16_t owner_psp) {
  for (int i = 5; i < kMaxFiles; ++i) {
    if (files_[i].kind != HandleKind::Unused &&
        files_[i].owner_psp == owner_psp) {
      if (files_[i].stream.is_open()) {
        files_[i].stream.flush();
        files_[i].stream.close();
      }
      files_[i] = FileSlot{};
    }
  }
}

void DosKernel::close_handles_for_host_path(const std::filesystem::path& path) {
  if (path.empty()) {
    return;
  }
  for (int i = 5; i < kMaxFiles; ++i) {
    if (files_[i].kind != HandleKind::DiskFile || files_[i].host_path.empty()) {
      continue;
    }
    std::error_code ec;
    const bool same =
        std::filesystem::equivalent(files_[i].host_path, path, ec)
            ? true
            : (!ec ? false : (files_[i].host_path == path));
    if (same) {
      if (files_[i].stream.is_open()) {
        files_[i].stream.flush();
        files_[i].stream.close();
      }
      files_[i] = FileSlot{};
    }
  }
}

std::uint8_t DosKernel::guest_read8(std::uint32_t addr) const {
  if (addr >= ram_size_) {
    return 0xFFu;
  }
  if (addr >= 0x00400u && addr < 0xA0000u) {
    return guest_ram_[addr];
  }
  if (emu_ != nullptr) {
    return x86emu_read_byte(emu_, addr);
  }
  return guest_ram_[addr];
}

void DosKernel::guest_write8(std::uint32_t addr, std::uint8_t v) {
  if (addr >= ram_size_) {
    return;
  }
  if (addr >= 0x00400u && addr < 0xA0000u) {
    guest_ram_[addr] = v;
    return;
  }
  if (emu_ != nullptr) {
    x86emu_write_byte(emu_, addr, v);
    return;
  }
  if (addr >= 0xF0000u && addr < 0x100000u) {
    return;
  }
  guest_ram_[addr] = v;
}

std::uint16_t DosKernel::guest_read16(std::uint32_t addr) const {
  return static_cast<std::uint16_t>(guest_read8(addr)) |
         (static_cast<std::uint16_t>(guest_read8(addr + 1u)) << 8);
}

void DosKernel::guest_write16(std::uint32_t addr, std::uint16_t v) {
  guest_write8(addr, static_cast<std::uint8_t>(v & 0xFFu));
  guest_write8(addr + 1u, static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}

void DosKernel::guest_read_block(std::uint32_t src, void* dst,
                                 std::size_t len) const {
  if (len == 0u || dst == nullptr) {
    return;
  }
  auto* out = static_cast<std::uint8_t*>(dst);
  if (src >= ram_size_) {
    std::memset(out, 0xFF, len);
    return;
  }
  const std::size_t avail =
      std::min<std::size_t>(len, ram_size_ - static_cast<std::size_t>(src));
  if (src >= 0x00400u && (static_cast<std::size_t>(src) + avail) <= 0xA0000u) {
    std::memcpy(out, guest_ram_ + src, avail);
  } else {
    for (std::size_t i = 0u; i < avail; ++i) {
      out[i] = guest_read8(src + static_cast<std::uint32_t>(i));
    }
  }
  if (avail < len) {
    std::memset(out + avail, 0xFF, len - avail);
  }
}

void DosKernel::guest_write_block(std::uint32_t dst, const void* src,
                                  std::size_t len) {
  if (len == 0u || src == nullptr || dst >= ram_size_) {
    return;
  }
  const auto* in = static_cast<const std::uint8_t*>(src);
  const std::size_t avail =
      std::min<std::size_t>(len, ram_size_ - static_cast<std::size_t>(dst));
  if (dst >= 0x00400u && (static_cast<std::size_t>(dst) + avail) <= 0xA0000u) {
    std::memcpy(guest_ram_ + dst, in, avail);
  } else {
    for (std::size_t i = 0u; i < avail; ++i) {
      guest_write8(dst + static_cast<std::uint32_t>(i), in[i]);
    }
  }
}

std::string DosKernel::read_asciiz(std::uint16_t seg, std::uint16_t off,
                                   std::size_t max_len) const {
  std::string s;
  const std::uint32_t base = (static_cast<std::uint32_t>(seg) << 4) + off;
  for (std::size_t i = 0; i < max_len; ++i) {
    const std::uint8_t c = guest_read8(base + static_cast<std::uint32_t>(i));
    if (c == 0u || (base + static_cast<std::uint32_t>(i) >= ram_size_)) {
      break;
    }
    s.push_back(static_cast<char>(c));
  }
  return s;
}

std::uint16_t DosKernel::build_environment_block(
    std::string_view upper_exe_name) {
  // Format required by MS C 7.0 MOVE overlay manager:
  //   "PATH=C:\\\0COMSPEC=C:\\COMMAND.COM\0\0" + uint16(1) + "C:\\<EXE>\0"
  std::vector<std::uint8_t> env;
  const char kVars[] = "PATH=C:\\\0COMSPEC=C:\\COMMAND.COM\0";
  env.insert(env.end(), kVars, kVars + sizeof(kVars));  // ends with double \0
  env.push_back(0x01u);
  env.push_back(0x00u);
  const std::string prog_path = "C:\\" + std::string(upper_exe_name);
  env.insert(env.end(), prog_path.begin(), prog_path.end());
  env.push_back(0x00u);

  const auto paras =
      static_cast<std::uint16_t>((env.size() + 15u) / 16u);
  const std::uint16_t alloc_p = std::max<std::uint16_t>(paras, 4u);
  auto seg_opt = mcb_alloc(alloc_p, 0xFFFEu);
  if (!seg_opt) {
    return 0u;
  }
  const std::size_t base = static_cast<std::size_t>(*seg_opt) << 4;
  const std::size_t byte_len = static_cast<std::size_t>(alloc_p) * 16u;
  if (base + byte_len > ram_size_) {
    mcb_free(*seg_opt);
    return 0u;
  }
  std::memset(guest_ram_ + base, 0, byte_len);
  std::memcpy(guest_ram_ + base, env.data(), env.size());
  return *seg_opt;
}

bool DosKernel::boot_program(std::string_view dos_prog_name,
                             std::string_view cmd_tail) {
  if (emu_ == nullptr || guest_ram_ == nullptr) {
    return false;
  }
  std::string formatted_tail;
  formatted_tail.push_back(
      static_cast<char>(std::min<std::size_t>(126, cmd_tail.size())));
  formatted_tail.append(cmd_tail.substr(0, 126));
  formatted_tail.push_back('\r');
  return launch_executable(emu_, dos_prog_name, formatted_tail, true);
}

bool DosKernel::load_com_image(const std::vector<std::uint8_t>& exe_bytes,
                               std::uint16_t max_free, LoadedImage& out) {
  if (exe_bytes.empty() || exe_bytes.size() > 0xFE00u) {
    return false;
  }
  const std::size_t min_needed = (exe_bytes.size() + 256u + 512u + 15u) / 16u;
  if (static_cast<std::size_t>(max_free) < min_needed) {
    return false;
  }

  const std::uint16_t alloc_paras = max_free;
  auto seg_opt = mcb_alloc(alloc_paras, 0xFFFDu);
  if (!seg_opt) {
    return false;
  }

  const std::uint16_t child_psp = *seg_opt;
  const std::size_t psp_lin = static_cast<std::size_t>(child_psp) << 4;
  const std::size_t load_addr = psp_lin + 0x0100u;
  const std::size_t block_end =
      psp_lin + static_cast<std::size_t>(alloc_paras) * 16u;
  if (load_addr + exe_bytes.size() > block_end || block_end > ram_size_) {
    mcb_free(child_psp);
    return false;
  }

  std::memset(guest_ram_ + psp_lin, 0, block_end - psp_lin);
  std::memcpy(guest_ram_ + load_addr, exe_bytes.data(), exe_bytes.size());

  out.child_psp = child_psp;
  out.alloc_paras = alloc_paras;
  out.init_cs = child_psp;
  out.init_ip = 0x0100u;
  out.init_ss = child_psp;
  out.init_sp = (alloc_paras >= 0x1000u)
                    ? 0xFFFEu
                    : static_cast<std::uint16_t>(alloc_paras * 16u - 2u);
  ram_write16((static_cast<std::uint32_t>(out.init_ss) << 4) + out.init_sp,
              0x0000u);
  return true;
}

bool DosKernel::load_mz_exe_image(const std::vector<std::uint8_t>& exe_bytes,
                                  std::uint16_t max_free, LoadedImage& out) {
  if (exe_bytes.size() < 0x1Cu) {
    return false;
  }
  const std::uint16_t e_cblp = read_le16(exe_bytes, 0x02u);
  const std::uint16_t e_cp = read_le16(exe_bytes, 0x04u);
  const std::uint16_t e_crlc = read_le16(exe_bytes, 0x06u);
  const std::uint16_t e_cparhdr = read_le16(exe_bytes, 0x08u);
  const std::uint16_t e_minalloc = read_le16(exe_bytes, 0x0Au);
  const std::uint16_t e_maxalloc = read_le16(exe_bytes, 0x0Cu);
  const std::uint16_t e_ss = read_le16(exe_bytes, 0x0Eu);
  const std::uint16_t e_sp = read_le16(exe_bytes, 0x10u);
  const std::uint16_t e_ip = read_le16(exe_bytes, 0x14u);
  const std::uint16_t e_cs = read_le16(exe_bytes, 0x16u);
  const std::uint16_t e_lfarlc = read_le16(exe_bytes, 0x18u);

  if (e_cp == 0u || e_cblp > 512u) {
    return false;
  }
  const std::size_t hdr_bytes = static_cast<std::size_t>(e_cparhdr) * 16u;
  std::size_t declared_total =
      static_cast<std::size_t>(e_cp - 1u) * 512u +
      (e_cblp == 0u ? 512u : static_cast<std::size_t>(e_cblp));
  declared_total = std::min(declared_total, exe_bytes.size());
  if (declared_total <= hdr_bytes) {
    return false;
  }

  const std::size_t img_bytes = declared_total - hdr_bytes;
  const std::size_t img_paras = (img_bytes + 15u) / 16u;
  const std::size_t min_paras =
      16u + img_paras + static_cast<std::size_t>(e_minalloc);
  std::size_t want_paras =
      (e_maxalloc == 0u)
          ? min_paras
          : (16u + img_paras + static_cast<std::size_t>(e_maxalloc));
  want_paras = std::max(min_paras, want_paras);

  if (static_cast<std::size_t>(max_free) < min_paras) {
    return false;
  }

  // Validate relocation table file bounds before allocating guest memory.
  const std::size_t rel_table_end =
      static_cast<std::size_t>(e_lfarlc) + static_cast<std::size_t>(e_crlc) * 4u;
  if (e_crlc > 0u && rel_table_end > exe_bytes.size()) {
    return false;
  }

  const auto alloc_paras =
      static_cast<std::uint16_t>(std::min<std::size_t>(max_free, want_paras));
  auto seg_opt = mcb_alloc(alloc_paras, 0xFFFDu);
  if (!seg_opt) {
    return false;
  }

  const std::uint16_t child_psp = *seg_opt;
  const std::uint16_t load_seg = static_cast<std::uint16_t>(child_psp + 16u);
  const std::size_t psp_lin = static_cast<std::size_t>(child_psp) << 4;
  const std::size_t load_lin = static_cast<std::size_t>(load_seg) << 4;
  const std::size_t block_end =
      psp_lin + static_cast<std::size_t>(alloc_paras) * 16u;

  if (block_end > ram_size_ || load_lin + img_bytes > block_end) {
    mcb_free(child_psp);
    return false;
  }

  // Pre-validate every relocation linear address before modifying guest RAM.
  for (std::uint16_t r = 0; r < e_crlc; ++r) {
    const std::size_t roff =
        static_cast<std::size_t>(e_lfarlc) + static_cast<std::size_t>(r) * 4u;
    const std::uint16_t item_off = read_le16(exe_bytes, roff + 0u);
    const std::uint16_t item_seg = read_le16(exe_bytes, roff + 2u);
    const std::uint32_t rel_lin =
        (static_cast<std::uint32_t>(load_seg) + item_seg) * 16u + item_off;
    if (rel_lin < load_lin ||
        static_cast<std::size_t>(rel_lin) + 2u > block_end) {
      mcb_free(child_psp);
      return false;
    }
  }

  std::memset(guest_ram_ + psp_lin, 0, block_end - psp_lin);
  std::memcpy(guest_ram_ + load_lin, exe_bytes.data() + hdr_bytes, img_bytes);

  for (std::uint16_t r = 0; r < e_crlc; ++r) {
    const std::size_t roff =
        static_cast<std::size_t>(e_lfarlc) + static_cast<std::size_t>(r) * 4u;
    const std::uint16_t item_off = read_le16(exe_bytes, roff + 0u);
    const std::uint16_t item_seg = read_le16(exe_bytes, roff + 2u);
    const std::uint32_t rel_lin =
        (static_cast<std::uint32_t>(load_seg) + item_seg) * 16u + item_off;
    ram_write16(rel_lin,
                static_cast<std::uint16_t>(ram_read16(rel_lin) + load_seg));
  }

  out.child_psp = child_psp;
  out.alloc_paras = alloc_paras;
  out.init_cs = static_cast<std::uint16_t>(load_seg + e_cs);
  out.init_ip = e_ip;
  out.init_ss = static_cast<std::uint16_t>(load_seg + e_ss);
  out.init_sp = e_sp;
  return true;
}

bool DosKernel::launch_executable(x86emu_t* emu, std::string_view dos_path,
                                  std::string_view raw_cmd_tail,
                                  bool is_root_process) {
  const std::string upper_base = CaseInsensitiveVfs::dos_basename_upper(dos_path);
  if (trace_dos_enabled()) {
    std::cerr << "[dos-trace] EXEC '" << dos_path << "' (upper='" << upper_base
              << "', root=" << is_root_process << ")\n";
  }
  if (!is_root_process && skip_intro_ && upper_base == "OPEN.EXE") {
    // Cleanly bypass intro stage when --skip-intro is requested: SAN486.COM
    // checks AH=4Dh == 0 and proceeds directly to MAIN.EXE with BGM enabled.
    last_child_return_code_ = 0x0000u;
    return true;
  }

  auto host_exe = vfs_.resolve_for_read(dos_path);
  if (!host_exe) {
    return false;
  }

  std::ifstream ifs(*host_exe, std::ios::binary | std::ios::ate);
  if (!ifs) {
    return false;
  }
  const auto end_pos = ifs.tellg();
  if (end_pos < 0) {
    return false;
  }
  const auto fsize = static_cast<std::size_t>(end_pos);
  if (fsize == 0u) {
    return false;
  }
  ifs.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> exe_bytes(fsize);
  if (!ifs.read(reinterpret_cast<char*>(exe_bytes.data()),
                static_cast<std::streamsize>(fsize))) {
    return false;
  }
  ++file_activity_;

  const bool has_mz_sig =
      exe_bytes.size() >= 2u &&
      ((exe_bytes[0] == 'M' && exe_bytes[1] == 'Z') ||
       (exe_bytes[0] == 'Z' && exe_bytes[1] == 'M'));
  if (has_mz_sig && exe_bytes.size() < 0x1Cu) {
    return false;
  }

  const std::uint16_t env_seg = build_environment_block(upper_base);
  if (env_seg == 0u) {
    return false;
  }

  std::uint16_t max_free = 0;
  mcb_alloc(0xFFFFu, 0xFFFFu, &max_free);

  LoadedImage img{};
  const bool loaded_ok =
      has_mz_sig ? load_mz_exe_image(exe_bytes, max_free, img)
                 : load_com_image(exe_bytes, max_free, img);
  if (!loaded_ok) {
    mcb_free(env_seg);
    return false;
  }

  const std::uint16_t child_psp = img.child_psp;
  const std::uint16_t alloc_paras = img.alloc_paras;

  // Assign true child PSP ownership to both MCBs
  ram_write16((static_cast<std::uint32_t>(env_seg - 1u) << 4) + 1u, child_psp);
  ram_write16((static_cast<std::uint32_t>(child_psp - 1u) << 4) + 1u, child_psp);

  // Initialize 256-byte PSP header
  const std::uint32_t psp_base = static_cast<std::uint32_t>(child_psp) << 4;
  ram_write8(psp_base + 0x00u, 0xCDu);  // INT 20h
  ram_write8(psp_base + 0x01u, 0x20u);
  ram_write16(psp_base + 0x02u,
              static_cast<std::uint16_t>(child_psp + alloc_paras));
  ram_write32(psp_base + 0x0Au, ram_read32(0x22u * 4u));
  ram_write32(psp_base + 0x0Eu, ram_read32(0x23u * 4u));
  ram_write32(psp_base + 0x12u, ram_read32(0x24u * 4u));
  ram_write16(psp_base + 0x16u, is_root_process ? child_psp : current_psp_);
  ram_write16(psp_base + 0x2Cu, env_seg);

  if (raw_cmd_tail.empty()) {
    ram_write8(psp_base + 0x80u, 0x00u);
    ram_write8(psp_base + 0x81u, 0x0Du);
  } else {
    const std::size_t copy_n = std::min<std::size_t>(128u, raw_cmd_tail.size());
    std::memcpy(guest_ram_ + psp_base + 0x80u, raw_cmd_tail.data(), copy_n);
  }

  if (!is_root_process) {
    SavedProcessFrame frame{};
    frame.gen = {
        .eax = emu->x86.R_EAX,
        .ebx = emu->x86.R_EBX,
        .ecx = emu->x86.R_ECX,
        .edx = emu->x86.R_EDX,
    };
    frame.spc = {
        .esp = emu->x86.R_ESP,
        .ebp = emu->x86.R_EBP,
        .esi = emu->x86.R_ESI,
        .edi = emu->x86.R_EDI,
        .eip = emu->x86.R_EIP,
        .eflags = emu->x86.R_EFLG,
    };
    frame.seg_sels = {
        static_cast<std::uint16_t>(emu->x86.seg[R_ES_INDEX].sel),
        static_cast<std::uint16_t>(emu->x86.seg[R_CS_INDEX].sel),
        static_cast<std::uint16_t>(emu->x86.seg[R_SS_INDEX].sel),
        static_cast<std::uint16_t>(emu->x86.seg[R_DS_INDEX].sel),
        static_cast<std::uint16_t>(emu->x86.seg[R_FS_INDEX].sel),
        static_cast<std::uint16_t>(emu->x86.seg[R_GS_INDEX].sel),
    };
    frame.psp_seg = current_psp_;
    frame.dta_seg = dta_seg_;
    frame.dta_off = dta_off_;
    frame.mcb_alloc_strategy = mcb_alloc_strategy_;
    frame.umb_link_state = umb_link_state_;
    frame.ivt_1c = ram_read32(0x1Cu * 4u);
    frame.ivt_22 = ram_read32(0x22u * 4u);
    frame.ivt_23 = ram_read32(0x23u * 4u);
    frame.ivt_24 = ram_read32(0x24u * 4u);
    frame.ivt_3f = ram_read32(0x3Fu * 4u);
    frame.active_find_matches = active_find_matches_;
    process_stack_.push_back(std::move(frame));
  }

  current_psp_ = child_psp;
  dta_seg_ = child_psp;
  dta_off_ = 0x0080u;

  x86emu_set_seg_register(emu, emu->x86.R_CS_SEL, img.init_cs);
  x86emu_set_seg_register(emu, emu->x86.R_SS_SEL, img.init_ss);
  x86emu_set_seg_register(emu, emu->x86.R_DS_SEL, child_psp);
  x86emu_set_seg_register(emu, emu->x86.R_ES_SEL, child_psp);
  emu->x86.R_EIP = img.init_ip;
  emu->x86.R_ESP = img.init_sp;
  emu->x86.R_EAX = 0x0000u;
  emu->x86.R_EBX = 0x0000u;
  emu->x86.R_ECX = 0x00FFu;
  emu->x86.R_EDX = child_psp;
  emu->x86.R_ESI = img.init_ip;
  emu->x86.R_EDI = img.init_sp;
  emu->x86.R_EBP = 0x091Cu;
  emu->x86.R_EFLG = F_ALWAYS_ON | FB_IF;
  return true;
}

void DosKernel::terminate_current_process(x86emu_t* emu,
                                          std::uint16_t return_code,
                                          bool keep_resident,
                                          std::uint16_t keep_paras) {
  if (trace_dos_enabled()) {
    std::cerr << "[dos-trace] TERM psp=0x" << std::hex << current_psp_
              << " rc=0x" << return_code << " tsr=" << keep_resident
              << " keep_paras=0x" << keep_paras << std::dec << "\n";
  }
  last_child_return_code_ = return_code;
  const std::uint16_t exiting_psp = current_psp_;

  if (keep_resident) {
    mcb_resize(exiting_psp, std::max<std::uint16_t>(keep_paras, 16u));
    ems_.mark_tsr_watermark();

    // Check if this TSR is GRP600C.EXE by verifying the 5-byte dispatcher
    // signature (`E9 14 23 90 90`) at CS:0x0018 before patching RAM.
    const std::uint16_t cand_cs = static_cast<std::uint16_t>(exiting_psp + 16u);
    const std::uint32_t cand_lin = static_cast<std::uint32_t>(cand_cs) << 4;
    if (cand_lin + kGrpMinSegSpan <= ram_size_) {
      std::span<const std::uint8_t> view(guest_ram_ + cand_lin, kGrpMinSegSpan);
      if (has_expected_grp600c_dispatch_signature(view)) {
        grp600c_cs_ = cand_cs;
        // Patch CS:0x0018 (`jmp 0x232f; nop; nop`) -> `int 0xF1; jmp 0x232f`
        guest_ram_[cand_lin + kGrpEntryOff + 0u] = 0xCDu;
        guest_ram_[cand_lin + kGrpEntryOff + 1u] = 0xF1u;
        guest_ram_[cand_lin + kGrpEntryOff + 2u] = 0xE9u;
        guest_ram_[cand_lin + kGrpEntryOff + 3u] = 0x12u;
        guest_ram_[cand_lin + kGrpEntryOff + 4u] = 0x23u;
      }
    }
  } else {
    close_files_owned_by(exiting_psp);
    free_mcbs_owned_by(exiting_psp);
    ems_.reset_above_tsr();
  }

  if (process_stack_.empty()) {
    program_exited_ = true;
    x86emu_stop(emu);
    return;
  }

  SavedProcessFrame parent = std::move(process_stack_.back());
  process_stack_.pop_back();

  if (!keep_resident) {
    mcb_alloc_strategy_ = parent.mcb_alloc_strategy;
    umb_link_state_ = parent.umb_link_state;
    active_find_matches_ = std::move(parent.active_find_matches);

    ram_write32(0x22u * 4u, parent.ivt_22);
    ram_write32(0x23u * 4u, parent.ivt_23);
    ram_write32(0x24u * 4u, parent.ivt_24);

    // Restore INT 1Ch / INT 3Fh if the child hooked them and didn't unhook.
    const std::uint16_t cur_1c_seg = ram_read16(0x1Cu * 4u + 2u);
    if (cur_1c_seg >= exiting_psp) {
      ram_write32(0x1Cu * 4u, parent.ivt_1c);
    }
    const std::uint16_t cur_3f_seg = ram_read16(0x3Fu * 4u + 2u);
    if (cur_3f_seg >= exiting_psp) {
      ram_write32(0x3Fu * 4u, parent.ivt_3f);
    }
  }

  current_psp_ = parent.psp_seg;
  dta_seg_ = parent.dta_seg;
  dta_off_ = parent.dta_off;

  emu->x86.R_EAX = parent.gen.eax;
  emu->x86.R_EBX = parent.gen.ebx;
  emu->x86.R_ECX = parent.gen.ecx;
  emu->x86.R_EDX = parent.gen.edx;
  emu->x86.R_ESP = parent.spc.esp;
  emu->x86.R_EBP = parent.spc.ebp;
  emu->x86.R_ESI = parent.spc.esi;
  emu->x86.R_EDI = parent.spc.edi;
  emu->x86.R_EIP = parent.spc.eip;
  emu->x86.R_EFLG = parent.spc.eflags;

  x86emu_set_seg_register(emu, emu->x86.R_ES_SEL, parent.seg_sels[R_ES_INDEX]);
  x86emu_set_seg_register(emu, emu->x86.R_CS_SEL, parent.seg_sels[R_CS_INDEX]);
  x86emu_set_seg_register(emu, emu->x86.R_SS_SEL, parent.seg_sels[R_SS_INDEX]);
  x86emu_set_seg_register(emu, emu->x86.R_DS_SEL, parent.seg_sels[R_DS_INDEX]);
  x86emu_set_seg_register(emu, emu->x86.R_FS_SEL, parent.seg_sels[R_FS_INDEX]);
  x86emu_set_seg_register(emu, emu->x86.R_GS_SEL, parent.seg_sels[R_GS_INDEX]);
  emu->x86.R_AX = 0x0000u;
  set_cf(emu, false);
}

void DosKernel::write_dta_entry(const DosDirMatch& match,
                                std::uint16_t search_idx) {
  const std::uint32_t dta =
      (static_cast<std::uint32_t>(dta_seg_) << 4) + dta_off_;
  guest_write16(dta + 0x13u, search_idx);
  std::array<std::uint8_t, 22> rec{};
  rec[0] = match.attr;
  rec[1] = static_cast<std::uint8_t>(match.dos_time & 0xFFu);
  rec[2] = static_cast<std::uint8_t>((match.dos_time >> 8) & 0xFFu);
  rec[3] = static_cast<std::uint8_t>(match.dos_date & 0xFFu);
  rec[4] = static_cast<std::uint8_t>((match.dos_date >> 8) & 0xFFu);
  rec[5] = static_cast<std::uint8_t>(match.size & 0xFFu);
  rec[6] = static_cast<std::uint8_t>((match.size >> 8) & 0xFFu);
  rec[7] = static_cast<std::uint8_t>((match.size >> 16) & 0xFFu);
  rec[8] = static_cast<std::uint8_t>((match.size >> 24) & 0xFFu);
  for (std::size_t i = 0u; i < 12u && i < match.dos_name.size(); ++i) {
    rec[9u + i] = static_cast<std::uint8_t>(match.dos_name[i]);
  }
  guest_write_block(dta + 0x15u, rec.data(), rec.size());
}

bool DosKernel::bda_has_key() const {
  if (guest_ram_ == nullptr || ram_size_ < 0x0500u) {
    return false;
  }
  return ram_read16(0x041Au) != ram_read16(0x041Cu);
}

void DosKernel::pump_bda_keyboard() {
  if (guest_ram_ == nullptr || ram_size_ < 0x0500u || bda_has_key() ||
      key_queue_.empty()) {
    return;
  }
  std::uint16_t head = ram_read16(0x041Au);
  if (head < 0x001Eu || head >= 0x003Eu || (head & 1u) != 0u) {
    head = 0x001Eu;
    ram_write16(0x041Au, head);
  }
  const BiosKey k = key_queue_.front();
  key_queue_.pop_front();
  const std::uint16_t word =
      static_cast<std::uint16_t>(k.ascii) |
      (static_cast<std::uint16_t>(k.scancode) << 8);
  ram_write16(0x0400u + head, word);
  const std::uint16_t next_tail =
      (head + 2u >= 0x003Eu) ? 0x001Eu : static_cast<std::uint16_t>(head + 2u);
  ram_write16(0x041Cu, next_tail);
}

void DosKernel::set_shift_flags(std::uint8_t flags) {
  kbd_shift_flags_ = flags;
  if (guest_ram_ != nullptr && ram_size_ >= 0x0500u) {
    ram_write8(0x0417u, flags);
  }
}

bool DosKernel::has_keys() const {
  return bda_has_key() || !key_queue_.empty();
}

void DosKernel::clear_keys() {
  key_queue_.clear();
  if (guest_ram_ != nullptr && ram_size_ >= 0x0500u) {
    ram_write16(0x041Au, 0x001Eu);
    ram_write16(0x041Cu, 0x001Eu);
  }
}

BiosKey DosKernel::peek_key() {
  pump_bda_keyboard();
  if (bda_has_key()) {
    const std::uint16_t head = ram_read16(0x041Au);
    if (head >= 0x001Eu && head < 0x003Eu) {
      const std::uint16_t w = ram_read16(0x0400u + head);
      return BiosKey{
          .ascii = static_cast<std::uint8_t>(w & 0xFFu),
          .scancode = static_cast<std::uint8_t>(w >> 8),
      };
    }
  }
  if (!key_queue_.empty()) {
    return key_queue_.front();
  }
  return BiosKey{};
}

BiosKey DosKernel::pop_key() {
  pump_bda_keyboard();
  if (bda_has_key()) {
    const std::uint16_t head = ram_read16(0x041Au);
    const std::uint16_t tail = ram_read16(0x041Cu);
    std::uint16_t w = 0u;
    if (head >= 0x001Eu && head < 0x003Eu) {
      w = ram_read16(0x0400u + head);
    }
    const std::uint16_t adv =
        (head + 2u >= 0x003Eu) ? 0x001Eu : static_cast<std::uint16_t>(head + 2u);
    ram_write16(0x041Au, (adv == tail) ? tail : adv);
    pump_bda_keyboard();
    return BiosKey{
        .ascii = static_cast<std::uint8_t>(w & 0xFFu),
        .scancode = static_cast<std::uint8_t>(w >> 8),
    };
  }
  if (!key_queue_.empty()) {
    const BiosKey k = key_queue_.front();
    key_queue_.pop_front();
    return k;
  }
  return BiosKey{};
}

void DosKernel::push_key(std::uint8_t ascii, std::uint8_t scancode) {
  if (key_queue_.size() < 64u) {
    key_queue_.push_back(BiosKey{ascii, scancode});
  }
  pump_bda_keyboard();
}

void DosKernel::handle_int16(x86emu_t* emu, bool& out_was_poll) {
  out_was_poll = false;
  const std::uint8_t ah = emu->x86.R_AH;

  switch (ah) {
    case 0x00u:
    case 0x10u: {  // Blocking Read Keystroke
      pump_bda_keyboard();
      if (!has_keys()) {
        // Rewind EIP by 2 bytes (`CD 16`) and yield slice so host events and
        // PIT timers run cleanly without hanging!
        out_was_poll = true;
        emu->x86.R_IP = static_cast<std::uint16_t>(emu->x86.R_IP - 2u);
        x86emu_stop(emu);
        return;
      }
      const BiosKey k = pop_key();
      emu->x86.R_AL = k.ascii;
      emu->x86.R_AH = k.scancode;
      break;
    }

    case 0x01u:
    case 0x11u: {  // Non-Blocking Peek Keystroke (ZF=1 if empty)
      out_was_poll = true;
      pump_bda_keyboard();
      if (!has_keys()) {
        emu->x86.R_AX = 0x0000u;
        set_iret_zf(emu, true);
      } else {
        const BiosKey k = peek_key();
        emu->x86.R_AL = k.ascii;
        emu->x86.R_AH = k.scancode;
        set_iret_zf(emu, false);
      }
      break;
    }

    case 0x02u:
    case 0x12u:  // Get Shift Flags
      emu->x86.R_AL = kbd_shift_flags_;
      emu->x86.R_AH = 0x00u;
      break;

    case 0x03u:  // Set Typematic Rate (no-op)
    case 0x05u:  // Push Keystroke
      if (ah == 0x05u) {
        push_key(emu->x86.R_CL, emu->x86.R_CH);
        emu->x86.R_AL = 0x00u;
      }
      break;

    default:
      break;
  }
}

void DosKernel::handle_int20(x86emu_t* emu) {
  terminate_current_process(emu, 0x0000u, false, 0u);
}

void DosKernel::int21_console_io(x86emu_t* emu, std::uint8_t ah,
                                 std::uint8_t al) {
  switch (ah) {
    case 0x01u:
    case 0x07u:
    case 0x08u: {  // Char Input (with/without echo in GUI mode)
      pump_bda_keyboard();
      if (!has_keys()) {
        emu->x86.R_IP = static_cast<std::uint16_t>(emu->x86.R_IP - 2u);
        x86emu_stop(emu);
        return;
      }
      emu->x86.R_AL = pop_key().ascii;
      break;
    }

    case 0x02u:  // Write Char to Stdout
      emu->x86.R_AL = emu->x86.R_DL;
      break;

    case 0x06u: {  // Direct Console I/O
      if (emu->x86.R_DL == 0xFFu) {
        pump_bda_keyboard();
        if (!has_keys()) {
          emu->x86.R_AL = 0x00u;
          set_iret_zf(emu, true);
        } else {
          emu->x86.R_AL = pop_key().ascii;
          set_iret_zf(emu, false);
        }
      } else {
        emu->x86.R_AL = emu->x86.R_DL;
      }
      break;
    }

    case 0x09u:  // Write '$'-terminated String (discard silently in GUI mode)
      emu->x86.R_AL = '$';
      break;

    case 0x0Au: {  // Buffered Keyboard Input (DS:DX = [max_len, actual_len, ...])
      const std::uint32_t buf =
          (static_cast<std::uint32_t>(emu->x86.R_DS) << 4) + emu->x86.R_DX;
      const std::uint8_t max_chars = guest_read8(buf + 0u);
      if (max_chars == 0u || buf >= ram_size_) {
        break;
      }
      pump_bda_keyboard();
      if (!has_keys()) {
        emu->x86.R_IP = static_cast<std::uint16_t>(emu->x86.R_IP - 2u);
        x86emu_stop(emu);
        return;
      }
      const std::size_t cap =
          (max_chars > 1u) ? static_cast<std::size_t>(max_chars - 1u) : 0u;
      std::vector<std::uint8_t> line;
      line.reserve(cap + 1u);
      while (has_keys()) {
        const std::uint8_t ch = pop_key().ascii;
        if (ch == '\r' || ch == '\n') {
          break;
        }
        if (ch == 0x08u) {
          if (!line.empty()) {
            line.pop_back();
          }
          continue;
        }
        if (ch != 0u && line.size() < cap) {
          line.push_back(ch);
        }
      }
      guest_write8(buf + 1u, static_cast<std::uint8_t>(line.size()));
      line.push_back(0x0Du);
      guest_write_block(buf + 2u, line.data(), line.size());
      break;
    }

    case 0x0Bu:  // Check Stdin Status
      pump_bda_keyboard();
      emu->x86.R_AL = has_keys() ? 0xFFu : 0x00u;
      break;

    case 0x0Cu:  // Flush Keyboard Buffer and Read
      clear_keys();
      if (al == 0x01u || al == 0x06u || al == 0x07u || al == 0x08u ||
          al == 0x0Au) {
        emu->x86.R_AH = al;
        int21_console_io(emu, al, al);
      }
      break;

    default:
      break;
  }
}

void DosKernel::int21_open_or_create(x86emu_t* emu, bool create_mode,
                                     std::uint8_t al) {
  const std::string path = read_asciiz(emu->x86.R_DS, emu->x86.R_DX);
  if (CaseInsensitiveVfs::is_rejected_device(path)) {
    emu->x86.R_AX = 2u;  // File not found (reject MS$KANJI)
    set_cf(emu, true);
    return;
  }

  int slot = -1;
  for (int i = 5; i < kMaxFiles; ++i) {
    if (files_[i].kind == HandleKind::Unused) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    emu->x86.R_AX = 4u;  // Too many open files
    set_cf(emu, true);
    return;
  }

  if (CaseInsensitiveVfs::is_emm_device(path)) {
    files_[slot] = FileSlot{
        .kind = HandleKind::EmmDevice,
        .owner_psp = current_psp_,
        .writable = false,
        .host_path = {},
        .stream = {},
    };
    emu->x86.R_AX = static_cast<std::uint16_t>(slot);
    set_cf(emu, false);
    return;
  }
  if (CaseInsensitiveVfs::is_nul_or_con_device(path)) {
    files_[slot] = FileSlot{
        .kind = HandleKind::NulDevice,
        .owner_psp = current_psp_,
        .writable = true,
        .host_path = {},
        .stream = {},
    };
    emu->x86.R_AX = static_cast<std::uint16_t>(slot);
    set_cf(emu, false);
    return;
  }

  const std::uint8_t access = static_cast<std::uint8_t>(al & 0x07u);
  const bool want_write = create_mode || (access == 1u) || (access == 2u);
  const std::string base_upper = CaseInsensitiveVfs::dos_basename_upper(path);
  const bool is_save_file =
      (base_upper == "SAVEDAT.S4" || base_upper == "NBDATA.S4");
  std::optional<std::filesystem::path> resolved =
      want_write ? vfs_.resolve_for_write(path, create_mode)
                 : vfs_.resolve_for_read(path);

  // If game opens a read-only asset with AL=02h (R/W) without mutating it,
  // fall back to read-only open ONLY when not SAVEDAT.S4 / NBDATA.S4.
  bool opened_writable = want_write;
  std::fstream fs;
  if (resolved) {
    if (create_mode) {
      fs.open(*resolved,
              std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
    } else if (want_write) {
      fs.open(*resolved, std::ios::in | std::ios::out | std::ios::binary);
    } else {
      fs.open(*resolved, std::ios::in | std::ios::binary);
    }
  }
  if (!fs.is_open() && !create_mode && want_write && !is_save_file) {
    if (auto ro = vfs_.resolve_for_read(path)) {
      fs.open(*ro, std::ios::in | std::ios::binary);
      if (fs.is_open()) {
        resolved = ro;
        opened_writable = false;
      }
    }
  }

  if (!fs.is_open() || !resolved) {
    if (is_save_file && want_write &&
        (create_mode || resolved.has_value() ||
         vfs_.resolve_for_read(path).has_value())) {
      emu->x86.R_AX = 5u;  // Access denied
    } else {
      emu->x86.R_AX = 2u;  // File not found
    }
    set_cf(emu, true);
    return;
  }

  files_[slot].kind = HandleKind::DiskFile;
  files_[slot].last_io = FileSlot::LastIo::None;
  files_[slot].owner_psp = current_psp_;
  files_[slot].writable = opened_writable;
  files_[slot].host_path = *resolved;
  files_[slot].stream = std::move(fs);
  ++file_activity_;
  emu->x86.R_AX = static_cast<std::uint16_t>(slot);
  set_cf(emu, false);
}

void DosKernel::int21_read_file(x86emu_t* emu) {
  const std::uint16_t h = emu->x86.R_BX;
  const std::uint16_t count = emu->x86.R_CX;
  if (h >= kMaxFiles || files_[h].kind == HandleKind::Unused) {
    emu->x86.R_AX = 6u;
    set_cf(emu, true);
    return;
  }
  if (files_[h].kind != HandleKind::DiskFile || count == 0u) {
    emu->x86.R_AX = 0u;
    set_cf(emu, false);
    return;
  }

  const std::uint32_t dst =
      (static_cast<std::uint32_t>(emu->x86.R_DS) << 4) + emu->x86.R_DX;
  if (dst >= ram_size_) {
    emu->x86.R_AX = 0u;
    set_cf(emu, false);
    return;
  }
  const auto max_bytes = static_cast<std::uint16_t>(
      std::min<std::size_t>(count, ram_size_ - static_cast<std::size_t>(dst)));

  auto& f = files_[h];
  f.stream.clear();
  if (f.last_io == FileSlot::LastIo::Write) {
    const auto pos = f.stream.tellp();
    if (pos != std::streampos(-1)) {
      f.stream.seekg(pos);
    } else {
      f.stream.seekg(0, std::ios::cur);
    }
    f.stream.clear();
  }
  f.stream.read(io_scratch_.data(), max_bytes);
  const auto got = static_cast<std::uint16_t>(f.stream.gcount());
  f.last_io = FileSlot::LastIo::Read;
  if (got > 0u) {
    guest_write_block(dst, io_scratch_.data(), got);
  }
  f.stream.clear();
  ++file_activity_;
  emu->x86.R_AX = got;
  set_cf(emu, false);
}

void DosKernel::int21_write_file(x86emu_t* emu) {
  const std::uint16_t h = emu->x86.R_BX;
  const std::uint16_t count = emu->x86.R_CX;
  if (h >= kMaxFiles || files_[h].kind == HandleKind::Unused) {
    emu->x86.R_AX = 6u;
    set_cf(emu, true);
    return;
  }
  if (files_[h].kind != HandleKind::DiskFile) {
    emu->x86.R_AX = count;
    set_cf(emu, false);
    return;
  }
  if (!files_[h].writable) {
    emu->x86.R_AX = 5u;
    set_cf(emu, true);
    return;
  }

  auto& f = files_[h];
  f.stream.clear();
  if (f.last_io == FileSlot::LastIo::Read) {
    const auto pos = f.stream.tellg();
    if (pos != std::streampos(-1)) {
      f.stream.seekp(pos);
    } else {
      f.stream.seekp(0, std::ios::cur);
    }
    f.stream.clear();
  }
  if (count == 0u) {
    const auto pos = f.stream.tellp();
    f.stream.flush();
    if (pos >= 0) {
      std::error_code ec;
      std::filesystem::resize_file(f.host_path,
                                   static_cast<std::uintmax_t>(pos), ec);
      f.stream.clear();
      f.stream.seekg(pos);
      f.stream.seekp(pos);
    }
    f.last_io = FileSlot::LastIo::None;
    emu->x86.R_AX = 0u;
    set_cf(emu, false);
    return;
  }

  const std::uint32_t src =
      (static_cast<std::uint32_t>(emu->x86.R_DS) << 4) + emu->x86.R_DX;
  if (src >= ram_size_) {
    emu->x86.R_AX = 0u;
    set_cf(emu, true);
    return;
  }
  const auto to_write = static_cast<std::uint16_t>(
      std::min<std::size_t>(count, ram_size_ - static_cast<std::size_t>(src)));
  guest_read_block(src, io_scratch_.data(), to_write);
  f.stream.write(io_scratch_.data(), to_write);
  f.stream.flush();
  f.last_io = FileSlot::LastIo::Write;
  ++file_activity_;
  emu->x86.R_AX = f.stream.good() ? to_write : 0u;
  set_cf(emu, !f.stream.good());
}

void DosKernel::int21_seek_file(x86emu_t* emu, std::uint8_t al) {
  const std::uint16_t h = emu->x86.R_BX;
  if (h >= kMaxFiles || files_[h].kind == HandleKind::Unused) {
    emu->x86.R_AX = 6u;
    set_cf(emu, true);
    return;
  }
  if (files_[h].kind != HandleKind::DiskFile) {
    emu->x86.R_DX = 0u;
    emu->x86.R_AX = 0u;
    set_cf(emu, false);
    return;
  }
  const auto offset = static_cast<std::int32_t>(
      (static_cast<std::uint32_t>(emu->x86.R_CX) << 16) | emu->x86.R_DX);
  std::ios_base::seekdir dir = std::ios::beg;
  if (al == 1u) {
    dir = std::ios::cur;
  } else if (al == 2u) {
    dir = std::ios::end;
  }
  auto& f = files_[h];
  f.stream.clear();
  f.stream.seekg(offset, dir);
  const auto pos = f.stream.tellg();
  if (pos != std::streampos(-1)) {
    f.stream.seekg(pos);
    if (f.writable) {
      f.stream.seekp(pos);
    }
  } else if (f.writable) {
    f.stream.clear();
    f.stream.seekp(offset, dir);
  }
  f.last_io = FileSlot::LastIo::None;
  const std::uint32_t new_pos =
      (pos >= 0) ? static_cast<std::uint32_t>(pos) : 0u;
  f.stream.clear();
  emu->x86.R_DX = static_cast<std::uint16_t>((new_pos >> 16) & 0xFFFFu);
  emu->x86.R_AX = static_cast<std::uint16_t>(new_pos & 0xFFFFu);
  set_cf(emu, false);
}

void DosKernel::int21_ioctl(x86emu_t* emu, std::uint8_t al) {
  const std::uint16_t h = emu->x86.R_BX;
  if (al == 0x00u) {
    if (h >= kMaxFiles || files_[h].kind == HandleKind::Unused) {
      emu->x86.R_AX = 6u;
      set_cf(emu, true);
      return;
    }
    const std::uint16_t info =
        (files_[h].kind == HandleKind::DiskFile) ? 0x0002u : 0x0080u;
    emu->x86.R_DX = info;
    emu->x86.R_AX = info;
    set_cf(emu, false);
  } else if (al == 0x07u) {
    emu->x86.R_AL = 0xFFu;
    set_cf(emu, false);
  } else if (al == 0x08u) {
    emu->x86.R_AX = 0x0001u;
    set_cf(emu, false);
  } else {
    set_cf(emu, false);
  }
}

void DosKernel::int21_exec(x86emu_t* emu, std::uint8_t al) {
  if (al != 0x00u) {
    emu->x86.R_AX = 1u;
    set_cf(emu, true);
    return;
  }
  const std::string prog = read_asciiz(emu->x86.R_DS, emu->x86.R_DX);
  const std::uint32_t pb =
      (static_cast<std::uint32_t>(emu->x86.R_ES) << 4) + emu->x86.R_BX;
  const std::uint16_t tail_off = guest_read16(pb + 2u);
  const std::uint16_t tail_seg = guest_read16(pb + 4u);
  const std::uint32_t tail_lin =
      (static_cast<std::uint32_t>(tail_seg) << 4) + tail_off;

  std::string raw_tail;
  const std::uint8_t tlen =
      std::min<std::uint8_t>(126u, guest_read8(tail_lin));
  raw_tail.reserve(static_cast<std::size_t>(tlen) + 2u);
  for (std::size_t i = 0; i < static_cast<std::size_t>(tlen) + 2u; ++i) {
    raw_tail.push_back(static_cast<char>(
        guest_read8(tail_lin + static_cast<std::uint32_t>(i))));
  }

  if (!launch_executable(emu, prog, raw_tail, false)) {
    emu->x86.R_AX = 2u;
    set_cf(emu, true);
  } else {
    set_cf(emu, false);
  }
}

void DosKernel::int21_find_first_next(x86emu_t* emu, std::uint8_t ah) {
  const std::uint32_t dta =
      (static_cast<std::uint32_t>(dta_seg_) << 4) + dta_off_;
  if (ah == 0x4Eu) {
    const std::string pat = read_asciiz(emu->x86.R_DS, emu->x86.R_DX);
    auto matches = vfs_.find_matching(pat);
    if (matches.empty()) {
      active_find_matches_.erase(dta);
      emu->x86.R_AX = 18u;  // No more files
      set_cf(emu, true);
    } else {
      if (!active_find_matches_.contains(dta) &&
          active_find_matches_.size() >= 64u) {
        active_find_matches_.erase(active_find_matches_.begin());
      }
      write_dta_entry(matches[0], 1u);
      active_find_matches_[dta] = std::move(matches);
      emu->x86.R_AX = 0u;
      set_cf(emu, false);
    }
    return;
  }

  const std::uint16_t idx = guest_read16(dta + 0x13u);
  auto it = active_find_matches_.find(dta);
  if (it == active_find_matches_.end() || idx >= it->second.size()) {
    emu->x86.R_AX = 18u;
    set_cf(emu, true);
  } else {
    write_dta_entry(it->second[idx], static_cast<std::uint16_t>(idx + 1u));
    emu->x86.R_AX = 0u;
    set_cf(emu, false);
  }
}

void DosKernel::int21_memory_ops(x86emu_t* emu, std::uint8_t ah,
                                 std::uint8_t al) {
  if (ah == 0x48u) {  // Allocate Memory (BX = paragraphs)
    std::uint16_t max_free = 0;
    if (auto seg = mcb_alloc(emu->x86.R_BX, current_psp_, &max_free)) {
      emu->x86.R_AX = *seg;
      set_cf(emu, false);
    } else {
      emu->x86.R_AX = 8u;  // Insufficient memory
      emu->x86.R_BX = max_free;
      set_cf(emu, true);
    }
  } else if (ah == 0x49u) {  // Free Memory (ES = segment)
    if (mcb_free(emu->x86.R_ES)) {
      set_cf(emu, false);
    } else {
      emu->x86.R_AX = 9u;
      set_cf(emu, true);
    }
  } else if (ah == 0x4Au) {  // Resize Memory Block (ES = seg, BX = new paras)
    std::uint16_t max_poss = 0;
    if (mcb_resize(emu->x86.R_ES, emu->x86.R_BX, &max_poss)) {
      set_cf(emu, false);
    } else {
      emu->x86.R_AX = 8u;
      emu->x86.R_BX = max_poss;
      set_cf(emu, true);
    }
  } else if (ah == 0x58u) {  // Get/Set Strategy & UMB Link State
    if (al == 0x00u) {
      emu->x86.R_AX = mcb_alloc_strategy_;
      set_cf(emu, false);
    } else if (al == 0x01u) {
      mcb_alloc_strategy_ = static_cast<std::uint8_t>(emu->x86.R_BX & 0xFFu);
      set_cf(emu, false);
    } else if (al == 0x02u) {
      emu->x86.R_AL = umb_link_state_;
      set_cf(emu, false);
    } else if (al == 0x03u) {
      umb_link_state_ = static_cast<std::uint8_t>(emu->x86.R_BX & 0x01u);
      set_cf(emu, false);
    } else {
      emu->x86.R_AX = 1u;
      set_cf(emu, true);
    }
  }
}

void DosKernel::handle_int21(x86emu_t* emu) {
  const std::uint8_t ah = emu->x86.R_AH;
  const std::uint8_t al = emu->x86.R_AL;
  if (trace_dos_enabled() && ah != 0x2Cu && ah != 0x06u) {
    std::cerr << "[dos-trace] INT21 AX=0x" << std::hex << emu->x86.R_AX
              << " BX=0x" << emu->x86.R_BX << " CX=0x" << emu->x86.R_CX
              << " DX=0x" << emu->x86.R_DX << " DS=0x" << emu->x86.R_DS
              << " ES=0x" << emu->x86.R_ES << " CS:IP=" << emu->x86.R_CS << ":"
              << emu->x86.R_IP << std::dec << "\n";
  }

  switch (ah) {
    case 0x00u:  // Legacy Terminate
      terminate_current_process(emu, 0x0000u, false, 0u);
      break;

    case 0x01u:
    case 0x02u:
    case 0x06u:
    case 0x07u:
    case 0x08u:
    case 0x09u:
    case 0x0Au:
    case 0x0Bu:
    case 0x0Cu:
      int21_console_io(emu, ah, al);
      break;

    case 0x0Du:  // Disk Reset
      set_cf(emu, false);
      break;

    case 0x0Eu:  // Select Default Drive
      current_drive_ = emu->x86.R_DL;
      emu->x86.R_AL = 26u;
      break;

    case 0x19u:  // Get Current Default Drive
      emu->x86.R_AL = current_drive_;
      break;

    case 0x1Au:  // Set DTA
      dta_seg_ = emu->x86.R_DS;
      dta_off_ = emu->x86.R_DX;
      break;

    case 0x25u: {  // Set Interrupt Vector AL = DS:DX
      const std::uint32_t vec = static_cast<std::uint32_t>(al) * 4u;
      ram_write16(vec + 0u, emu->x86.R_DX);
      ram_write16(vec + 2u, emu->x86.R_DS);
      break;
    }

    case 0x2Au:  // Get System Date
      emu->x86.R_CX = 1995u;
      emu->x86.R_DH = 12u;
      emu->x86.R_DL = 15u;
      emu->x86.R_AL = 5u;
      break;

    case 0x2Cu: {  // Get System Time (live clock so RNG seeds vary naturally)
      std::int64_t cs = 0;
      if (deterministic_clock_) {
        // One BIOS tick = 65536 / 1193182 s.
        cs = static_cast<std::int64_t>(ram_read32(0x046Cu)) * 6553600 / 1193182;
      } else {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        cs = std::chrono::duration_cast<std::chrono::milliseconds>(now).count() /
             10;
      }
      emu->x86.R_DL = static_cast<std::uint8_t>(cs % 100);
      emu->x86.R_DH = static_cast<std::uint8_t>((cs / 100) % 60);
      emu->x86.R_CL = static_cast<std::uint8_t>((cs / 6000) % 60);
      emu->x86.R_CH = static_cast<std::uint8_t>((cs / 360000) % 24);
      break;
    }

    case 0x2Fu:  // Get DTA -> ES:BX
      x86emu_set_seg_register(emu, emu->x86.R_ES_SEL, dta_seg_);
      emu->x86.R_BX = dta_off_;
      break;

    case 0x30u:  // Get DOS Version -> DOS 5.0 (required by MSC 7.0 MOVE)
      emu->x86.R_AL = 5u;
      emu->x86.R_AH = 0u;
      emu->x86.R_BX = 0u;
      emu->x86.R_CX = 0u;
      break;

    case 0x31u:  // Terminate and Stay Resident
      terminate_current_process(
          emu, static_cast<std::uint16_t>(0x0300u | al), true, emu->x86.R_DX);
      break;

    case 0x33u:  // Ctrl-Break Check
      if (al == 0x00u) {
        emu->x86.R_DL = 0u;
      }
      set_cf(emu, false);
      break;

    case 0x35u: {  // Get Interrupt Vector AL -> ES:BX
      const std::uint32_t vec = static_cast<std::uint32_t>(al) * 4u;
      emu->x86.R_BX = ram_read16(vec + 0u);
      x86emu_set_seg_register(emu, emu->x86.R_ES_SEL, ram_read16(vec + 2u));
      break;
    }

    case 0x36u:  // Get Disk Free Space (~512 MB free)
      emu->x86.R_AX = 64u;
      emu->x86.R_BX = 16384u;
      emu->x86.R_CX = 512u;
      emu->x86.R_DX = 32768u;
      break;

    case 0x38u: {  // Get Country-Dependent Info (CRITICAL: BX=1 for .16P fonts)
      if (al != 0xFFu) {
        const std::uint32_t buf =
            (static_cast<std::uint32_t>(emu->x86.R_DS) << 4) + emu->x86.R_DX;
        std::array<std::uint8_t, 34> country{};
        country[0] = 1u;
        country[1] = 0u;
        guest_write_block(buf, country.data(), country.size());
      }
      emu->x86.R_BX = 1u;  // Country code 1 (non-81 -> Traditional Chinese .16P)
      emu->x86.R_AX = 1u;
      set_cf(emu, false);
      break;
    }

    case 0x3Cu:
    case 0x3Du:
      int21_open_or_create(emu, ah == 0x3Cu, al);
      break;

    case 0x3Eu: {  // Close File
      const std::uint16_t h = emu->x86.R_BX;
      if (h >= kMaxFiles || files_[h].kind == HandleKind::Unused) {
        emu->x86.R_AX = 6u;
        set_cf(emu, true);
        break;
      }
      if (h >= 5u) {
        if (files_[h].stream.is_open()) {
          files_[h].stream.flush();
          files_[h].stream.close();
        }
        files_[h] = FileSlot{};
      }
      set_cf(emu, false);
      break;
    }

    case 0x3Fu:
      int21_read_file(emu);
      break;

    case 0x40u:
      int21_write_file(emu);
      break;

    case 0x41u: {  // Delete File
      const std::string path = read_asciiz(emu->x86.R_DS, emu->x86.R_DX);
      if (auto wr = vfs_.resolve_existing_in_write_dir(path)) {
        close_handles_for_host_path(*wr);
        std::error_code perm_ec;
        if (vfs_.save_dir() != vfs_.root_dir()) {
          std::filesystem::permissions(
              *wr,
              std::filesystem::perms::owner_read |
                  std::filesystem::perms::owner_write,
              std::filesystem::perm_options::add, perm_ec);
        }
        std::error_code ec;
        if (std::filesystem::remove(*wr, ec) && !ec) {
          ++file_activity_;
          set_cf(emu, false);
          break;
        }
      }
      emu->x86.R_AX = 2u;
      set_cf(emu, true);
      break;
    }

    case 0x42u:
      int21_seek_file(emu, al);
      break;

    case 0x43u: {  // Get/Set File Attributes
      const std::string path = read_asciiz(emu->x86.R_DS, emu->x86.R_DX);
      if (vfs_.resolve_for_read(path)) {
        emu->x86.R_CX = 0x0020u;
        set_cf(emu, false);
      } else {
        emu->x86.R_AX = 2u;
        set_cf(emu, true);
      }
      break;
    }

    case 0x44u:
      int21_ioctl(emu, al);
      break;

    case 0x47u: {  // Get Current Directory -> DS:SI
      const std::uint32_t dst =
          (static_cast<std::uint32_t>(emu->x86.R_DS) << 4) + emu->x86.R_SI;
      guest_write8(dst, 0u);
      set_cf(emu, false);
      break;
    }

    case 0x48u:
    case 0x49u:
    case 0x4Au:
    case 0x58u:
      int21_memory_ops(emu, ah, al);
      break;

    case 0x4Bu:
      int21_exec(emu, al);
      break;

    case 0x4Cu:  // Terminate Process with Return Code AL
      terminate_current_process(emu, al, false, 0u);
      break;

    case 0x4Du:  // Get Child Return Code
      emu->x86.R_AX = last_child_return_code_;
      last_child_return_code_ = 0u;
      set_cf(emu, false);
      break;

    case 0x4Eu:
    case 0x4Fu:
      int21_find_first_next(emu, ah);
      break;

    case 0x50u:  // Set Current PSP
      current_psp_ = emu->x86.R_BX;
      break;

    case 0x51u:
    case 0x62u:  // Get Current PSP
      emu->x86.R_BX = current_psp_;
      break;

    case 0x56u: {  // Rename File (DS:DX -> ES:DI)
      const std::string old_p = read_asciiz(emu->x86.R_DS, emu->x86.R_DX);
      const std::string new_p = read_asciiz(emu->x86.R_ES, emu->x86.R_DI);
      auto src = vfs_.resolve_for_write(old_p, false);
      auto dst = vfs_.resolve_for_write(new_p, true);
      std::error_code ec;
      if (src && dst) {
        close_handles_for_host_path(*src);
        close_handles_for_host_path(*dst);
        std::filesystem::rename(*src, *dst, ec);
        if (!ec) {
          ++file_activity_;
          set_cf(emu, false);
          break;
        }
      }
      emu->x86.R_AX = 2u;
      set_cf(emu, true);
      break;
    }

    case 0x57u:  // Get/Set File Date & Time
      if (al == 0x00u) {
        emu->x86.R_CX = 0x0000u;
        emu->x86.R_DX = 0x2121u;
      }
      set_cf(emu, false);
      break;

    case 0x67u:  // Set Handle Count (BX <= 128)
      set_cf(emu, false);
      break;

    case 0x68u:
    case 0x6Au: {  // Commit / Flush File
      const std::uint16_t h = emu->x86.R_BX;
      if (h >= kMaxFiles || files_[h].kind == HandleKind::Unused) {
        emu->x86.R_AX = 6u;
        set_cf(emu, true);
        break;
      }
      if (files_[h].kind == HandleKind::DiskFile && files_[h].stream.is_open()) {
        files_[h].stream.clear();
        files_[h].stream.flush();
      }
      set_cf(emu, false);
      break;
    }

    default:
      set_cf(emu, false);
      break;
  }
}

}  // namespace san4
