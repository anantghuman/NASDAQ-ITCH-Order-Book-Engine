#include "lob/order_book.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void require(const bool condition, const std::string_view message) {
  if (!condition) {
    std::cerr << "order_book_test: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void test_depth_and_best_prices() {
  lob::OrderBook book;
  require(!book.best_bid() && !book.best_ask(), "empty book has no best prices");

  require(book.add(1, lob::Side::Buy, 10'000, 30), "add first bid");
  require(book.add(2, lob::Side::Buy, 10'000, 20), "add second bid");
  require(book.add(3, lob::Side::Buy, 9'999, 10), "add lower bid");
  require(book.add(4, lob::Side::Sell, 10'002, 25), "add offer");
  require(book.add(5, lob::Side::Sell, 10'001, 15), "add better offer");

  require(book.best_bid() == 10'000, "highest bid is best bid");
  require(book.best_ask() == 10'001, "lowest offer is best ask");
  const auto bid_depth = book.depth(lob::Side::Buy, 10'000);
  require(bid_depth && bid_depth->quantity == 50 && bid_depth->order_count == 2,
          "same-price bid depth aggregates quantities and order count");
  const auto ask_depth = book.best_ask_depth();
  require(ask_depth && ask_depth->price == 10'001 && ask_depth->quantity == 15,
          "best ask depth is exposed");
  require(book.order_count() == 5, "all resting orders counted");
}

void test_priority_and_modify_rules() {
  lob::OrderBook book;
  require(book.add(10, lob::Side::Buy, 500, 10), "add first FIFO order");
  require(book.add(11, lob::Side::Buy, 500, 20), "add second FIFO order");
  require(book.front_order(lob::Side::Buy, 500)->order_id == 10,
          "first order starts at FIFO head");

  require(book.modify(10, 500, 5), "same-price reduction succeeds");
  require(book.front_order(lob::Side::Buy, 500)->order_id == 10,
          "same-price reduction keeps time priority");
  require(book.depth(lob::Side::Buy, 500)->quantity == 25,
          "reduction updates aggregate depth");

  require(book.modify(10, 500, 7), "same-price increase succeeds");
  require(book.front_order(lob::Side::Buy, 500)->order_id == 11,
          "same-price increase loses priority");
  require(book.depth(lob::Side::Buy, 500)->quantity == 27,
          "increase updates aggregate depth");

  require(book.modify(11, 501, 20), "price change succeeds");
  require(book.best_bid() == 501, "price change updates best bid");
  require(book.front_order(lob::Side::Buy, 500)->order_id == 10,
          "price change removes old FIFO member");
  require(book.front_order(lob::Side::Buy, 501)->order_id == 11,
          "price change appends at new level");
}

void test_cancel_execute_delete_and_replace() {
  lob::OrderBook book;
  require(book.add(20, lob::Side::Sell, 1'000, 50), "add sell order");
  require(!book.cancel(20, 51), "over-cancel rejected");
  require(!book.cancel(20, 0), "zero cancel rejected");
  require(book.cancel(20, 15), "partial cancel succeeds");
  require(book.order(20)->quantity == 35, "partial cancel updates order");
  require(book.execute(20, 34), "partial execute succeeds");
  require(book.order(20)->quantity == 1, "execute decrements order");
  require(book.execute(20, 1), "final execute succeeds");
  require(!book.contains(20) && !book.depth(lob::Side::Sell, 1'000),
          "final execute removes order and level");

  require(book.add(21, lob::Side::Sell, 1'005, 4), "add replace source");
  require(book.add(22, lob::Side::Sell, 1'005, 9), "add later sell order");
  require(book.replace(21, 31, 1'005, 6), "replace succeeds");
  require(!book.contains(21) && book.contains(31), "replace changes reference number");
  require(book.order(31)->side == lob::Side::Sell, "replace inherits side");
  require(book.front_order(lob::Side::Sell, 1'005)->order_id == 22,
          "replacement loses time priority at same price");
  require(book.erase(22) && book.erase(31), "delete removes remaining orders");
  require(book.order_count() == 0, "delete empties book");
}

void test_sparse_full_32_bit_price_range() {
  lob::OrderBook book;
  constexpr lob::Price kMaxPrice = 0xffff'ffffU;
  require(book.add(40, lob::Side::Buy, 0, 1), "add zero-price bid");
  require(book.add(41, lob::Side::Buy, kMaxPrice, 1), "add max-price bid");
  require(book.add(42, lob::Side::Sell, kMaxPrice, 1), "add max-price offer");
  require(book.add(43, lob::Side::Sell, 0, 1), "add zero-price offer");
  require(book.best_bid() == kMaxPrice, "sparse index finds max 32-bit price");
  require(book.best_ask() == 0, "sparse index finds min 32-bit price");

  book.clear();
  require(book.order_count() == 0 && !book.best_bid() && !book.best_ask(),
          "clear releases every active level and index entry");
}

}  // namespace

int main() {
  test_depth_and_best_prices();
  test_priority_and_modify_rules();
  test_cancel_execute_delete_and_replace();
  test_sparse_full_32_bit_price_range();
  return EXIT_SUCCESS;
}
