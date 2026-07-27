#include "lob/matching_engine.hpp"

#include <array>
#include <cstdlib>
#include <iostream>

namespace {

void require(const bool condition, const char* const message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

lob::itch::MessageHeader header(const lob::StockLocate locate = 1) {
  return lob::itch::MessageHeader{locate, 0, 0};
}

lob::itch::AddOrder add(const lob::OrderId id, const lob::Side side, const lob::Quantity shares,
                        const lob::Price price, const lob::StockLocate locate = 1) {
  return lob::itch::AddOrder{header(locate), id, side, shares, {}, price};
}

}  // namespace

int main() {
  lob::MatchingEngine engine;

  require(engine.apply(add(100, lob::Side::Buy, 50, 10'000)).action == lob::ApplyAction::Added,
          "adds must reconstruct resting orders");
  require(engine.apply(add(101, lob::Side::Sell, 40, 10'200)).changed_book,
          "second add must change its book");
  require(engine.find_book(1)->best_bid() == 10'000, "best bid must be exposed");
  require(engine.find_book(1)->best_ask() == 10'200, "best ask must be exposed");

  const lob::itch::OrderExecuted partial{header(), 100, 20, 1};
  require(engine.apply(partial).action == lob::ApplyAction::Executed,
          "execution must decrement the referenced order");
  require(engine.find_book(1)->order(100)->quantity == 30,
          "partial execution must retain the remaining quantity");

  const lob::itch::OrderReplace replace{header(), 100, 102, 25, 10'050};
  require(engine.apply(replace).action == lob::ApplyAction::Replaced,
          "replace must remove old reference and rest a new one");
  require(!engine.find_book(1)->contains(100) && engine.find_book(1)->contains(102),
          "replace must update the order reference");

  // A separate instrument must never share an order book with the first.
  require(engine.apply(add(200, lob::Side::Sell, 5, 500, 2)).changed_book,
          "add on another stock locate must be accepted");
  require(engine.instrument_count() == 2, "books must be isolated by stock locate");

  // Simulation consumes the oldest orders at the best price before moving on.
  lob::MatchingEngine simulation;
  require(simulation.apply(add(1, lob::Side::Sell, 30, 100, 7)).changed_book,
          "first simulation ask must be added");
  require(simulation.apply(add(2, lob::Side::Sell, 10, 100, 7)).changed_book,
          "second simulation ask must be added");
  require(simulation.apply(add(3, lob::Side::Sell, 10, 101, 7)).changed_book,
          "next-level simulation ask must be added");
  std::array<lob::Fill, 3> fills{};
  const auto result = simulation.submit_limit(7, 4, lob::Side::Buy, 101, 45, fills);
  require(result.accepted && result.filled == 45 && result.resting == 0,
          "marketable limit must sweep available levels");
  require(result.fill_count == 3 && !result.fills_truncated && fills[0].resting_order == 1 &&
              fills[0].quantity == 30 && fills[1].resting_order == 2 &&
              fills[1].quantity == 10 && fills[2].resting_order == 3 && fills[2].quantity == 5,
          "matching must honour time then price priority");
  require(simulation.find_book(7)->order(3)->quantity == 5,
          "partial sweep must leave residual resting quantity");

  const auto resting = simulation.submit_limit(7, 5, lob::Side::Buy, 99, 11);
  require(resting.accepted && resting.filled == 0 && resting.resting == 11,
          "non-marketable limit must rest in the book");
  require(simulation.find_book(7)->contains(5), "unfilled order must receive its submitted id");

  lob::MatchingEngine limited({.max_instruments = 1, .max_resting_orders = 1});
  require(limited.apply(add(1, lob::Side::Buy, 1, 100, 9)).changed_book,
          "engine limit allows the first resting order");
  require(limited.resting_order_count() == 1, "resting-order count tracks successful add");
  require(limited.apply(add(2, lob::Side::Buy, 1, 100, 9)).action == lob::ApplyAction::Rejected,
          "engine limit rejects an order beyond the configured cap");
  require(limited.apply(lob::itch::OrderDelete{header(9), 1}).changed_book,
          "delete below the cap succeeds");
  require(limited.resting_order_count() == 0, "delete updates resting-order count");

  lob::MatchingEngine truncation;
  require(truncation.apply(add(10, lob::Side::Sell, 1, 100, 8)).changed_book,
          "first truncation ask must be added");
  require(truncation.apply(add(11, lob::Side::Sell, 1, 100, 8)).changed_book,
          "second truncation ask must be added");
  std::array<lob::Fill, 1> short_output{};
  const auto truncated = truncation.submit_market(8, lob::Side::Buy, 2, short_output);
  require(truncated.filled == 2 && truncated.unfilled == 0 && truncated.fill_count == 2 &&
              truncated.fills_truncated && short_output[0].resting_order == 10,
          "matching must remain complete when the caller supplies a short fill buffer");

  return 0;
}
