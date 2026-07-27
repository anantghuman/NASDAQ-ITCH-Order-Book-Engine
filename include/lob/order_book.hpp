#pragma once

#include "lob/common.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace lob {

/** Aggregated displayed quantity at one price on one side of the book. */
struct Depth {
  Price price{};
  std::uint64_t quantity{};
  std::size_t order_count{};
};

/** A read-only snapshot of an individual resting order. */
struct OrderView {
  OrderId order_id{};
  Side side{};
  Price price{};
  Quantity quantity{};
};

/**
 * A full-depth, price-time-priority limit order book for one instrument.
 *
 * Each active price owns an intrusive FIFO order list.  An order-id hash index
 * points directly at its list node, so cancellation, execution decrement, and
 * re-prioritising modification perform expected O(1) lookup and O(1) unlink /
 * append. (The expectation is the usual hash-table qualification.)
 *
 * Prices are not stored in a fixed, dense tick array.  A sparse, four-tier
 * bitmap indexes the four bytes of the 32-bit ITCH price.  Activating,
 * deactivating, and finding the best price therefore visit at most four fixed
 * tiers, while allocating nodes only for prefixes that occur in the book.
 * Per-price depth lookup is expected O(1).
 *
 * This class is intentionally single-writer.  Its caller is responsible for
 * serialising mutations (normally the matching/reconstruction thread).
 */
class OrderBook {
 public:
  OrderBook();
  ~OrderBook();

  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;
  OrderBook(OrderBook&&) noexcept;
  OrderBook& operator=(OrderBook&&) noexcept;

  /** Rest a new order at the tail of its price-level FIFO queue. */
  [[nodiscard]] bool add(OrderId id, Side side, Price price, Quantity quantity);

  /** Cancel shares from a resting order; returns false on an invalid request. */
  [[nodiscard]] bool cancel(OrderId id, Quantity quantity);

  /** Remove a resting order in full (the ITCH Order Delete operation). */
  [[nodiscard]] bool erase(OrderId id);

  /** Decrement a resting order following an execution; removes it at zero. */
  [[nodiscard]] bool execute(OrderId id, Quantity quantity);

  /**
   * Update an order in place. A same-price size reduction retains FIFO
   * priority. A size increase or any price change moves the order to the tail
   * of its (new) price level.
   */
  [[nodiscard]] bool modify(OrderId id, Price new_price, Quantity new_quantity);

  /**
   * ITCH-style replace: remove old_id and introduce new_id at the tail of the
   * requested price level. The new order inherits old_id's side.
   */
  [[nodiscard]] bool replace(OrderId old_id, OrderId new_id, Price new_price,
                             Quantity new_quantity);

  [[nodiscard]] bool contains(OrderId id) const noexcept;
  [[nodiscard]] std::optional<OrderView> order(OrderId id) const;
  [[nodiscard]] std::size_t order_count() const noexcept;

  [[nodiscard]] std::optional<Price> best_bid() const;
  [[nodiscard]] std::optional<Price> best_ask() const;
  [[nodiscard]] std::optional<Depth> best_bid_depth() const;
  [[nodiscard]] std::optional<Depth> best_ask_depth() const;
  [[nodiscard]] std::optional<Depth> depth(Side side, Price price) const;
  /** Oldest resting order at a price, useful to a matching engine. */
  [[nodiscard]] std::optional<OrderView> front_order(Side side, Price price) const;

  void clear() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace lob
