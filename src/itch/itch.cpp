#include "lob/itch.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace lob::itch {
namespace {

constexpr std::size_t kFramePrefixBytes = 2;
constexpr std::size_t kCommonHeaderBytes = 11;

template <typename Unsigned>
[[nodiscard]] Unsigned read_be(const std::span<const std::uint8_t> bytes,
                               const std::size_t offset) noexcept {
  Unsigned value{};
  for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
    value = static_cast<Unsigned>(
        (value << 8U) | static_cast<Unsigned>(bytes[offset + index]));
  }
  return value;
}

[[nodiscard]] TimestampNs read_timestamp(
    const std::span<const std::uint8_t> bytes,
    const std::size_t offset) noexcept {
  TimestampNs value{};
  for (std::size_t index = 0; index < 6; ++index) {
    value = static_cast<TimestampNs>((value << 8U) | bytes[offset + index]);
  }
  return value;
}

template <std::size_t Count>
[[nodiscard]] std::array<char, Count> read_chars(
    const std::span<const std::uint8_t> bytes,
    const std::size_t offset) noexcept {
  std::array<char, Count> value{};
  for (std::size_t index = 0; index < Count; ++index) {
    value[index] = static_cast<char>(bytes[offset + index]);
  }
  return value;
}

[[nodiscard]] MessageHeader read_header(
    const std::span<const std::uint8_t> message) noexcept {
  return MessageHeader{
      .stock_locate = read_be<StockLocate>(message, 1),
      .tracking_number = read_be<std::uint16_t>(message, 3),
      .timestamp = read_timestamp(message, 5),
  };
}

[[nodiscard]] DecodeResult need_more_data() noexcept {
  return DecodeResult{.status = DecodeStatus::NeedMoreData};
}

[[nodiscard]] DecodeResult error(const DecodeErrorCode code,
                                 const std::size_t offset,
                                 const char* const reason) noexcept {
  return DecodeResult{
      .status = DecodeStatus::Error,
      .error = DecodeError{.code = code, .offset = offset, .reason = reason},
  };
}

[[nodiscard]] DecodeResult complete(ItchEvent event,
                                    const std::size_t bytes_consumed) noexcept {
  return DecodeResult{
      .status = DecodeStatus::Complete,
      .message = DecodedMessage{
          .event = std::move(event),
          .bytes_consumed = bytes_consumed,
      },
  };
}

[[nodiscard]] bool valid_side(const char value) noexcept {
  return value == static_cast<char>(Side::Buy) ||
         value == static_cast<char>(Side::Sell);
}

[[nodiscard]] Side to_side(const char value) noexcept {
  return value == static_cast<char>(Side::Buy) ? Side::Buy : Side::Sell;
}

[[nodiscard]] DecodeResult wrong_size(const char type,
                                      const std::size_t actual,
                                      const std::size_t expected) noexcept {
  static_cast<void>(type);
  static_cast<void>(actual);
  static_cast<void>(expected);
  return error(DecodeErrorCode::UnexpectedMessageLength, kFramePrefixBytes,
               "message length does not match ITCH 5.0 type");
}

[[nodiscard]] DecodeResult decode_message(
    const std::span<const std::uint8_t> message,
    const std::size_t bytes_consumed) noexcept {
  const char type = static_cast<char>(message[0]);
  const auto require_size = [&](const std::size_t expected) {
    return message.size() == expected;
  };
  const auto require_side = [&](const std::size_t offset) -> bool {
    return valid_side(static_cast<char>(message[offset]));
  };

  switch (type) {
    case 'S': {
      if (!require_size(12)) return wrong_size(type, message.size(), 12);
      return complete(SystemEvent{.header = read_header(message),
                                  .event_code = static_cast<char>(message[11])},
                      bytes_consumed);
    }
    case 'R': {
      if (!require_size(39)) return wrong_size(type, message.size(), 39);
      return complete(
          StockDirectory{
              .header = read_header(message),
              .stock = read_chars<8>(message, 11),
              .market_category = static_cast<char>(message[19]),
              .financial_status_indicator = static_cast<char>(message[20]),
              .round_lot_size = read_be<std::uint32_t>(message, 21),
              .round_lots_only = static_cast<char>(message[25]),
              .issue_classification = static_cast<char>(message[26]),
              .issue_sub_type = read_chars<2>(message, 27),
              .authenticity = static_cast<char>(message[29]),
              .short_sale_threshold_indicator = static_cast<char>(message[30]),
              .ipo_flag = static_cast<char>(message[31]),
              .luld_reference_price_tier = static_cast<char>(message[32]),
              .etp_flag = static_cast<char>(message[33]),
              .etp_leverage_factor = read_be<std::uint32_t>(message, 34),
              .inverse_indicator = static_cast<char>(message[38]),
          },
          bytes_consumed);
    }
    case 'H': {
      if (!require_size(25)) return wrong_size(type, message.size(), 25);
      return complete(
          TradingAction{
              .header = read_header(message),
              .stock = read_chars<8>(message, 11),
              .trading_state = static_cast<char>(message[19]),
              .reserved = static_cast<char>(message[20]),
              .reason = read_chars<4>(message, 21),
          },
          bytes_consumed);
    }
    case 'A': {
      if (!require_size(36)) return wrong_size(type, message.size(), 36);
      if (!require_side(19)) {
        return error(DecodeErrorCode::InvalidSide, kFramePrefixBytes + 19,
                     "add order side must be B or S");
      }
      return complete(
          AddOrder{
              .header = read_header(message),
              .order_reference = read_be<OrderId>(message, 11),
              .side = to_side(static_cast<char>(message[19])),
              .shares = read_be<Quantity>(message, 20),
              .stock = read_chars<8>(message, 24),
              .price = read_be<Price>(message, 32),
          },
          bytes_consumed);
    }
    case 'F': {
      if (!require_size(40)) return wrong_size(type, message.size(), 40);
      if (!require_side(19)) {
        return error(DecodeErrorCode::InvalidSide, kFramePrefixBytes + 19,
                     "attributed add order side must be B or S");
      }
      return complete(
          AddAttributedOrder{
              .header = read_header(message),
              .order_reference = read_be<OrderId>(message, 11),
              .side = to_side(static_cast<char>(message[19])),
              .shares = read_be<Quantity>(message, 20),
              .stock = read_chars<8>(message, 24),
              .price = read_be<Price>(message, 32),
              .attribution = read_chars<4>(message, 36),
          },
          bytes_consumed);
    }
    case 'E': {
      if (!require_size(31)) return wrong_size(type, message.size(), 31);
      return complete(
          OrderExecuted{
              .header = read_header(message),
              .order_reference = read_be<OrderId>(message, 11),
              .executed_shares = read_be<Quantity>(message, 19),
              .match_number = read_be<MatchId>(message, 23),
          },
          bytes_consumed);
    }
    case 'C': {
      if (!require_size(36)) return wrong_size(type, message.size(), 36);
      return complete(
          OrderExecutedWithPrice{
              .header = read_header(message),
              .order_reference = read_be<OrderId>(message, 11),
              .executed_shares = read_be<Quantity>(message, 19),
              .match_number = read_be<MatchId>(message, 23),
              .printable = static_cast<char>(message[31]),
              .execution_price = read_be<Price>(message, 32),
          },
          bytes_consumed);
    }
    case 'X': {
      if (!require_size(23)) return wrong_size(type, message.size(), 23);
      return complete(
          OrderCancel{
              .header = read_header(message),
              .order_reference = read_be<OrderId>(message, 11),
              .canceled_shares = read_be<Quantity>(message, 19),
          },
          bytes_consumed);
    }
    case 'D': {
      if (!require_size(19)) return wrong_size(type, message.size(), 19);
      return complete(OrderDelete{.header = read_header(message),
                                  .order_reference = read_be<OrderId>(message, 11)},
                      bytes_consumed);
    }
    case 'U': {
      if (!require_size(35)) return wrong_size(type, message.size(), 35);
      return complete(
          OrderReplace{
              .header = read_header(message),
              .original_order_reference = read_be<OrderId>(message, 11),
              .new_order_reference = read_be<OrderId>(message, 19),
              .shares = read_be<Quantity>(message, 27),
              .price = read_be<Price>(message, 31),
          },
          bytes_consumed);
    }
    case 'P': {
      if (!require_size(44)) return wrong_size(type, message.size(), 44);
      if (!require_side(19)) {
        return error(DecodeErrorCode::InvalidSide, kFramePrefixBytes + 19,
                     "trade side must be B or S");
      }
      return complete(
          Trade{
              .header = read_header(message),
              .order_reference = read_be<OrderId>(message, 11),
              .side = to_side(static_cast<char>(message[19])),
              .shares = read_be<Quantity>(message, 20),
              .stock = read_chars<8>(message, 24),
              .price = read_be<Price>(message, 32),
              .match_number = read_be<MatchId>(message, 36),
          },
          bytes_consumed);
    }
    case 'Q': {
      if (!require_size(40)) return wrong_size(type, message.size(), 40);
      return complete(
          CrossTrade{
              .header = read_header(message),
              .shares = read_be<std::uint64_t>(message, 11),
              .stock = read_chars<8>(message, 19),
              .cross_price = read_be<Price>(message, 27),
              .match_number = read_be<MatchId>(message, 31),
              .cross_type = static_cast<char>(message[39]),
          },
          bytes_consumed);
    }
    case 'B': {
      if (!require_size(19)) return wrong_size(type, message.size(), 19);
      return complete(BrokenTrade{.header = read_header(message),
                                  .match_number = read_be<MatchId>(message, 11)},
                      bytes_consumed);
    }
    case 'I': {
      if (!require_size(50)) return wrong_size(type, message.size(), 50);
      return complete(
          NetOrderImbalance{
              .header = read_header(message),
              .paired_shares = read_be<std::uint64_t>(message, 11),
              .imbalance_shares = read_be<std::uint64_t>(message, 19),
              .imbalance_direction = static_cast<char>(message[27]),
              .stock = read_chars<8>(message, 28),
              .far_price = read_be<Price>(message, 36),
              .near_price = read_be<Price>(message, 40),
              .current_reference_price = read_be<Price>(message, 44),
              .cross_type = static_cast<char>(message[48]),
              .price_variation_indicator = static_cast<char>(message[49]),
          },
          bytes_consumed);
    }
    default:
      // ITCH historical files include administrative messages that are not
      // relevant to displayed-book reconstruction (for example Reg SHO and
      // market-wide circuit-breaker updates). Their frame lengths are still
      // authoritative, so surface a typed skip event and let replay continue.
      return complete(
          SkippedMessage{
              .message_type = type,
              .header = message.size() >= kCommonHeaderBytes
                            ? std::optional<MessageHeader>(read_header(message))
                            : std::nullopt,
          },
          bytes_consumed);
  }
}

}  // namespace

DecodeResult ItchDecoder::decode_next(
    const std::span<const std::uint8_t> input) noexcept {
  if (input.size() < kFramePrefixBytes) return need_more_data();

  const std::size_t message_length = read_be<std::uint16_t>(input, 0);
  if (message_length == 0) {
    return error(DecodeErrorCode::InvalidFrameLength, 0,
                 "ITCH frame length must include a message type byte");
  }

  const std::size_t total_length = kFramePrefixBytes + message_length;
  if (input.size() < total_length) return need_more_data();

  const auto message = input.subspan(kFramePrefixBytes, message_length);
  return decode_message(message, total_length);
}

}  // namespace lob::itch
