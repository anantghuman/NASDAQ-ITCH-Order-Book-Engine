#include "lob/matching_engine.hpp"

#include <type_traits>
#include <utility>

namespace lob {
namespace {

template <class... Callables>
struct Overloaded : Callables... {
  using Callables::operator()...;
};

template <class... Callables>
Overloaded(Callables...) -> Overloaded<Callables...>;

}  // namespace

MatchingEngine::MatchingEngine(const EngineLimits limits) : limits_(limits) {}

ApplyResult MatchingEngine::apply(const itch::ItchEvent& event) {
  ++stats_.messages;

  return std::visit(
      Overloaded{
          [this](const itch::AddOrder& message) {
            const auto locate = message.header.stock_locate;
            if (!can_create_order(locate)) {
              return reject(locate);
            }
            const bool added = ensure_book(locate).add(message.order_reference, message.side,
                                                        message.price, message.shares);
            if (!added) {
              return reject(locate);
            }
            ++resting_order_count_;
            ++stats_.adds;
            return ApplyResult{ApplyAction::Added, locate, true};
          },
          [this](const itch::AddAttributedOrder& message) {
            const auto locate = message.header.stock_locate;
            if (!can_create_order(locate)) {
              return reject(locate);
            }
            const bool added = ensure_book(locate).add(message.order_reference, message.side,
                                                        message.price, message.shares);
            if (!added) {
              return reject(locate);
            }
            ++resting_order_count_;
            ++stats_.adds;
            return ApplyResult{ApplyAction::Added, locate, true};
          },
          [this](const itch::OrderExecuted& message) {
            const auto locate = message.header.stock_locate;
            auto* book = find_book(locate);
            const auto order = book == nullptr ? std::nullopt : book->order(message.order_reference);
            if (!order.has_value() || !book->execute(message.order_reference, message.executed_shares)) {
              return reject(locate);
            }
            if (order->quantity == message.executed_shares) {
              --resting_order_count_;
            }
            ++stats_.executions;
            return ApplyResult{ApplyAction::Executed, locate, true};
          },
          [this](const itch::OrderExecutedWithPrice& message) {
            const auto locate = message.header.stock_locate;
            auto* book = find_book(locate);
            const auto order = book == nullptr ? std::nullopt : book->order(message.order_reference);
            if (!order.has_value() || !book->execute(message.order_reference, message.executed_shares)) {
              return reject(locate);
            }
            if (order->quantity == message.executed_shares) {
              --resting_order_count_;
            }
            ++stats_.executions;
            return ApplyResult{ApplyAction::Executed, locate, true};
          },
          [this](const itch::OrderCancel& message) {
            const auto locate = message.header.stock_locate;
            auto* book = find_book(locate);
            const auto order = book == nullptr ? std::nullopt : book->order(message.order_reference);
            if (!order.has_value() || !book->cancel(message.order_reference, message.canceled_shares)) {
              return reject(locate);
            }
            if (order->quantity == message.canceled_shares) {
              --resting_order_count_;
            }
            ++stats_.cancels;
            return ApplyResult{ApplyAction::Cancelled, locate, true};
          },
          [this](const itch::OrderDelete& message) {
            const auto locate = message.header.stock_locate;
            auto* book = find_book(locate);
            if (book == nullptr || !book->erase(message.order_reference)) {
              return reject(locate);
            }
            --resting_order_count_;
            ++stats_.deletes;
            return ApplyResult{ApplyAction::Deleted, locate, true};
          },
          [this](const itch::OrderReplace& message) {
            const auto locate = message.header.stock_locate;
            auto* book = find_book(locate);
            if (book == nullptr ||
                !book->replace(message.original_order_reference, message.new_order_reference,
                               message.price, message.shares)) {
              return reject(locate);
            }
            ++stats_.replaces;
            return ApplyResult{ApplyAction::Replaced, locate, true};
          },
          [this](const itch::SkippedMessage& message) {
            ++stats_.ignored;
            return ApplyResult{ApplyAction::Ignored,
                               message.header.has_value() ? message.header->stock_locate
                                                          : static_cast<StockLocate>(0),
                               false};
          },
          [this](const auto& message) {
            ++stats_.ignored;
            return ApplyResult{ApplyAction::Ignored, message.header.stock_locate, false};
          },
      },
      event);
}

MatchResult MatchingEngine::submit_limit(const StockLocate stock_locate, const OrderId order_id,
                                         const Side side, const Price limit_price,
                                         const Quantity quantity,
                                         const std::span<Fill> fill_output) {
  if (quantity == 0 || (find_book(stock_locate) != nullptr &&
                        find_book(stock_locate)->contains(order_id))) {
    return MatchResult{.accepted = false};
  }

  auto result = match(stock_locate, side, limit_price, quantity, true, fill_output);
  if (result.resting != 0) {
    if (!can_create_order(stock_locate) ||
        !ensure_book(stock_locate).add(order_id, side, limit_price, result.resting)) {
      result.unfilled += result.resting;
      result.resting = 0;
      result.accepted = false;
    } else {
      ++resting_order_count_;
    }
  }
  return result;
}

MatchResult MatchingEngine::submit_market(const StockLocate stock_locate, const Side side,
                                          const Quantity quantity,
                                          const std::span<Fill> fill_output) {
  if (quantity == 0) {
    return MatchResult{.accepted = false};
  }
  auto result = match(stock_locate, side, 0, quantity, false, fill_output);
  result.unfilled = result.resting;
  result.resting = 0;
  return result;
}

const OrderBook* MatchingEngine::find_book(const StockLocate stock_locate) const noexcept {
  const auto found = books_.find(stock_locate);
  return found == books_.end() ? nullptr : found->second.get();
}

OrderBook* MatchingEngine::find_book(const StockLocate stock_locate) noexcept {
  const auto found = books_.find(stock_locate);
  return found == books_.end() ? nullptr : found->second.get();
}

std::size_t MatchingEngine::instrument_count() const noexcept { return books_.size(); }

std::size_t MatchingEngine::resting_order_count() const noexcept { return resting_order_count_; }

const ReplayStats& MatchingEngine::stats() const noexcept { return stats_; }

void MatchingEngine::clear() noexcept {
  books_.clear();
  resting_order_count_ = 0;
  stats_ = {};
}

OrderBook& MatchingEngine::ensure_book(const StockLocate stock_locate) {
  auto [entry, inserted] = books_.try_emplace(stock_locate);
  if (inserted) {
    entry->second = std::make_unique<OrderBook>();
  }
  return *entry->second;
}

bool MatchingEngine::can_create_order(const StockLocate stock_locate) const noexcept {
  if (resting_order_count_ >= limits_.max_resting_orders) {
    return false;
  }
  return find_book(stock_locate) != nullptr || books_.size() < limits_.max_instruments;
}

ApplyResult MatchingEngine::reject(const StockLocate stock_locate) {
  ++stats_.rejected;
  return ApplyResult{ApplyAction::Rejected, stock_locate, false};
}

MatchResult MatchingEngine::match(const StockLocate stock_locate, const Side side,
                                  const Price limit_price, const Quantity quantity,
                                  const bool has_limit, const std::span<Fill> fill_output) {
  MatchResult result{};
  auto* book = find_book(stock_locate);
  if (book == nullptr) {
    result.resting = quantity;
    return result;
  }
  Quantity remaining = quantity;

  while (remaining != 0) {
    const auto opposite_price = side == Side::Buy ? book->best_ask() : book->best_bid();
    if (!opposite_price.has_value()) {
      break;
    }

    const bool crosses = side == Side::Buy ? limit_price >= *opposite_price
                                            : limit_price <= *opposite_price;
    if (has_limit && !crosses) {
      break;
    }

    const auto resting = book->front_order(opposite(side), *opposite_price);
    if (!resting.has_value()) {
      // A level can only be marked active when it contains a front order. Do
      // not spin indefinitely if a caller discovers a broken book invariant.
      result.accepted = false;
      break;
    }

    const Quantity fill_quantity = remaining < resting->quantity ? remaining : resting->quantity;
    if (!book->execute(resting->order_id, fill_quantity)) {
      result.accepted = false;
      break;
    }
    if (result.fill_count < fill_output.size()) {
      fill_output[result.fill_count] = Fill{resting->order_id, *opposite_price, fill_quantity};
    } else {
      result.fills_truncated = true;
    }
    ++result.fill_count;
    result.filled += fill_quantity;
    if (fill_quantity == resting->quantity) {
      --resting_order_count_;
    }
    remaining -= fill_quantity;
  }

  result.resting = remaining;
  return result;
}

}  // namespace lob
