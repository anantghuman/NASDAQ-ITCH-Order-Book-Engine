#pragma once

#include <cstddef>
#include <limits>
#include <memory>
#include <span>
#include <unordered_map>

#include "lob/common.hpp"
#include "lob/itch.hpp"
#include "lob/order_book.hpp"

namespace lob {

enum class ApplyAction : std::uint8_t {
  Ignored,
  Added,
  Executed,
  Cancelled,
  Deleted,
  Replaced,
  Rejected,
};

struct ApplyResult {
  ApplyAction action{ApplyAction::Ignored};
  StockLocate stock_locate{};
  bool changed_book{false};
};

struct ReplayStats {
  std::uint64_t messages{};
  std::uint64_t ignored{};
  std::uint64_t adds{};
  std::uint64_t executions{};
  std::uint64_t cancels{};
  std::uint64_t deletes{};
  std::uint64_t replaces{};
  std::uint64_t rejected{};
};

struct EngineLimits {
  std::size_t max_instruments{std::numeric_limits<std::size_t>::max()};
  std::size_t max_resting_orders{std::numeric_limits<std::size_t>::max()};
};

struct Fill {
  OrderId resting_order{};
  Price price{};
  Quantity quantity{};
};

struct MatchResult {
  Quantity filled{};
  Quantity resting{};
  Quantity unfilled{};
  std::size_t fill_count{};
  bool fills_truncated{false};
  bool accepted{true};
};

// Applies ITCH order-lifecycle messages to an independent full-depth book for
// every Stock Locate value. It reconstructs historical displayed liquidity; it
// does not submit simulated orders into the historical event stream.
class MatchingEngine {
 public:
  explicit MatchingEngine(EngineLimits limits = {});

  [[nodiscard]] ApplyResult apply(const itch::ItchEvent& event);

  // Simulation-only APIs. Historical ITCH Add messages must use apply(); they
  // represent already-matched exchange state and are not re-matched locally.
  [[nodiscard]] MatchResult submit_limit(StockLocate stock_locate, OrderId order_id, Side side,
                                         Price limit_price, Quantity quantity,
                                         std::span<Fill> fill_output = {});
  [[nodiscard]] MatchResult submit_market(StockLocate stock_locate, Side side,
                                          Quantity quantity, std::span<Fill> fill_output = {});

  [[nodiscard]] const OrderBook* find_book(StockLocate stock_locate) const noexcept;
  [[nodiscard]] OrderBook* find_book(StockLocate stock_locate) noexcept;
  [[nodiscard]] std::size_t instrument_count() const noexcept;
  [[nodiscard]] std::size_t resting_order_count() const noexcept;
  [[nodiscard]] const ReplayStats& stats() const noexcept;
  void clear() noexcept;

 private:
  [[nodiscard]] OrderBook& ensure_book(StockLocate stock_locate);
  [[nodiscard]] bool can_create_order(StockLocate stock_locate) const noexcept;
  [[nodiscard]] ApplyResult reject(StockLocate stock_locate);
  [[nodiscard]] MatchResult match(StockLocate stock_locate, Side side, Price limit_price,
                                  Quantity quantity, bool has_limit,
                                  std::span<Fill> fill_output);

  std::unordered_map<StockLocate, std::unique_ptr<OrderBook>> books_;
  EngineLimits limits_{};
  std::size_t resting_order_count_{};
  ReplayStats stats_{};
};

}  // namespace lob
