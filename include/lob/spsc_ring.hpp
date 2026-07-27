#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace lob {

// A bounded, single-producer/single-consumer queue.
//
// One thread must exclusively call try_push()/emplace(), and one other thread
// must exclusively call try_pop().  full() is intended for the producer and
// empty() for the consumer.  The queue allocates its storage only in the
// constructor; no hot-path operation allocates or takes a lock.
//
// T must be safely movable out of an occupied slot.  This is deliberately a
// no-throw requirement: if popping an item could throw after moving from it,
// the consumer could not safely retry the operation.
template <typename T>
class SpscRing {
  static_assert(std::atomic<std::size_t>::is_always_lock_free,
                "SpscRing requires lock-free size_t atomics on this target");
  static_assert(std::is_nothrow_destructible_v<T>,
                "SpscRing requires a non-throwing destructor");
  static_assert(std::is_nothrow_move_constructible_v<T>,
                "SpscRing requires a non-throwing move constructor");
  static_assert(std::is_nothrow_move_assignable_v<T>,
                "SpscRing requires a non-throwing move assignment operator");

 public:
  explicit SpscRing(const std::size_t capacity)
      : capacity_(capacity), mask_(capacity - 1), slots_(allocate_slots(capacity)) {}

  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;
  SpscRing(SpscRing&&) = delete;
  SpscRing& operator=(SpscRing&&) = delete;

  ~SpscRing() { clear(); }

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  template <typename... Args>
  [[nodiscard]] bool emplace(Args&&... args) {
    const std::size_t write = write_index_.value.load(std::memory_order_relaxed);
    const std::size_t read = read_index_.value.load(std::memory_order_acquire);
    if (write - read == capacity_) {
      return false;
    }

    ::new (static_cast<void*>(slot_at(write).storage)) T(std::forward<Args>(args)...);
    // Publishing the write index after construction makes the object visible
    // to the consumer without a mutex.
    write_index_.value.store(write + 1, std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool try_push(const T& value) { return emplace(value); }
  [[nodiscard]] bool try_push(T&& value) { return emplace(std::move(value)); }

  [[nodiscard]] bool try_pop(T& out) noexcept {
    const std::size_t read = read_index_.value.load(std::memory_order_relaxed);
    const std::size_t write = write_index_.value.load(std::memory_order_acquire);
    if (read == write) {
      return false;
    }

    T* const object = slot_at(read).object();
    out = std::move(*object);
    object->~T();
    // Releasing the read index makes this slot available for reuse by the
    // producer only after destruction has completed.
    read_index_.value.store(read + 1, std::memory_order_release);
    return true;
  }

  // This is a producer-side query.  It may become stale immediately when the
  // consumer advances, which is normal for a non-blocking queue.
  [[nodiscard]] bool full() const noexcept {
    const std::size_t write = write_index_.value.load(std::memory_order_relaxed);
    const std::size_t read = read_index_.value.load(std::memory_order_acquire);
    return write - read == capacity_;
  }

  // This is a consumer-side query.  It may become stale immediately when the
  // producer publishes an item, which is normal for a non-blocking queue.
  [[nodiscard]] bool empty() const noexcept {
    const std::size_t read = read_index_.value.load(std::memory_order_relaxed);
    const std::size_t write = write_index_.value.load(std::memory_order_acquire);
    return read == write;
  }

  // Returns a bounded snapshot.  Concurrent producer/consumer progress means
  // the result is advisory rather than a linearizable point-in-time count.
  [[nodiscard]] std::size_t size() const noexcept {
    [[maybe_unused]] const std::size_t write_before =
        write_index_.value.load(std::memory_order_acquire);
    const std::size_t read = read_index_.value.load(std::memory_order_acquire);
    // A second producer-index read cannot move backwards in its modification
    // order, including when the monotonically increasing counter wraps.
    const std::size_t write = write_index_.value.load(std::memory_order_acquire);
    const std::size_t used = write - read;
    return used <= capacity_ ? used : capacity_;
  }

 private:
  static constexpr std::size_t kCacheLineBytes = 64;

  struct Slot {
    alignas(T) std::byte storage[sizeof(T)];

    [[nodiscard]] T* object() noexcept {
      return std::launder(reinterpret_cast<T*>(storage));
    }
  };

  struct alignas(kCacheLineBytes) Index {
    std::atomic<std::size_t> value{0};
  };

  static std::unique_ptr<Slot[]> allocate_slots(const std::size_t capacity) {
    if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
      throw std::invalid_argument("SpscRing capacity must be a non-zero power of two");
    }
    return std::make_unique<Slot[]>(capacity);
  }

  [[nodiscard]] Slot& slot_at(const std::size_t index) noexcept {
    return slots_[index & mask_];
  }

  [[nodiscard]] const Slot& slot_at(const std::size_t index) const noexcept {
    return slots_[index & mask_];
  }

  void clear() noexcept {
    std::size_t read = read_index_.value.load(std::memory_order_relaxed);
    const std::size_t write = write_index_.value.load(std::memory_order_relaxed);
    while (read != write) {
      slot_at(read).object()->~T();
      ++read;
    }
  }

  const std::size_t capacity_;
  const std::size_t mask_;
  std::unique_ptr<Slot[]> slots_;

  // The producer writes only write_index_ and the consumer writes only
  // read_index_.  Their cache-line isolation avoids false sharing.
  Index write_index_;
  Index read_index_;
};

}  // namespace lob
