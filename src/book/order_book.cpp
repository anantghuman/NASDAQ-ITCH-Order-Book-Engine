#include "lob/order_book.hpp"

#include <array>
#include <bit>
#include <limits>
#include <unordered_map>
#include <utility>

namespace lob {
namespace {

using Mask = std::array<std::uint64_t, 4>;

[[nodiscard]] constexpr std::size_t mask_word(const std::uint8_t value) noexcept {
  return value / 64;
}

[[nodiscard]] constexpr std::uint64_t mask_bit(const std::uint8_t value) noexcept {
  return std::uint64_t{1} << (value % 64);
}

void set(Mask& mask, const std::uint8_t value) noexcept {
  mask[mask_word(value)] |= mask_bit(value);
}

void clear(Mask& mask, const std::uint8_t value) noexcept {
  mask[mask_word(value)] &= ~mask_bit(value);
}

[[nodiscard]] bool any(const Mask& mask) noexcept {
  return (mask[0] | mask[1] | mask[2] | mask[3]) != 0;
}

[[nodiscard]] std::uint8_t lowest(const Mask& mask) noexcept {
  for (std::size_t word = 0; word < mask.size(); ++word) {
    if (mask[word] != 0) {
      const auto bit = static_cast<std::size_t>(std::countr_zero(mask[word]));
      return static_cast<std::uint8_t>(word * 64U + bit);
    }
  }
  return 0;  // Caller establishes that at least one bit is set.
}

[[nodiscard]] std::uint8_t highest(const Mask& mask) noexcept {
  for (std::size_t word = mask.size(); word-- > 0;) {
    if (mask[word] != 0) {
      const auto bit = 63U - static_cast<unsigned>(std::countl_zero(mask[word]));
      return static_cast<std::uint8_t>(word * 64U + bit);
    }
  }
  return 0;  // Caller establishes that at least one bit is set.
}

[[nodiscard]] std::uint8_t byte(const Price price, const unsigned shift) noexcept {
  return static_cast<std::uint8_t>((price >> shift) & 0xffU);
}

/**
 * Sparse fixed-height trie over a 32-bit Price. A mask at each tier skips
 * absent children, which lets best() traverse exactly four byte levels.
 */
class PriceIndex {
 public:
  void add(const Price price) {
    const auto b3 = byte(price, 24);
    const auto b2 = byte(price, 16);
    const auto b1 = byte(price, 8);
    const auto b0 = byte(price, 0);

    // Allocate every needed path before setting an occupancy mask. Thus a
    // throwing allocation cannot make a non-active price visible to best().
    auto& first = first_[b3];
    if (!first) {
      first = std::make_unique<First>();
    }
    auto& second = first->child[b2];
    if (!second) {
      second = std::make_unique<Second>();
    }
    auto& third = second->child[b1];
    if (!third) {
      third = std::make_unique<Third>();
    }

    set(third->prices, b0);
    set(second->children, b1);
    set(first->children, b2);
    set(root_, b3);
  }

  void remove(const Price price) noexcept {
    const auto b3 = byte(price, 24);
    const auto b2 = byte(price, 16);
    const auto b1 = byte(price, 8);
    const auto b0 = byte(price, 0);

    auto& first = first_[b3];
    auto& second = first->child[b2];
    auto& third = second->child[b1];
    clear(third->prices, b0);
    if (any(third->prices)) {
      return;
    }

    third.reset();
    clear(second->children, b1);
    if (any(second->children)) {
      return;
    }

    second.reset();
    clear(first->children, b2);
    if (any(first->children)) {
      return;
    }

    first.reset();
    clear(root_, b3);
  }

  [[nodiscard]] std::optional<Price> minimum() const noexcept {
    return find(false);
  }

  [[nodiscard]] std::optional<Price> maximum() const noexcept {
    return find(true);
  }

  void reset() noexcept {
    for (auto& child : first_) {
      child.reset();
    }
    root_ = {};
  }

 private:
  struct Third {
    Mask prices{};
  };
  struct Second {
    Mask children{};
    std::array<std::unique_ptr<Third>, 256> child{};
  };
  struct First {
    Mask children{};
    std::array<std::unique_ptr<Second>, 256> child{};
  };

  [[nodiscard]] std::optional<Price> find(const bool reverse) const noexcept {
    if (!any(root_)) {
      return std::nullopt;
    }
    const auto b3 = reverse ? highest(root_) : lowest(root_);
    const First& first = *first_[b3];
    const auto b2 = reverse ? highest(first.children) : lowest(first.children);
    const Second& second = *first.child[b2];
    const auto b1 = reverse ? highest(second.children) : lowest(second.children);
    const Third& third = *second.child[b1];
    const auto b0 = reverse ? highest(third.prices) : lowest(third.prices);

    return (static_cast<Price>(b3) << 24U) |
           (static_cast<Price>(b2) << 16U) |
           (static_cast<Price>(b1) << 8U) | static_cast<Price>(b0);
  }

  Mask root_{};
  std::array<std::unique_ptr<First>, 256> first_{};
};

[[nodiscard]] constexpr std::size_t side_index(const Side side) noexcept {
  return side == Side::Buy ? 0U : 1U;
}

}  // namespace

struct OrderBook::Impl {
  struct PriceLevel;

  struct OrderNode {
    OrderId id{};
    Side side{};
    Price price{};
    Quantity quantity{};
    PriceLevel* level{};
    OrderNode* previous{};
    OrderNode* next{};
  };

  struct PriceLevel {
    explicit PriceLevel(const Price price_in) : price(price_in) {}

    Price price{};
    std::uint64_t quantity{};
    std::size_t order_count{};
    OrderNode* head{};
    OrderNode* tail{};
  };

  using OrderMap = std::unordered_map<OrderId, std::unique_ptr<OrderNode>>;
  using LevelMap = std::unordered_map<Price, std::unique_ptr<PriceLevel>>;

  [[nodiscard]] PriceLevel& get_or_create_level(const Side side, const Price price) {
    auto& side_levels = levels[side_index(side)];
    const auto found = side_levels.find(price);
    if (found != side_levels.end()) {
      return *found->second;
    }

    auto level = std::make_unique<PriceLevel>(price);
    auto [inserted, did_insert] = side_levels.emplace(price, std::move(level));
    (void)did_insert;
    return *inserted->second;
  }

  static void append(PriceLevel& level, OrderNode& order) noexcept {
    order.level = &level;
    order.previous = level.tail;
    order.next = nullptr;
    if (level.tail != nullptr) {
      level.tail->next = &order;
    } else {
      level.head = &order;
    }
    level.tail = &order;
    level.quantity += order.quantity;
    ++level.order_count;
  }

  static void detach(PriceLevel& level, OrderNode& order) noexcept {
    if (order.previous != nullptr) {
      order.previous->next = order.next;
    } else {
      level.head = order.next;
    }
    if (order.next != nullptr) {
      order.next->previous = order.previous;
    } else {
      level.tail = order.previous;
    }
    order.previous = nullptr;
    order.next = nullptr;
    order.level = nullptr;
    --level.order_count;
  }

  void erase_node(const OrderMap::iterator order_it) noexcept {
    OrderNode& order = *order_it->second;
    PriceLevel& level = *order.level;
    const Side side = order.side;
    const Price price = order.price;

    level.quantity -= order.quantity;
    detach(level, order);
    if (level.order_count == 0) {
      indices[side_index(side)].remove(price);
      levels[side_index(side)].erase(price);
    }
    orders.erase(order_it);
  }

  std::array<LevelMap, 2> levels;
  std::array<PriceIndex, 2> indices;
  OrderMap orders;
};

OrderBook::OrderBook() : impl_(std::make_unique<Impl>()) {}
OrderBook::~OrderBook() = default;
OrderBook::OrderBook(OrderBook&&) noexcept = default;
OrderBook& OrderBook::operator=(OrderBook&&) noexcept = default;

bool OrderBook::add(const OrderId id, const Side side, const Price price,
                    const Quantity quantity) {
  if (quantity == 0 || impl_->orders.contains(id)) {
    return false;
  }

  Impl::PriceLevel& level = impl_->get_or_create_level(side, price);
  auto order = std::make_unique<Impl::OrderNode>();
  order->id = id;
  order->side = side;
  order->price = price;
  order->quantity = quantity;

  auto [inserted, did_insert] = impl_->orders.emplace(id, std::move(order));
  if (!did_insert) {
    return false;
  }

  const bool was_empty = level.order_count == 0;
  try {
    if (was_empty) {
      impl_->indices[side_index(side)].add(price);
    }
  } catch (...) {
    impl_->orders.erase(inserted);
    throw;
  }
  Impl::append(level, *inserted->second);
  return true;
}

bool OrderBook::cancel(const OrderId id, const Quantity quantity) {
  const auto found = impl_->orders.find(id);
  if (found == impl_->orders.end() || quantity == 0 ||
      quantity > found->second->quantity) {
    return false;
  }

  if (quantity == found->second->quantity) {
    impl_->erase_node(found);
    return true;
  }

  found->second->quantity -= quantity;
  found->second->level->quantity -= quantity;
  return true;
}

bool OrderBook::erase(const OrderId id) {
  const auto found = impl_->orders.find(id);
  if (found == impl_->orders.end()) {
    return false;
  }
  impl_->erase_node(found);
  return true;
}

bool OrderBook::execute(const OrderId id, const Quantity quantity) {
  return cancel(id, quantity);
}

bool OrderBook::modify(const OrderId id, const Price new_price,
                       const Quantity new_quantity) {
  const auto found = impl_->orders.find(id);
  if (found == impl_->orders.end() || new_quantity == 0) {
    return false;
  }

  Impl::OrderNode& order = *found->second;
  Impl::PriceLevel& old_level = *order.level;
  if (new_price == order.price && new_quantity <= order.quantity) {
    old_level.quantity -= static_cast<std::uint64_t>(order.quantity - new_quantity);
    order.quantity = new_quantity;
    return true;
  }

  const Side side = order.side;
  Impl::PriceLevel& new_level = impl_->get_or_create_level(side, new_price);
  const bool activating_new_price = &new_level != &old_level &&
                                    new_level.order_count == 0;
  if (activating_new_price) {
    impl_->indices[side_index(side)].add(new_price);
  }

  old_level.quantity -= order.quantity;
  Impl::detach(old_level, order);
  if (old_level.order_count == 0 && &old_level != &new_level) {
    impl_->indices[side_index(side)].remove(order.price);
    impl_->levels[side_index(side)].erase(order.price);
  }

  order.price = new_price;
  order.quantity = new_quantity;
  Impl::append(new_level, order);
  return true;
}

bool OrderBook::replace(const OrderId old_id, const OrderId new_id,
                        const Price new_price, const Quantity new_quantity) {
  const auto old_order = impl_->orders.find(old_id);
  if (old_order == impl_->orders.end() || new_quantity == 0) {
    return false;
  }
  if (old_id == new_id) {
    return modify(old_id, new_price, new_quantity);
  }
  if (impl_->orders.contains(new_id)) {
    return false;
  }

  // Insert first so an allocation failure preserves the old order. The new
  // order is at the tail, and deleting the old one then gives ITCH replacement
  // its required loss of time priority.
  const Side side = old_order->second->side;
  if (!add(new_id, side, new_price, new_quantity)) {
    return false;
  }
  return erase(old_id);
}

bool OrderBook::contains(const OrderId id) const noexcept {
  return impl_->orders.contains(id);
}

std::optional<OrderView> OrderBook::order(const OrderId id) const {
  const auto found = impl_->orders.find(id);
  if (found == impl_->orders.end()) {
    return std::nullopt;
  }
  const Impl::OrderNode& order = *found->second;
  return OrderView{order.id, order.side, order.price, order.quantity};
}

std::size_t OrderBook::order_count() const noexcept {
  return impl_->orders.size();
}

std::optional<Price> OrderBook::best_bid() const {
  return impl_->indices[side_index(Side::Buy)].maximum();
}

std::optional<Price> OrderBook::best_ask() const {
  return impl_->indices[side_index(Side::Sell)].minimum();
}

std::optional<Depth> OrderBook::best_bid_depth() const {
  const auto price = best_bid();
  return price ? depth(Side::Buy, *price) : std::nullopt;
}

std::optional<Depth> OrderBook::best_ask_depth() const {
  const auto price = best_ask();
  return price ? depth(Side::Sell, *price) : std::nullopt;
}

std::optional<Depth> OrderBook::depth(const Side side, const Price price) const {
  const auto& side_levels = impl_->levels[side_index(side)];
  const auto found = side_levels.find(price);
  if (found == side_levels.end() || found->second->order_count == 0) {
    return std::nullopt;
  }
  const Impl::PriceLevel& level = *found->second;
  return Depth{level.price, level.quantity, level.order_count};
}

std::optional<OrderView> OrderBook::front_order(const Side side,
                                                const Price price) const {
  const auto& side_levels = impl_->levels[side_index(side)];
  const auto found = side_levels.find(price);
  if (found == side_levels.end() || found->second->head == nullptr) {
    return std::nullopt;
  }
  const Impl::OrderNode& order = *found->second->head;
  return OrderView{order.id, order.side, order.price, order.quantity};
}

void OrderBook::clear() noexcept {
  impl_->orders.clear();
  impl_->levels[0].clear();
  impl_->levels[1].clear();
  impl_->indices[0].reset();
  impl_->indices[1].reset();
}

}  // namespace lob
