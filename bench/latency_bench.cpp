#include "lob/latency.hpp"
#include "lob/matching_engine.hpp"
#include "lob/order_book.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] std::size_t parse_order_count(const char* input) {
  try {
    const auto value = std::stoull(input);
    if (value == 0 || value > std::numeric_limits<std::uint32_t>::max()) {
      throw std::out_of_range("order count outside supported range");
    }
    return static_cast<std::size_t>(value);
  } catch (const std::exception&) {
    return 0;
  }
}

void print(const char* name, const lob::metrics::LatencyHistogram& histogram) {
  const auto summary = histogram.summary();
  std::cout << name << ": count=" << summary.count << " p50=" << summary.p50_ns
            << "ns p99=" << summary.p99_ns << "ns p99.9=" << summary.p999_ns
            << "ns max=" << summary.max_ns << "ns\n";
}

[[nodiscard]] std::uint64_t elapsed_ns(const Clock::time_point begin,
                                       const Clock::time_point end) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
}

}  // namespace

int main(const int argc, char** argv) {
  if (argc > 2) {
    std::cerr << "usage: lob_latency_bench [orders]\n";
    return EXIT_FAILURE;
  }
  const std::size_t order_count = argc == 2 ? parse_order_count(argv[1]) : 200'000U;
  if (order_count == 0) {
    std::cerr << "orders must be an integer in [1, 4294967295]\n";
    return EXIT_FAILURE;
  }

  constexpr lob::Price kBasePrice = 1'000'000U;
  constexpr lob::StockLocate kLocate = 1;
  lob::OrderBook book;
  lob::metrics::LatencyHistogram add_latency;
  lob::metrics::LatencyHistogram cancel_latency;

  for (std::size_t index = 0; index < order_count; ++index) {
    const auto begin = Clock::now();
    const bool added = book.add(static_cast<lob::OrderId>(index + 1U), lob::Side::Buy,
                                kBasePrice + static_cast<lob::Price>(index % 256U), 100U);
    const auto end = Clock::now();
    if (!added) {
      std::cerr << "benchmark setup failed while adding order\n";
      return EXIT_FAILURE;
    }
    add_latency.record(elapsed_ns(begin, end));
  }

  for (std::size_t index = 0; index < order_count; ++index) {
    const auto begin = Clock::now();
    const bool cancelled = book.cancel(static_cast<lob::OrderId>(index + 1U), 100U);
    const auto end = Clock::now();
    if (!cancelled) {
      std::cerr << "benchmark setup failed while cancelling order\n";
      return EXIT_FAILURE;
    }
    cancel_latency.record(elapsed_ns(begin, end));
  }

  lob::MatchingEngine engine;
  for (std::size_t index = 0; index < order_count; ++index) {
    const lob::itch::AddOrder add{
        .header = {.stock_locate = kLocate, .tracking_number = 0, .timestamp = 0},
        .order_reference = static_cast<lob::OrderId>(index + 1U),
        .side = lob::Side::Sell,
        .shares = 100U,
        .stock = {},
        .price = kBasePrice + static_cast<lob::Price>(index % 256U),
    };
    if (!engine.apply(add).changed_book) {
      std::cerr << "benchmark setup failed while rebuilding matching book\n";
      return EXIT_FAILURE;
    }
  }

  lob::metrics::LatencyHistogram match_latency;
  std::array<lob::Fill, 1> fill_output{};
  for (std::size_t index = 0; index < order_count; ++index) {
    const auto begin = Clock::now();
    const auto result = engine.submit_market(kLocate, lob::Side::Buy, 100U, fill_output);
    const auto end = Clock::now();
    if (!result.accepted || result.filled != 100U || result.unfilled != 0U ||
        result.fill_count != 1U || result.fills_truncated) {
      std::cerr << "benchmark match produced an unexpected fill\n";
      return EXIT_FAILURE;
    }
    match_latency.record(elapsed_ns(begin, end));
  }

  std::cout << "orders=" << order_count << '\n';
  print("add", add_latency);
  print("cancel", cancel_latency);
  print("match", match_latency);
  return EXIT_SUCCESS;
}
