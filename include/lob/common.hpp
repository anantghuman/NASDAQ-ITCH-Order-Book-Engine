#pragma once

#include <cstdint>

namespace lob {

using Price = std::uint32_t;       // ITCH price: 4 implied decimal places.
using Quantity = std::uint32_t;
using OrderId = std::uint64_t;     // ITCH order reference number.
using MatchId = std::uint64_t;
using StockLocate = std::uint16_t;
using TimestampNs = std::uint64_t; // ITCH carries a 48-bit nanosecond timestamp.
using Sequence = std::uint64_t;

enum class Side : std::uint8_t {
  Buy = 'B',
  Sell = 'S',
};

[[nodiscard]] constexpr Side opposite(const Side side) noexcept {
  return side == Side::Buy ? Side::Sell : Side::Buy;
}

}  // namespace lob
