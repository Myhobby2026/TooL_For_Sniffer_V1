// -----------------------------------------------------------------------------
// spsc_queue.h -- bounded single-producer/single-consumer queue.
//
// Used for the usn.rx -> usn.dispatch hand-off, the hottest path in the capture
// pipeline and the one place where avoiding a lock is worth the complexity
// (docs/architecture_review.md section 6.3, invariant 2).
//
// Lock-free for both sides: each thread writes only its own index and reads the
// other's with acquire/release ordering. Capacity is rounded up to a power of two
// so the wrap is a mask.
//
// NOT multi-producer and NOT multi-consumer. Fan-in paths use the mutex-based
// queue inside CapturePipeline; using this type for MPSC would be a data race.
//
// Note on a trap this implementation deliberately avoids: per-slot "filled"
// flags stored in a std::vector<bool> would be a data race, because vector<bool>
// is bit-packed and two threads writing different indices modify the same byte.
// There are no per-slot flags here; occupancy is derived from head and tail.
// -----------------------------------------------------------------------------
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace usn {

template <class T>
class SpscQueue {
    static_assert(std::is_move_constructible_v<T>, "SpscQueue requires a movable element");

public:
    // `capacity` is the number of USABLE slots. One extra slot is sacrificed to
    // distinguish full from empty, so the allocation is roundUpPow2(capacity + 1).
    explicit SpscQueue(std::size_t capacity) : m_capacity(roundUpPow2(capacity + 1)) {
        m_slots.resize(m_capacity);
    }

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;
    SpscQueue(SpscQueue&&) = delete;
    SpscQueue& operator=(SpscQueue&&) = delete;
    ~SpscQueue() = default;

    // Producer side only. Returns false when full; the caller decides the
    // backpressure policy. The queue itself never drops anything silently.
    [[nodiscard]] bool tryPush(T value) {
        std::size_t const head = m_head.load(std::memory_order_relaxed);
        std::size_t const next = (head + 1) & mask();
        if (next == m_tail.load(std::memory_order_acquire)) {
            m_pushFailures.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        m_slots[head] = std::move(value);
        m_head.store(next, std::memory_order_release);

        std::size_t const used = (next - m_tail.load(std::memory_order_relaxed)) & mask();
        std::size_t prevHigh = m_highWater.load(std::memory_order_relaxed);
        while (used > prevHigh &&
               !m_highWater.compare_exchange_weak(prevHigh, used, std::memory_order_relaxed)) {
        }
        return true;
    }

    // Consumer side only.
    [[nodiscard]] std::optional<T> tryPop() {
        std::size_t const tail = m_tail.load(std::memory_order_relaxed);
        if (tail == m_head.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        T value = std::move(m_slots[tail]);
        m_tail.store((tail + 1) & mask(), std::memory_order_release);
        return value;
    }

    // Consumer side. Drains up to maxCount items, returns how many were taken.
    std::size_t drain(std::vector<T>& out, std::size_t maxCount) {
        std::size_t taken = 0;
        while (taken < maxCount) {
            auto value = tryPop();
            if (!value.has_value()) {
                break;
            }
            out.push_back(std::move(*value));
            ++taken;
        }
        return taken;
    }

    [[nodiscard]] std::size_t sizeApprox() const noexcept {
        std::size_t const head = m_head.load(std::memory_order_acquire);
        std::size_t const tail = m_tail.load(std::memory_order_acquire);
        return (head - tail) & mask();
    }

    [[nodiscard]] bool emptyApprox() const noexcept { return sizeApprox() == 0; }

    [[nodiscard]] std::size_t usableCapacity() const noexcept { return m_capacity - 1; }
    [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }
    [[nodiscard]] std::size_t highWaterMark() const noexcept {
        return m_highWater.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t pushFailures() const noexcept {
        return m_pushFailures.load(std::memory_order_relaxed);
    }

private:
    [[nodiscard]] std::size_t mask() const noexcept { return m_capacity - 1; }

    static std::size_t roundUpPow2(std::size_t value) noexcept {
        std::size_t result = 2;
        while (result < (value < 2 ? 2 : value)) {
            result *= 2;
        }
        return result;
    }

    std::size_t m_capacity;
    std::vector<T> m_slots;
    alignas(64) std::atomic<std::size_t> m_head{0};
    alignas(64) std::atomic<std::size_t> m_tail{0};
    std::atomic<std::size_t> m_highWater{0};
    std::atomic<std::uint64_t> m_pushFailures{0};
};

}  // namespace usn
