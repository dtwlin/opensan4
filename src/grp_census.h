#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <ostream>
#include <vector>

namespace san4 {

// Statistical census of GRP600C driver calls (`INT F1h`): call counts per
// (AH, AL & 3, ROP) key, plus guest instructions attributed to the most
// recently called key whenever an emulation slice ends inside the GRP600C
// code segment. Only allocated when profiling is enabled.
class GrpCensus {
 public:
  // ROP values above this share one "kRopOther" bucket.
  static constexpr std::uint16_t kMaxTrackedRop = 15u;
  static constexpr std::uint16_t kRopOther = kMaxTrackedRop + 1u;

  struct Row {
    std::uint8_t ah = 0;
    std::uint8_t al_mode = 0;  // AL & 3
    std::uint16_t rop = 0;     // kRopOther for any value > kMaxTrackedRop
    std::uint64_t calls = 0;
    std::uint64_t instrs = 0;
  };

  void record_call(std::uint8_t ah, std::uint8_t al, std::uint16_t rop) {
    const std::uint16_t bucket = std::min(rop, kRopOther);
    last_index_ = index_of(ah, static_cast<std::uint8_t>(al & 0x03u), bucket);
    ++entries_[last_index_].calls;
    ++total_calls_;
  }

  // Credits a whole emulation slice that ended inside GRP600C to the most
  // recent call key. This samples the code segment at slice boundaries: the
  // share of slices ending in GRP600C estimates its share of guest time.
  void credit_grp_slice(std::uint64_t slice_begin_tsc,
                        std::uint64_t slice_end_tsc) {
    if (total_calls_ == 0u || slice_end_tsc <= slice_begin_tsc) {
      return;
    }
    entries_[last_index_].instrs += slice_end_tsc - slice_begin_tsc;
  }

  void add_guest_instrs(std::uint64_t n) { total_guest_instrs_ += n; }

  std::uint64_t total_guest_instrs() const { return total_guest_instrs_; }
  std::uint64_t total_calls() const { return total_calls_; }

  // Non-empty rows, most attributed instructions first (ties: most calls).
  std::vector<Row> sorted_rows() const {
    std::vector<Row> rows;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
      if (entries_[i].calls == 0u && entries_[i].instrs == 0u) {
        continue;
      }
      rows.push_back(Row{
          .ah = static_cast<std::uint8_t>(i / (4u * kRopBuckets)),
          .al_mode = static_cast<std::uint8_t>((i / kRopBuckets) % 4u),
          .rop = static_cast<std::uint16_t>(i % kRopBuckets),
          .calls = entries_[i].calls,
          .instrs = entries_[i].instrs,
      });
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
      return (a.instrs != b.instrs) ? (a.instrs > b.instrs)
                                    : (a.calls > b.calls);
    });
    return rows;
  }

  void print(std::ostream& os) const {
    const auto rows = sorted_rows();
    std::uint64_t attributed = 0;
    for (const Row& r : rows) {
      attributed += r.instrs;
    }
    char line[160];
    std::snprintf(line, sizeof(line),
                  "[san4-census] GRP600C INT F1h: calls=%llu guest_instrs=%llu "
                  "attributed=%llu (%.2f%%)\n",
                  static_cast<unsigned long long>(total_calls_),
                  static_cast<unsigned long long>(total_guest_instrs_),
                  static_cast<unsigned long long>(attributed),
                  percent(attributed));
    os << line;
    os << "[san4-census]   AH AL&3  ROP        calls        instrs   %total\n";
    for (const Row& r : rows) {
      char rop[8];
      if (r.rop == kRopOther) {
        std::snprintf(rop, sizeof(rop), ">%u", unsigned{kMaxTrackedRop});
      } else {
        std::snprintf(rop, sizeof(rop), "%u", unsigned{r.rop});
      }
      std::snprintf(line, sizeof(line),
                    "[san4-census] 0x%02X %4u %4s %12llu %13llu %7.2f%%\n",
                    unsigned{r.ah}, unsigned{r.al_mode}, rop,
                    static_cast<unsigned long long>(r.calls),
                    static_cast<unsigned long long>(r.instrs),
                    percent(r.instrs));
      os << line;
    }
  }

 private:
  static constexpr std::size_t kRopBuckets = kRopOther + 1u;

  struct Entry {
    std::uint64_t calls = 0;
    std::uint64_t instrs = 0;
  };

  static std::size_t index_of(std::uint8_t ah, std::uint8_t al_mode,
                              std::uint16_t rop_bucket) {
    return (static_cast<std::size_t>(ah) * 4u + al_mode) * kRopBuckets +
           rop_bucket;
  }

  double percent(std::uint64_t n) const {
    return (total_guest_instrs_ == 0u)
               ? 0.0
               : 100.0 * static_cast<double>(n) /
                     static_cast<double>(total_guest_instrs_);
  }

  std::array<Entry, 256u * 4u * kRopBuckets> entries_{};
  std::size_t last_index_ = 0;
  std::uint64_t total_calls_ = 0;
  std::uint64_t total_guest_instrs_ = 0;
};

}  // namespace san4
