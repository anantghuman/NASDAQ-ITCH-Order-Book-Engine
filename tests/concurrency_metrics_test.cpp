#include "lob/latency.hpp"
#include "lob/spsc_ring.hpp"

#include <atomic>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

void require(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void test_ring_basic() {
  bool rejected_bad_capacity = false;
  try {
    [[maybe_unused]] lob::SpscRing<std::uint64_t> invalid(3);
  } catch (const std::invalid_argument&) {
    rejected_bad_capacity = true;
  }
  require(rejected_bad_capacity, "ring must reject non-power-of-two capacity");

  lob::SpscRing<std::uint64_t> ring(4);
  require(ring.empty(), "new ring must be empty");
  require(ring.size() == 0, "new ring must have size zero");
  for (std::uint64_t value = 1; value <= 4; ++value) {
    require(ring.try_push(value), "ring unexpectedly rejected a value");
  }
  require(ring.full(), "ring must report full at capacity");
  require(!ring.try_push(5), "full ring accepted a value");

  std::uint64_t value = 0;
  for (std::uint64_t expected = 1; expected <= 4; ++expected) {
    require(ring.try_pop(value), "ring unexpectedly reported empty");
    require(value == expected, "ring did not preserve FIFO order");
  }
  require(!ring.try_pop(value), "empty ring produced a value");
  require(ring.empty(), "ring must be empty after draining");

  // Force multiple index wraps through the same four slots.
  for (std::uint64_t expected = 0; expected < 1000; ++expected) {
    require(ring.try_push(expected), "ring rejected a wraparound value");
    require(ring.try_pop(value), "ring lost a wraparound value");
    require(value == expected, "ring changed wraparound order");
  }
}

void test_ring_threaded_order_preservation() {
  constexpr std::uint64_t kMessageCount = 1'000'000;
  lob::SpscRing<std::uint64_t> ring(1024);
  std::atomic<bool> start{false};
  std::atomic<bool> order_preserved{true};

  std::thread producer([&] {
    while (!start.load(std::memory_order_acquire)) {
    }
    for (std::uint64_t value = 0; value < kMessageCount; ++value) {
      while (!ring.try_push(value)) {
      }
    }
  });

  std::thread consumer([&] {
    while (!start.load(std::memory_order_acquire)) {
    }
    std::uint64_t received = 0;
    for (std::uint64_t expected = 0; expected < kMessageCount; ++expected) {
      while (!ring.try_pop(received)) {
      }
      if (received != expected) {
        order_preserved.store(false, std::memory_order_relaxed);
      }
    }
  });

  start.store(true, std::memory_order_release);
  producer.join();
  consumer.join();

  require(order_preserved.load(std::memory_order_relaxed),
          "threaded ring stress test did not preserve order");
  require(ring.empty(), "threaded ring must drain completely");
}

void test_histogram() {
  lob::metrics::HdrHistogram histogram;
  require(histogram.count() == 0, "empty histogram count must be zero");
  require(histogram.value_at_percentile(99.0) == 0,
          "empty histogram percentile must be zero");

  for (std::uint64_t value = 1; value <= 1000; ++value) {
    histogram.record(value);
  }
  const auto summary = histogram.summary();
  require(summary.count == 1000, "histogram count mismatch");
  require(summary.min_ns == 1 && summary.max_ns == 1000, "histogram extrema mismatch");
  require(summary.p50_ns == 500, "histogram p50 must use nearest rank");
  require(summary.p99_ns == 990, "histogram p99 must use nearest rank");
  require(summary.p999_ns == 999, "histogram p99.9 must use nearest rank");

  histogram.reset();
  constexpr std::uint64_t kLargeLatency = 1'000'000'007ULL;
  histogram.record(kLargeLatency);
  const std::uint64_t sampled = histogram.value_at_percentile(50.0);
  require(sampled <= kLargeLatency, "histogram must return an equivalent lower bound");
  require(kLargeLatency - sampled <= (kLargeLatency >> 10U),
          "histogram exceeded its documented precision bound");
  require(histogram.min() == kLargeLatency && histogram.max() == kLargeLatency,
          "histogram must retain exact extrema");
}

}  // namespace

int main() {
  try {
    test_ring_basic();
    test_ring_threaded_order_preservation();
    test_histogram();
  } catch (const std::exception& error) {
    std::cerr << "concurrency_metrics_test failed: " << error.what() << '\n';
    return 1;
  }

  std::cout << "concurrency_metrics_test passed\n";
  return 0;
}
