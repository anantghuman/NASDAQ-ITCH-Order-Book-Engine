#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace lob::metrics {

struct LatencySummary {
  std::uint64_t count{0};
  std::uint64_t min_ns{0};
  std::uint64_t max_ns{0};
  std::uint64_t p50_ns{0};
  std::uint64_t p99_ns{0};
  std::uint64_t p999_ns{0};
};

// A fixed-range, HDR-style histogram for non-negative nanosecond durations.
//
// The histogram records the exponent of each value and ten significant binary
// digits inside that exponent. Values below 2^11 ns are represented exactly;
// above that, the reported percentile is the lower bound of an equivalent
// range. The range width is at most value / 1024 (roughly 0.1%), matching the
// useful precision of a three-significant-digit HDR histogram while covering
// the entire uint64_t nanosecond range. It uses the compact HDR convention of
// retaining only the upper half of each larger exponent bucket. Its counter
// array is allocated inline, has a fixed ~440 KiB footprint, and record()
// performs no allocation or locking.
//
// It is intentionally single-writer. Use one instance per matching thread and
// merge summaries externally if multiple writers are required.
class HdrHistogram {
 public:
  static constexpr unsigned kSignificantBits = 10;
  static constexpr std::size_t kSubBucketCount = 1U << (kSignificantBits + 1U);
  static constexpr std::size_t kSubBucketHalfCount = kSubBucketCount / 2U;
  static constexpr std::size_t kBucketCount = 64;
  static constexpr std::size_t kBinCount =
      kSubBucketCount +
      ((kBucketCount - (kSignificantBits + 1U)) * kSubBucketHalfCount);

  HdrHistogram() noexcept;

  void record(std::uint64_t value_ns) noexcept;
  void reset() noexcept;

  [[nodiscard]] std::uint64_t count() const noexcept;
  [[nodiscard]] std::uint64_t min() const noexcept;
  [[nodiscard]] std::uint64_t max() const noexcept;

  // percentile must be in [0, 100]. Values outside this range are clamped.
  // A non-empty histogram uses nearest-rank semantics: ceil(p * count / 100).
  [[nodiscard]] std::uint64_t value_at_percentile(double percentile) const noexcept;
  [[nodiscard]] LatencySummary summary() const noexcept;

 private:
  [[nodiscard]] static std::size_t index_for(std::uint64_t value_ns) noexcept;
  [[nodiscard]] static std::uint64_t value_for_index(std::size_t index) noexcept;

  std::array<std::uint64_t, kBinCount> counts_{};
  std::uint64_t count_{0};
  std::uint64_t min_{0};
  std::uint64_t max_{0};
};

using LatencyHistogram = HdrHistogram;

}  // namespace lob::metrics
