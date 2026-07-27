#include "lob/itch.hpp"

#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <random>
#include <span>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

void append_u16(std::vector<std::uint8_t>& bytes, const std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
  bytes.push_back(static_cast<std::uint8_t>(value));
}

void append_u32(std::vector<std::uint8_t>& bytes, const std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    bytes.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void append_u48(std::vector<std::uint8_t>& bytes, const std::uint64_t value) {
  for (int shift = 40; shift >= 0; shift -= 8) {
    bytes.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void append_u64(std::vector<std::uint8_t>& bytes, const std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    bytes.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void append_chars(std::vector<std::uint8_t>& bytes,
                  const std::initializer_list<char> characters) {
  for (const char character : characters) {
    bytes.push_back(static_cast<std::uint8_t>(character));
  }
}

void append_header(std::vector<std::uint8_t>& message, const char type) {
  message.push_back(static_cast<std::uint8_t>(type));
  append_u16(message, 0x1234U);
  append_u16(message, 0x5678U);
  append_u48(message, 0x010203040506ULL);
}

std::vector<std::uint8_t> frame(std::vector<std::uint8_t> message) {
  std::vector<std::uint8_t> output;
  append_u16(output, static_cast<std::uint16_t>(message.size()));
  output.insert(output.end(), message.begin(), message.end());
  return output;
}

lob::itch::DecodedMessage decode_complete(
    const std::vector<std::uint8_t>& bytes) {
  const auto result = lob::itch::ItchDecoder::decode_next(bytes);
  assert(result.status == lob::itch::DecodeStatus::Complete);
  assert(result.message.has_value());
  assert(!result.error.has_value());
  assert(result.message->bytes_consumed == bytes.size());
  return std::move(*result.message);
}

template <typename Event>
Event decode_as(const std::vector<std::uint8_t>& bytes) {
  const auto decoded = decode_complete(bytes);
  const auto* event = std::get_if<Event>(&decoded.event);
  assert(event != nullptr);
  return *event;
}

void test_add_order_golden_bytes() {
  std::vector<std::uint8_t> message;
  append_header(message, 'A');
  append_u64(message, 0x0102030405060708ULL);
  message.push_back('B');
  append_u32(message, 500U);
  append_chars(message, {'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '});
  append_u32(message, 1'234'500U);

  const auto bytes = frame(std::move(message));
  const auto decoded = decode_complete(bytes);
  const auto* event = std::get_if<lob::itch::AddOrder>(&decoded.event);
  assert(event != nullptr);
  assert(event->header.stock_locate == 0x1234U);
  assert(event->header.tracking_number == 0x5678U);
  assert(event->header.timestamp == 0x010203040506ULL);
  assert(event->order_reference == 0x0102030405060708ULL);
  assert(event->side == lob::Side::Buy);
  assert(event->shares == 500U);
  assert(event->stock[0] == 'A' && event->stock[3] == 'L' && event->stock[7] == ' ');
  assert(event->price == 1'234'500U);
}

void test_order_lifecycle_messages() {
  {
    std::vector<std::uint8_t> message;
    append_header(message, 'F');
    append_u64(message, 44U);
    message.push_back('S');
    append_u32(message, 19U);
    append_chars(message, {'M', 'S', 'F', 'T', ' ', ' ', ' ', ' '});
    append_u32(message, 42'000U);
    append_chars(message, {'M', 'M', '0', '1'});
    const auto event =
        decode_as<lob::itch::AddAttributedOrder>(frame(std::move(message)));
    assert(event.side == lob::Side::Sell);
    assert(event.attribution[2] == '0');
  }
  {
    std::vector<std::uint8_t> message;
    append_header(message, 'E');
    append_u64(message, 44U);
    append_u32(message, 7U);
    append_u64(message, 99U);
    const auto event = decode_as<lob::itch::OrderExecuted>(frame(std::move(message)));
    assert(event.executed_shares == 7U && event.match_number == 99U);
  }
  {
    std::vector<std::uint8_t> message;
    append_header(message, 'C');
    append_u64(message, 44U);
    append_u32(message, 8U);
    append_u64(message, 100U);
    message.push_back('Y');
    append_u32(message, 98'765U);
    const auto event =
        decode_as<lob::itch::OrderExecutedWithPrice>(frame(std::move(message)));
    assert(event.printable == 'Y' && event.execution_price == 98'765U);
  }
  {
    std::vector<std::uint8_t> message;
    append_header(message, 'X');
    append_u64(message, 44U);
    append_u32(message, 9U);
    const auto event = decode_as<lob::itch::OrderCancel>(frame(std::move(message)));
    assert(event.canceled_shares == 9U);
  }
  {
    std::vector<std::uint8_t> message;
    append_header(message, 'D');
    append_u64(message, 44U);
    const auto event = decode_as<lob::itch::OrderDelete>(frame(std::move(message)));
    assert(event.order_reference == 44U);
  }
  {
    std::vector<std::uint8_t> message;
    append_header(message, 'U');
    append_u64(message, 44U);
    append_u64(message, 45U);
    append_u32(message, 10U);
    append_u32(message, 99'999U);
    const auto event = decode_as<lob::itch::OrderReplace>(frame(std::move(message)));
    assert(event.original_order_reference == 44U &&
           event.new_order_reference == 45U && event.price == 99'999U);
  }
}

void test_observable_messages() {
  {
    std::vector<std::uint8_t> message;
    append_header(message, 'S');
    message.push_back('O');
    const auto event = decode_as<lob::itch::SystemEvent>(frame(std::move(message)));
    assert(event.event_code == 'O');
  }
  {
    std::vector<std::uint8_t> message;
    append_header(message, 'H');
    append_chars(message, {'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '});
    append_chars(message, {'T', ' ', 'N', 'E', 'W', 'S'});
    const auto event = decode_as<lob::itch::TradingAction>(frame(std::move(message)));
    assert(event.trading_state == 'T' && event.reason[0] == 'N');
  }
  {
    std::vector<std::uint8_t> message;
    append_header(message, 'P');
    append_u64(message, 222U);
    message.push_back('B');
    append_u32(message, 77U);
    append_chars(message, {'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '});
    append_u32(message, 123'400U);
    append_u64(message, 333U);
    const auto event = decode_as<lob::itch::Trade>(frame(std::move(message)));
    assert(event.order_reference == 222U && event.match_number == 333U);
  }
  {
    std::vector<std::uint8_t> message;
    append_header(message, 'Q');
    append_u64(message, 1'000U);
    append_chars(message, {'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '});
    append_u32(message, 123'500U);
    append_u64(message, 444U);
    message.push_back('O');
    const auto event = decode_as<lob::itch::CrossTrade>(frame(std::move(message)));
    assert(event.shares == 1'000U && event.cross_type == 'O');
  }
  {
    std::vector<std::uint8_t> message;
    append_header(message, 'B');
    append_u64(message, 555U);
    const auto event = decode_as<lob::itch::BrokenTrade>(frame(std::move(message)));
    assert(event.match_number == 555U);
  }
  {
    std::vector<std::uint8_t> message;
    append_header(message, 'I');
    append_u64(message, 10U);
    append_u64(message, 4U);
    message.push_back('B');
    append_chars(message, {'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '});
    append_u32(message, 101U);
    append_u32(message, 102U);
    append_u32(message, 103U);
    append_chars(message, {'O', 'L'});
    const auto event =
        decode_as<lob::itch::NetOrderImbalance>(frame(std::move(message)));
    assert(event.paired_shares == 10U && event.current_reference_price == 103U &&
           event.cross_type == 'O');
  }
}

void test_stock_directory_golden_bytes() {
  std::vector<std::uint8_t> message;
  append_header(message, 'R');
  append_chars(message, {'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '});
  append_chars(message, {'Q', 'N'});
  append_u32(message, 100U);
  append_chars(message, {'N', 'A', ' ', ' '});
  append_chars(message, {'P', ' ', 'N', '1', 'Y'});
  append_u32(message, 2U);
  message.push_back('N');
  const auto event = decode_as<lob::itch::StockDirectory>(frame(std::move(message)));
  assert(event.round_lot_size == 100U);
  assert(event.issue_sub_type[0] == ' ' && event.issue_sub_type[1] == ' ');
  assert(event.etp_leverage_factor == 2U && event.inverse_indicator == 'N');
}

void test_malformed_and_streaming_boundaries() {
  assert(lob::itch::ItchDecoder::decode_next({}).status ==
         lob::itch::DecodeStatus::NeedMoreData);
  const std::vector<std::uint8_t> one_byte{0};
  assert(lob::itch::ItchDecoder::decode_next(one_byte).status ==
         lob::itch::DecodeStatus::NeedMoreData);

  const std::vector<std::uint8_t> zero_length{0, 0};
  const auto zero = lob::itch::ItchDecoder::decode_next(zero_length);
  assert(zero.status == lob::itch::DecodeStatus::Error);
  assert(zero.error->code == lob::itch::DecodeErrorCode::InvalidFrameLength);

  const std::vector<std::uint8_t> incomplete{0, 12, 'S', 0, 0};
  assert(lob::itch::ItchDecoder::decode_next(incomplete).status ==
         lob::itch::DecodeStatus::NeedMoreData);

  // Unknown but syntactically complete ITCH frames must not interrupt a
  // historical replay. The declared frame length remains authoritative.
  const std::vector<std::uint8_t> skipped_frame{0, 11, 'Z', 0x12, 0x34, 0x56, 0x78,
                                                 1, 2, 3, 4, 5, 6};
  const auto skipped = decode_complete(skipped_frame);
  const auto* skipped_event = std::get_if<lob::itch::SkippedMessage>(&skipped.event);
  assert(skipped_event != nullptr);
  assert(skipped_event->message_type == 'Z');
  assert(skipped_event->header.has_value());
  assert(skipped_event->header->stock_locate == 0x1234U);
  assert(skipped_event->header->tracking_number == 0x5678U);
  assert(skipped_event->header->timestamp == 0x010203040506ULL);

  // Some control records do not carry the normal locator/tracking/timestamp
  // header. They are not book mutations and must not stop a full-day replay.
  const auto short_skipped = decode_complete({0, 5, 'T', 1, 2, 3, 4});
  const auto* short_event = std::get_if<lob::itch::SkippedMessage>(&short_skipped.event);
  assert(short_event != nullptr && short_event->message_type == 'T');
  assert(!short_event->header.has_value());

  std::vector<std::uint8_t> bad_side_message;
  append_header(bad_side_message, 'A');
  append_u64(bad_side_message, 1U);
  bad_side_message.push_back('X');
  append_u32(bad_side_message, 1U);
  append_chars(bad_side_message, {'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '});
  append_u32(bad_side_message, 1U);
  const auto bad_side = lob::itch::ItchDecoder::decode_next(frame(std::move(bad_side_message)));
  assert(bad_side.status == lob::itch::DecodeStatus::Error);
  assert(bad_side.error->code == lob::itch::DecodeErrorCode::InvalidSide);

  const std::vector<std::uint8_t> wrong_length{0, 11, 'S', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  const auto length_error = lob::itch::ItchDecoder::decode_next(wrong_length);
  assert(length_error.status == lob::itch::DecodeStatus::Error);
  assert(length_error.error->code == lob::itch::DecodeErrorCode::UnexpectedMessageLength);
}

void test_random_binary_inputs_are_bounded() {
  std::mt19937_64 random(0x59a1'fe20'5eedULL);
  for (std::size_t iteration = 0; iteration < 20'000; ++iteration) {
    std::vector<std::uint8_t> input(static_cast<std::size_t>(random() % 128U));
    for (auto& byte : input) {
      byte = static_cast<std::uint8_t>(random());
    }

    const auto result = lob::itch::ItchDecoder::decode_next(input);
    switch (result.status) {
      case lob::itch::DecodeStatus::Complete:
        assert(result.message.has_value());
        assert(!result.error.has_value());
        assert(result.message->bytes_consumed > 0);
        assert(result.message->bytes_consumed <= input.size());
        break;
      case lob::itch::DecodeStatus::NeedMoreData:
        assert(!result.message.has_value());
        assert(!result.error.has_value());
        break;
      case lob::itch::DecodeStatus::Error:
        assert(!result.message.has_value());
        assert(result.error.has_value());
        break;
    }
  }
}

}  // namespace

int main() {
  test_add_order_golden_bytes();
  test_order_lifecycle_messages();
  test_observable_messages();
  test_stock_directory_golden_bytes();
  test_malformed_and_streaming_boundaries();
  test_random_binary_inputs_are_bounded();
}
