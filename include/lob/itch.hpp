#pragma once

#include "lob/common.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

namespace lob::itch {

// NASDAQ encodes stock symbols and MPIDs as fixed-width, space-padded ASCII.
using StockSymbol = std::array<char, 8>;
using MarketParticipantId = std::array<char, 4>;

struct MessageHeader {
  StockLocate stock_locate{};
  std::uint16_t tracking_number{};
  TimestampNs timestamp{};
};

struct SystemEvent {
  MessageHeader header{};
  char event_code{};
};

struct StockDirectory {
  MessageHeader header{};
  StockSymbol stock{};
  char market_category{};
  char financial_status_indicator{};
  std::uint32_t round_lot_size{};
  char round_lots_only{};
  char issue_classification{};
  std::array<char, 2> issue_sub_type{};
  char authenticity{};
  char short_sale_threshold_indicator{};
  char ipo_flag{};
  char luld_reference_price_tier{};
  char etp_flag{};
  std::uint32_t etp_leverage_factor{};
  char inverse_indicator{};
};

struct TradingAction {
  MessageHeader header{};
  StockSymbol stock{};
  char trading_state{};
  char reserved{};
  std::array<char, 4> reason{};
};

struct AddOrder {
  MessageHeader header{};
  OrderId order_reference{};
  Side side{};
  Quantity shares{};
  StockSymbol stock{};
  Price price{};
};

struct AddAttributedOrder {
  MessageHeader header{};
  OrderId order_reference{};
  Side side{};
  Quantity shares{};
  StockSymbol stock{};
  Price price{};
  MarketParticipantId attribution{};
};

struct OrderExecuted {
  MessageHeader header{};
  OrderId order_reference{};
  Quantity executed_shares{};
  MatchId match_number{};
};

struct OrderExecutedWithPrice {
  MessageHeader header{};
  OrderId order_reference{};
  Quantity executed_shares{};
  MatchId match_number{};
  char printable{};
  Price execution_price{};
};

struct OrderCancel {
  MessageHeader header{};
  OrderId order_reference{};
  Quantity canceled_shares{};
};

struct OrderDelete {
  MessageHeader header{};
  OrderId order_reference{};
};

struct OrderReplace {
  MessageHeader header{};
  OrderId original_order_reference{};
  OrderId new_order_reference{};
  Quantity shares{};
  Price price{};
};

// Non-cross trade (P). This is an observable market-data event and does not
// necessarily imply a displayed-order lifecycle transition in the local book.
struct Trade {
  MessageHeader header{};
  OrderId order_reference{};
  Side side{};
  Quantity shares{};
  StockSymbol stock{};
  Price price{};
  MatchId match_number{};
};

struct CrossTrade {
  MessageHeader header{};
  std::uint64_t shares{};
  StockSymbol stock{};
  Price cross_price{};
  MatchId match_number{};
  char cross_type{};
};

struct BrokenTrade {
  MessageHeader header{};
  MatchId match_number{};
};

struct NetOrderImbalance {
  MessageHeader header{};
  std::uint64_t paired_shares{};
  std::uint64_t imbalance_shares{};
  char imbalance_direction{};
  StockSymbol stock{};
  Price far_price{};
  Price near_price{};
  Price current_reference_price{};
  char cross_type{};
  char price_variation_indicator{};
};

// A syntactically complete ITCH frame whose type is not modelled by this
// decoder revision.  Keeping its standard header lets a replay account for
// the event and continue through historical files containing administrative
// messages that do not affect displayed order depth.
struct SkippedMessage {
  char message_type{};
  // Most TotalView-ITCH administrative messages have the normal 10-byte
  // header after the type byte. A few control records do not, so replay must
  // retain the frame without assuming a locator is present.
  std::optional<MessageHeader> header{};
};

using ItchEvent = std::variant<SystemEvent,
                               StockDirectory,
                               TradingAction,
                               AddOrder,
                               AddAttributedOrder,
                               OrderExecuted,
                               OrderExecutedWithPrice,
                               OrderCancel,
                               OrderDelete,
                               OrderReplace,
                               Trade,
                               CrossTrade,
                               BrokenTrade,
                               NetOrderImbalance,
                               SkippedMessage>;

enum class DecodeStatus : std::uint8_t {
  Complete,
  NeedMoreData,
  Error,
};

enum class DecodeErrorCode : std::uint8_t {
  InvalidFrameLength,
  TruncatedFrame,
  UnexpectedMessageLength,
  UnsupportedMessageType,
  InvalidSide,
};

struct DecodeError {
  DecodeErrorCode code{};
  // Offset relative to the beginning of the input span (including its frame
  // length prefix) where the decoder identified the problem.
  std::size_t offset{};
  const char* reason{};
};

struct DecodedMessage {
  ItchEvent event{};
  // Includes the two-byte native ITCH frame-length prefix.
  std::size_t bytes_consumed{};
};

struct DecodeResult {
  DecodeStatus status{DecodeStatus::NeedMoreData};
  std::optional<DecodedMessage> message{};
  std::optional<DecodeError> error{};
};

// Stateless decoder for a native TotalView-ITCH 5.0 framed message. The
// caller can retain unconsumed input and call this repeatedly while streaming
// from a file or socket. No input field aliases the supplied byte span.
class ItchDecoder {
 public:
  [[nodiscard]] static DecodeResult decode_next(
      std::span<const std::uint8_t> input) noexcept;
};

}  // namespace lob::itch
