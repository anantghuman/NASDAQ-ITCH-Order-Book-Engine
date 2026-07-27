#include "lob/order_book.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <string_view>
#include <unordered_map>

namespace {

void require(const bool condition, const std::string_view message) {
  if (!condition) {
    std::cerr << "order_book_model_test: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

struct ReferenceOrder {
  lob::Side side{};
  lob::Price price{};
  lob::Quantity quantity{};
};

// Intentionally simple, non-performance-oriented model used to validate the
// optimized intrusive-list/radix implementation against identical semantics.
class ReferenceBook {
 public:
  [[nodiscard]] bool add(const lob::OrderId id, const lob::Side side, const lob::Price price,
                         const lob::Quantity quantity) {
    if (quantity == 0 || orders_.contains(id)) {
      return false;
    }
    orders_.emplace(id, ReferenceOrder{side, price, quantity});
    levels(side)[price].push_back(id);
    return true;
  }

  [[nodiscard]] bool cancel(const lob::OrderId id, const lob::Quantity quantity) {
    const auto found = orders_.find(id);
    if (found == orders_.end() || quantity == 0 || quantity > found->second.quantity) {
      return false;
    }
    if (quantity == found->second.quantity) {
      return erase(id);
    }
    found->second.quantity -= quantity;
    return true;
  }

  [[nodiscard]] bool execute(const lob::OrderId id, const lob::Quantity quantity) {
    return cancel(id, quantity);
  }

  [[nodiscard]] bool erase(const lob::OrderId id) {
    const auto found = orders_.find(id);
    if (found == orders_.end()) {
      return false;
    }
    remove_from_level(id, found->second);
    orders_.erase(found);
    return true;
  }

  [[nodiscard]] bool modify(const lob::OrderId id, const lob::Price new_price,
                            const lob::Quantity new_quantity) {
    const auto found = orders_.find(id);
    if (found == orders_.end() || new_quantity == 0) {
      return false;
    }
    if (new_price == found->second.price && new_quantity <= found->second.quantity) {
      found->second.quantity = new_quantity;
      return true;
    }

    const ReferenceOrder old = found->second;
    remove_from_level(id, old);
    found->second.price = new_price;
    found->second.quantity = new_quantity;
    levels(found->second.side)[new_price].push_back(id);
    return true;
  }

  [[nodiscard]] bool replace(const lob::OrderId old_id, const lob::OrderId new_id,
                             const lob::Price new_price, const lob::Quantity new_quantity) {
    const auto old = orders_.find(old_id);
    if (old == orders_.end() || new_quantity == 0) {
      return false;
    }
    if (old_id == new_id) {
      return modify(old_id, new_price, new_quantity);
    }
    if (orders_.contains(new_id)) {
      return false;
    }
    const lob::Side side = old->second.side;
    const bool added = add(new_id, side, new_price, new_quantity);
    require(added, "reference replacement add must succeed");
    return erase(old_id);
  }

  [[nodiscard]] bool contains(const lob::OrderId id) const { return orders_.contains(id); }
  [[nodiscard]] std::size_t order_count() const { return orders_.size(); }

  [[nodiscard]] std::optional<lob::OrderId> any_order() const {
    if (orders_.empty()) {
      return std::nullopt;
    }
    return orders_.begin()->first;
  }

  [[nodiscard]] std::optional<ReferenceOrder> order(const lob::OrderId id) const {
    const auto found = orders_.find(id);
    return found == orders_.end() ? std::nullopt : std::optional<ReferenceOrder>(found->second);
  }

  [[nodiscard]] const std::map<lob::Price, std::deque<lob::OrderId>>& levels_for(
      const lob::Side side) const {
    return side == lob::Side::Buy ? bids_ : asks_;
  }

  [[nodiscard]] std::optional<lob::Price> best(const lob::Side side) const {
    const auto& side_levels = levels_for(side);
    if (side_levels.empty()) {
      return std::nullopt;
    }
    return side == lob::Side::Buy ? std::optional<lob::Price>(side_levels.rbegin()->first)
                                  : std::optional<lob::Price>(side_levels.begin()->first);
  }

 private:
  [[nodiscard]] std::map<lob::Price, std::deque<lob::OrderId>>& levels(const lob::Side side) {
    return side == lob::Side::Buy ? bids_ : asks_;
  }

  void remove_from_level(const lob::OrderId id, const ReferenceOrder& order) {
    auto& side_levels = levels(order.side);
    const auto level = side_levels.find(order.price);
    require(level != side_levels.end(), "reference order must have a price level");
    const auto position = std::find(level->second.begin(), level->second.end(), id);
    require(position != level->second.end(), "reference order must be in FIFO queue");
    level->second.erase(position);
    if (level->second.empty()) {
      side_levels.erase(level);
    }
  }

  std::unordered_map<lob::OrderId, ReferenceOrder> orders_;
  std::map<lob::Price, std::deque<lob::OrderId>> bids_;
  std::map<lob::Price, std::deque<lob::OrderId>> asks_;
};

void assert_equivalent(const lob::OrderBook& actual, const ReferenceBook& expected) {
  require(actual.order_count() == expected.order_count(), "order counts must match model");
  require(actual.best_bid() == expected.best(lob::Side::Buy), "best bid must match model");
  require(actual.best_ask() == expected.best(lob::Side::Sell), "best ask must match model");

  for (const lob::Side side : {lob::Side::Buy, lob::Side::Sell}) {
    for (const auto& [price, fifo] : expected.levels_for(side)) {
      std::uint64_t quantity = 0;
      for (const lob::OrderId id : fifo) {
        quantity += expected.order(id)->quantity;
      }
      const auto depth = actual.depth(side, price);
      require(depth.has_value() && depth->quantity == quantity && depth->order_count == fifo.size(),
              "aggregate depth must match model");
      const auto front = actual.front_order(side, price);
      require(front.has_value() && front->order_id == fifo.front(),
              "FIFO head must match model");
    }
  }
}

[[nodiscard]] lob::Price random_price(std::mt19937_64& random) {
  // Regular reuse exercises FIFO at the same price; the high bits exercise the
  // sparse price hierarchy as well.
  return static_cast<lob::Price>((random() % 10'000U) * 100U) |
         static_cast<lob::Price>((random() % 32U) << 24U);
}

[[nodiscard]] lob::OrderId selected_or_unknown(const ReferenceBook& reference,
                                                const lob::OrderId next_id,
                                                std::mt19937_64& random) {
  if ((random() % 5U) == 0U || !reference.any_order().has_value()) {
    return next_id + 1000U + (random() % 1000U);
  }
  return *reference.any_order();
}

void run_model_sequence(const std::uint64_t seed) {
  std::mt19937_64 random(seed);
  lob::OrderBook actual;
  ReferenceBook expected;
  lob::OrderId next_id = 1;

  constexpr std::size_t kOperations = 25'000;
  for (std::size_t step = 0; step < kOperations; ++step) {
    const std::uint64_t operation = random() % 100U;
    if (operation < 35U) {
      const lob::OrderId id = (random() % 20U == 0U && next_id > 1U)
                                  ? next_id - 1U
                                  : next_id++;
      const lob::Side side = (random() & 1U) == 0U ? lob::Side::Buy : lob::Side::Sell;
      const lob::Quantity quantity = (random() % 25U == 0U)
                                         ? 0U
                                         : static_cast<lob::Quantity>(1U + (random() % 10'000U));
      const lob::Price price = random_price(random);
      const bool model_result = expected.add(id, side, price, quantity);
      require(actual.add(id, side, price, quantity) == model_result,
              "add result must match model");
    } else if (operation < 55U) {
      const lob::OrderId id = selected_or_unknown(expected, next_id, random);
      const auto order = expected.order(id);
      const lob::Quantity quantity = !order.has_value() || (random() % 4U) == 0U
                                         ? 0U
                                         : static_cast<lob::Quantity>(1U + (random() % (order->quantity + 1U)));
      require(actual.cancel(id, quantity) == expected.cancel(id, quantity),
              "cancel result must match model");
    } else if (operation < 70U) {
      const lob::OrderId id = selected_or_unknown(expected, next_id, random);
      const auto order = expected.order(id);
      const lob::Quantity quantity = !order.has_value() || (random() % 4U) == 0U
                                         ? 0U
                                         : static_cast<lob::Quantity>(1U + (random() % (order->quantity + 1U)));
      require(actual.execute(id, quantity) == expected.execute(id, quantity),
              "execution result must match model");
    } else if (operation < 85U) {
      const lob::OrderId id = selected_or_unknown(expected, next_id, random);
      const auto order = expected.order(id);
      const lob::Price price = order.has_value() && (random() & 1U) == 0U
                                   ? order->price
                                   : random_price(random);
      const lob::Quantity quantity = !order.has_value() || (random() % 16U) == 0U
                                         ? 0U
                                         : static_cast<lob::Quantity>(1U + (random() % 20'000U));
      require(actual.modify(id, price, quantity) == expected.modify(id, price, quantity),
              "modify result must match model");
    } else if (operation < 95U) {
      const lob::OrderId old_id = selected_or_unknown(expected, next_id, random);
      const lob::OrderId new_id = (random() % 8U == 0U) ? old_id : next_id++;
      const lob::Quantity quantity = (random() % 16U) == 0U
                                         ? 0U
                                         : static_cast<lob::Quantity>(1U + (random() % 20'000U));
      const lob::Price price = random_price(random);
      require(actual.replace(old_id, new_id, price, quantity) ==
                  expected.replace(old_id, new_id, price, quantity),
              "replace result must match model");
    } else {
      const lob::OrderId id = selected_or_unknown(expected, next_id, random);
      require(actual.erase(id) == expected.erase(id), "delete result must match model");
    }

    require(actual.order_count() == expected.order_count(), "order counts must always match model");
    require(actual.best_bid() == expected.best(lob::Side::Buy), "best bid must always match model");
    require(actual.best_ask() == expected.best(lob::Side::Sell), "best ask must always match model");
    if ((step % 97U) == 0U) {
      assert_equivalent(actual, expected);
    }
  }
  assert_equivalent(actual, expected);
}

}  // namespace

int main() {
  run_model_sequence(0x1234'5678'9abc'def0ULL);
  run_model_sequence(0x0bad'f00d'1234'5678ULL);
  run_model_sequence(0xc001'd00d'feeb'deedULL);
  return EXIT_SUCCESS;
}
