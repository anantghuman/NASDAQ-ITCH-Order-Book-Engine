#include "lob/itch.hpp"
#include "lob/latency.hpp"
#include "lob/matching_engine.hpp"
#include "lob/spsc_ring.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef LOB_HAVE_ZLIB
#include <zlib.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct QueuedEvent {
  lob::itch::ItchEvent event{};
  Clock::time_point enqueued_at{};
};

struct OperationLatency {
  lob::metrics::LatencyHistogram add;
  lob::metrics::LatencyHistogram cancel;
  lob::metrics::LatencyHistogram execution;
  lob::metrics::LatencyHistogram replace;
};

struct ReplayConfig {
  std::string input_path;
  std::size_t ring_capacity{1U << 16U};
  lob::EngineLimits engine_limits{};
  std::uint64_t checkpoint_every{};
  std::optional<lob::StockLocate> snapshot_locate{};
  bool fail_on_reject{false};
};

[[nodiscard]] bool has_gzip_suffix(const std::string& path) {
  return path.size() >= 3 && path.compare(path.size() - 3, 3, ".gz") == 0;
}

enum class FrameReadStatus : std::uint8_t {
  Frame,
  EndOfFile,
  Error,
};

// Reads one native ITCH frame at a time. This keeps replay memory bounded for
// full trading-day samples; the queue, not the input file size, determines the
// live memory footprint of the pipeline.
class ItchFrameReader {
 public:
  ItchFrameReader(const std::string& path, std::string& error) : gzip_(has_gzip_suffix(path)) {
    if (gzip_) {
#ifdef LOB_HAVE_ZLIB
      gzip_stream_ = gzopen(path.c_str(), "rb");
      if (gzip_stream_ == nullptr) {
        error = "cannot open gzip input file";
      }
#else
      error = "gzip support was not enabled; decompress the ITCH sample before replaying it";
#endif
      return;
    }
    plain_stream_.open(path, std::ios::binary);
    if (!plain_stream_) {
      error = "cannot open input file";
    }
  }

  ItchFrameReader(const ItchFrameReader&) = delete;
  ItchFrameReader& operator=(const ItchFrameReader&) = delete;

  ~ItchFrameReader() {
#ifdef LOB_HAVE_ZLIB
    if (gzip_stream_ != nullptr) {
      gzclose(gzip_stream_);
    }
#endif
  }

  [[nodiscard]] bool is_open() const noexcept {
#ifdef LOB_HAVE_ZLIB
    return gzip_ ? gzip_stream_ != nullptr : plain_stream_.is_open();
#else
    return !gzip_ && plain_stream_.is_open();
#endif
  }

  [[nodiscard]] FrameReadStatus next_frame(std::vector<std::uint8_t>& frame,
                                            std::string& error) {
    std::array<std::uint8_t, 2> prefix{};
    int read = read_at_most(prefix.data(), prefix.size(), error);
    if (read < 0) {
      return FrameReadStatus::Error;
    }
    if (read == 0) {
      return FrameReadStatus::EndOfFile;
    }
    std::size_t prefix_bytes = static_cast<std::size_t>(read);
    while (prefix_bytes < prefix.size()) {
      read = read_at_most(prefix.data() + prefix_bytes, prefix.size() - prefix_bytes, error);
      if (read <= 0) {
        if (read == 0) {
          error = "truncated ITCH frame-length prefix";
        }
        return FrameReadStatus::Error;
      }
      prefix_bytes += static_cast<std::size_t>(read);
    }

    const std::size_t payload_size =
        (static_cast<std::size_t>(prefix[0]) << 8U) | static_cast<std::size_t>(prefix[1]);
    frame.resize(2U + payload_size);
    frame[0] = prefix[0];
    frame[1] = prefix[1];

    std::size_t payload_bytes = 0;
    while (payload_bytes < payload_size) {
      read = read_at_most(frame.data() + 2U + payload_bytes, payload_size - payload_bytes, error);
      if (read <= 0) {
        if (read == 0) {
          error = "truncated ITCH frame payload";
        }
        return FrameReadStatus::Error;
      }
      payload_bytes += static_cast<std::size_t>(read);
    }
    return FrameReadStatus::Frame;
  }

 private:
  // Positive values are bytes read, zero is EOF, and -1 represents a read
  // failure whose descriptive message is placed in error.
  [[nodiscard]] int read_at_most(std::uint8_t* output, const std::size_t capacity,
                                 std::string& error) {
    if (gzip_) {
#ifdef LOB_HAVE_ZLIB
      const int read = gzread(gzip_stream_, output, static_cast<unsigned int>(capacity));
      if (read < 0) {
        int zlib_error = Z_OK;
        const char* const reason = gzerror(gzip_stream_, &zlib_error);
        error = reason == nullptr ? "gzip read failed" : reason;
      }
      return read;
#else
      error = "gzip support was not enabled";
      return -1;
#endif
    }

    plain_stream_.read(reinterpret_cast<char*>(output), static_cast<std::streamsize>(capacity));
    const auto read = plain_stream_.gcount();
    if (read > 0) {
      return static_cast<int>(read);
    }
    if (plain_stream_.eof()) {
      return 0;
    }
    error = "input read failed";
    return -1;
  }

  bool gzip_;
  std::ifstream plain_stream_;
#ifdef LOB_HAVE_ZLIB
  gzFile gzip_stream_{nullptr};
#endif
};

void print_summary(const char* name, const lob::metrics::LatencyHistogram& histogram) {
  const auto summary = histogram.summary();
  std::cout << name << ": count=" << summary.count << " p50=" << summary.p50_ns
            << "ns p99=" << summary.p99_ns << "ns p99.9=" << summary.p999_ns
            << "ns max=" << summary.max_ns << "ns\n";
}

void record_latency(OperationLatency& latency, const lob::ApplyAction action,
                    const std::uint64_t duration_ns) {
  switch (action) {
    case lob::ApplyAction::Added:
      latency.add.record(duration_ns);
      break;
    case lob::ApplyAction::Cancelled:
      latency.cancel.record(duration_ns);
      break;
    case lob::ApplyAction::Executed:
      latency.execution.record(duration_ns);
      break;
    case lob::ApplyAction::Replaced:
      latency.replace.record(duration_ns);
      break;
    default:
      break;
  }
}

[[nodiscard]] std::size_t parse_capacity(const char* input) {
  try {
    const auto value = std::stoull(input);
    if (value == 0 || (value & (value - 1)) != 0) {
      throw std::invalid_argument("not a power of two");
    }
    return static_cast<std::size_t>(value);
  } catch (const std::exception&) {
    return 0;
  }
}

[[nodiscard]] std::optional<std::size_t> parse_positive_size(const char* input) {
  try {
    const auto value = std::stoull(input);
    if (value == 0 || value > std::numeric_limits<std::size_t>::max()) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(value);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

[[nodiscard]] bool parse_arguments(const int argc, char** argv, ReplayConfig& config) {
  if (argc < 2) {
    return false;
  }
  config.input_path = argv[1];
  for (int index = 2; index < argc; ++index) {
    const std::string_view option = argv[index];
    if (option == "--fail-on-reject") {
      config.fail_on_reject = true;
      continue;
    }
    if (index + 1 >= argc) {
      return false;
    }
    const char* const value = argv[++index];
    if (option == "--ring-capacity") {
      const std::size_t capacity = parse_capacity(value);
      if (capacity == 0) {
        return false;
      }
      config.ring_capacity = capacity;
    } else if (option == "--max-instruments") {
      const auto limit = parse_positive_size(value);
      if (!limit.has_value()) {
        return false;
      }
      config.engine_limits.max_instruments = *limit;
    } else if (option == "--max-resting-orders") {
      const auto limit = parse_positive_size(value);
      if (!limit.has_value()) {
        return false;
      }
      config.engine_limits.max_resting_orders = *limit;
    } else if (option == "--checkpoint-every") {
      const auto interval = parse_positive_size(value);
      if (!interval.has_value()) {
        return false;
      }
      config.checkpoint_every = static_cast<std::uint64_t>(*interval);
    } else if (option == "--snapshot-locate") {
      const auto locate = parse_positive_size(value);
      if (!locate.has_value() || *locate > std::numeric_limits<lob::StockLocate>::max()) {
        return false;
      }
      config.snapshot_locate = static_cast<lob::StockLocate>(*locate);
    } else {
      return false;
    }
  }
  return true;
}

}  // namespace

int main(const int argc, char** argv) {
  ReplayConfig config;
  if (!parse_arguments(argc, argv, config)) {
    std::cerr << "usage: itch_replay <sample.itch[.gz]> [--ring-capacity N] "
                 "[--max-instruments N] [--max-resting-orders N] "
                 "[--checkpoint-every N] [--snapshot-locate N] [--fail-on-reject]\n";
    return 2;
  }

  std::string input_error;
  ItchFrameReader input(config.input_path, input_error);
  if (!input.is_open()) {
    std::cerr << "input error: " << input_error << '\n';
    return 1;
  }

  lob::SpscRing<QueuedEvent> queue(config.ring_capacity);
  std::atomic<bool> producer_done{false};
  std::atomic<bool> producer_failed{false};
  std::string decode_error;
  std::uint64_t decoded_messages = 0;

  const auto replay_start = Clock::now();
  std::thread producer([&] {
    std::vector<std::uint8_t> frame;
    while (true) {
      const auto frame_status = input.next_frame(frame, decode_error);
      if (frame_status == FrameReadStatus::EndOfFile) {
        break;
      }
      if (frame_status == FrameReadStatus::Error) {
        producer_failed.store(true, std::memory_order_release);
        break;
      }
      const auto decoded = lob::itch::ItchDecoder::decode_next(frame);
      if (decoded.status != lob::itch::DecodeStatus::Complete || !decoded.message.has_value()) {
        decode_error = decoded.error.has_value() ? decoded.error->reason
                                                 : "truncated ITCH frame at end of file";
        producer_failed.store(true, std::memory_order_release);
        break;
      }
      QueuedEvent queued{std::move(decoded.message->event), Clock::now()};
      while (!queue.try_push(std::move(queued))) {
        std::this_thread::yield();
      }
      ++decoded_messages;
    }
    producer_done.store(true, std::memory_order_release);
  });

  lob::MatchingEngine engine(config.engine_limits);
  OperationLatency latency;
  lob::metrics::LatencyHistogram end_to_end;
  QueuedEvent queued;
  std::uint64_t applied_messages = 0;
  while (!producer_done.load(std::memory_order_acquire) || !queue.empty()) {
    if (!queue.try_pop(queued)) {
      std::this_thread::yield();
      continue;
    }
    const auto operation_start = Clock::now();
    const auto result = engine.apply(queued.event);
    const auto operation_end = Clock::now();
    const auto operation_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(operation_end - operation_start).count());
    const auto end_to_end_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(operation_end - queued.enqueued_at).count());
    record_latency(latency, result.action, operation_ns);
    end_to_end.record(end_to_end_ns);
    ++applied_messages;
    if (config.checkpoint_every != 0 && (applied_messages % config.checkpoint_every) == 0) {
      const auto& checkpoint_stats = engine.stats();
      std::cerr << "event=checkpoint applied=" << applied_messages
                << " instruments=" << engine.instrument_count()
                << " resting_orders=" << engine.resting_order_count()
                << " rejected=" << checkpoint_stats.rejected << '\n';
    }
  }
  producer.join();

  if (producer_failed.load(std::memory_order_acquire)) {
    std::cerr << "decode error: " << decode_error << '\n';
    return 1;
  }

  const auto elapsed = Clock::now() - replay_start;
  const double seconds = std::chrono::duration<double>(elapsed).count();
  const auto& stats = engine.stats();
  std::cout << "decoded=" << decoded_messages << " applied=" << stats.messages
            << " instruments=" << engine.instrument_count()
            << " resting_orders=" << engine.resting_order_count() << " rejected=" << stats.rejected
            << " throughput=" << static_cast<std::uint64_t>(decoded_messages / seconds)
            << " messages/s\n";
  print_summary("add", latency.add);
  print_summary("cancel", latency.cancel);
  print_summary("execution/match", latency.execution);
  print_summary("replace", latency.replace);
  print_summary("enqueue-to-apply", end_to_end);
  if (config.snapshot_locate.has_value()) {
    const auto* book = engine.find_book(*config.snapshot_locate);
    if (book == nullptr) {
      std::cerr << "event=snapshot_missing stock_locate=" << *config.snapshot_locate << '\n';
    } else {
      const auto bid = book->best_bid_depth();
      const auto ask = book->best_ask_depth();
      std::cout << "snapshot.stock_locate=" << *config.snapshot_locate
                << " bid_price=" << (bid.has_value() ? bid->price : 0U)
                << " bid_quantity=" << (bid.has_value() ? bid->quantity : 0U)
                << " ask_price=" << (ask.has_value() ? ask->price : 0U)
                << " ask_quantity=" << (ask.has_value() ? ask->quantity : 0U) << '\n';
    }
  }
  return config.fail_on_reject && stats.rejected != 0 ? 1 : 0;
}
