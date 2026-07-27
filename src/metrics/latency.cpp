#include "lob/latency.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace lob::metrics {

HdrHistogram::HdrHistogram() noexcept { reset(); }

void HdrHistogram::record(const std::uint64_t value_ns) noexcept {
  ++counts_[index_for(value_ns)];
  ++count_;
  if (count_ == 1 || value_ns < min_) {
    min_ = value_ns;
  }
  if (value_ns > max_) {
    max_ = value_ns;
  }
}

void HdrHistogram::reset() noexcept {
  counts_.fill(0);
  count_ = 0;
  min_ = 0;
  max_ = 0;
}

std::uint64_t HdrHistogram::count() const noexcept { return count_; }

std::uint64_t HdrHistogram::min() const noexcept { return count_ == 0 ? 0 : min_; }

std::uint64_t HdrHistogram::max() const noexcept { return count_ == 0 ? 0 : max_; }

std::uint64_t HdrHistogram::value_at_percentile(double percentile) const noexcept {
  if (count_ == 0) {
    return 0;
  }

  percentile = std::clamp(percentile, 0.0, 100.0);
  const long double scaled =
      (static_cast<long double>(percentile) * static_cast<long double>(count_)) / 100.0L;
  std::uint64_t rank = static_cast<std::uint64_t>(scaled);
  if (static_cast<long double>(rank) < scaled) {
    ++rank;
  }
  rank = std::max<std::uint64_t>(rank, 1);

  std::uint64_t observed = 0;
  for (std::size_t index = 0; index < kBinCount; ++index) {
    observed += counts_[index];
    if (observed >= rank) {
      return value_for_index(index);
    }
  }

  // Counter overflow is outside the operational envelope, but returning the
  // observed maximum is safer than returning an arbitrary value if it occurs.
  return max_;
}

LatencySummary HdrHistogram::summary() const noexcept {
  return {
      .count = count(),
      .min_ns = min(),
      .max_ns = max(),
      .p50_ns = value_at_percentile(50.0),
      .p99_ns = value_at_percentile(99.0),
      .p999_ns = value_at_percentile(99.9),
  };
}

std::size_t HdrHistogram::index_for(const std::uint64_t value_ns) noexcept {
  const unsigned exponent =
      value_ns == 0 ? 0U : static_cast<unsigned>(std::bit_width(value_ns)) - 1U;
  const unsigned shift = exponent > kSignificantBits ? exponent - kSignificantBits : 0U;
  const std::size_t sub_bucket = static_cast<std::size_t>(value_ns >> shift);
  if (exponent <= kSignificantBits) {
    return sub_bucket;
  }

  const std::size_t compact_bucket = exponent - (kSignificantBits + 1U);
  return kSubBucketCount + (compact_bucket * kSubBucketHalfCount) +
         (sub_bucket - kSubBucketHalfCount);
}

std::uint64_t HdrHistogram::value_for_index(const std::size_t index) noexcept {
  if (index < kSubBucketCount) {
    return index;
  }

  const std::size_t compact_index = index - kSubBucketCount;
  const unsigned exponent = static_cast<unsigned>(kSignificantBits + 1U +
                                                  (compact_index / kSubBucketHalfCount));
  const std::uint64_t sub_bucket =
      kSubBucketHalfCount + (compact_index % kSubBucketHalfCount);
  const unsigned shift = exponent - kSignificantBits;
  return sub_bucket << shift;
}

}  // namespace lob::metrics
