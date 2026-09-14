// -----------------------------------------------------------------------------
// log.h -- structured logging (docs/architecture_review.md section 4.3,
// master spec section 44).
//
// Qt-free by construction: this lives in usn::common so the whole engine can log
// without a QCoreApplication. usn::app adds a QtSink that forwards to qCInfo and
// friends; that is the only Qt-aware sink and it lives above the seam.
//
// The level check happens BEFORE formatting, so a disabled log costs one atomic
// load and no allocation.
// -----------------------------------------------------------------------------
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fmt/core.h>

namespace usn::log {

enum class Level : std::uint8_t { Trace = 0, Debug, Info, Warning, Error, Critical, Off };

[[nodiscard]] std::string_view nameOf(Level level) noexcept;

// A compile-time category. The id indexes the runtime level array, so filtering
// is O(1) and allocation-free.
class Category {
public:
    constexpr Category(std::uint8_t id, const char* name) noexcept : m_id(id), m_name(name) {}
    [[nodiscard]] constexpr std::uint8_t id() const noexcept { return m_id; }
    [[nodiscard]] constexpr const char* name() const noexcept { return m_name; }

private:
    std::uint8_t m_id;
    const char* m_name;
};

inline constexpr std::size_t kMaxCategories = 32;

namespace cats {
inline constexpr Category kApp{0, "app"};
inline constexpr Category kDevice{1, "device"};
inline constexpr Category kUsb{2, "usb"};
inline constexpr Category kCapture{3, "capture"};
inline constexpr Category kDecoder{4, "decoder"};
inline constexpr Category kStorage{5, "storage"};
inline constexpr Category kPlugin{6, "plugin"};
inline constexpr Category kScript{7, "script"};
inline constexpr Category kPerf{8, "perf"};
inline constexpr Category kTrigger{9, "trigger"};
inline constexpr Category kGui{10, "gui"};
inline constexpr Category kTest{11, "test"};
}  // namespace cats

struct Record {
    Level level{Level::Info};
    const char* category{"app"};
    std::string message;
    std::int64_t wallClockNs{0};    // human reference only; never used for ordering
    std::uint64_t monotonicNs{0};   // authoritative ordering / latency measurement
    std::thread::id thread{};
};

class ISink {
public:
    ISink() = default;
    ISink(const ISink&) = delete;
    ISink& operator=(const ISink&) = delete;
    ISink(ISink&&) = delete;
    ISink& operator=(ISink&&) = delete;
    virtual ~ISink() = default;

    // Called with the logger lock held; implementations must not call back into
    // Logger (that would deadlock) and must not block for long.
    virtual void write(const Record& record) = 0;
};

// Bounded in-memory sink feeding the Diagnostics panel.
//
// When full it evicts the OLDEST record but increments an eviction counter and
// exposes it, so the UI can say "N earlier records evicted". Master spec
// section 14: never silently discard.
class RingBufferSink final : public ISink {
public:
    explicit RingBufferSink(std::size_t capacity = 4096);
    void write(const Record& record) final;

    [[nodiscard]] std::vector<Record> snapshot() const;
    [[nodiscard]] std::uint64_t evictedCount() const noexcept { return m_evicted; }
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }
    void clear();

private:
    std::size_t m_capacity;
    std::deque<Record> m_records;
    std::uint64_t m_evicted{0};
};

// Append-only text sink. One line per record, stable format.
class FileSink final : public ISink {
public:
    explicit FileSink(std::string path);
    ~FileSink() final;
    void write(const Record& record) final;
    [[nodiscard]] bool isOpen() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Writes to stdout; used by CLI tools and as the default in tests.
class ConsoleSink final : public ISink {
public:
    void write(const Record& record) final;
};

class Logger {
public:
    // The one justified process-wide singleton: logging must be reachable from
    // free functions and from threads that own no application state. It is
    // dependency-injection-hostile by nature, so every OTHER collaborator in the
    // system takes its dependencies explicitly.
    static Logger& instance();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void setLevel(Category category, Level level) noexcept;
    void setLevelForAll(Level level) noexcept;
    [[nodiscard]] Level level(Category category) const noexcept;

    void addSink(std::shared_ptr<ISink> sink);
    void removeAllSinks();
    [[nodiscard]] std::size_t sinkCount() const;

    // Already-formatted entry point. The USN_LOG macro formats first so disabled
    // logs never pay for it.
    void log(Level level, Category category, std::string_view message);

    [[nodiscard]] std::uint64_t recordCount() const noexcept {
        return m_recordCount.load(std::memory_order_relaxed);
    }

    // Test support: drop everything so suites do not leak into each other.
    void resetForTesting();

private:
    Logger() noexcept;
    ~Logger();

    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<ISink>> m_sinks;
    std::array<std::atomic<Level>, kMaxCategories> m_levels{};
    std::atomic<std::uint64_t> m_recordCount{0};
};

}  // namespace usn::log

// The single macro-heavy construct in the codebase. It exists because a function
// call cannot lazily skip the formatting work.
//
// The parameter names are deliberately ugly (usn_lvl_, usn_cat_, usn_log_). An
// earlier version used `level` and `category`, and because macro parameters are
// substituted everywhere in the replacement list, `usn_log_.level(usn_cat_)`
// expanded to `usn_log_.::usn::log::Level::Debug(usn_cat_)` -- the member
// function named level was replaced by the argument named level. Underscore-
// suffixed names cannot collide with any member of Logger or with a caller's
// local variable named level/category.
#define USN_LOG_AT(usn_lvl_, usn_cat_, ...)                                       \
    do {                                                                          \
        ::usn::log::Logger& usn_log_ = ::usn::log::Logger::instance();            \
        if (usn_log_.level(usn_cat_) <= (usn_lvl_)) {                             \
            usn_log_.log((usn_lvl_), (usn_cat_), ::fmt::format(__VA_ARGS__));     \
        }                                                                         \
    } while (false)

#define USN_LOG_TRACE(cat, ...) USN_LOG_AT(::usn::log::Level::Trace, cat, __VA_ARGS__)
#define USN_LOG_DEBUG(cat, ...) USN_LOG_AT(::usn::log::Level::Debug, cat, __VA_ARGS__)
#define USN_LOG_INFO(cat, ...)  USN_LOG_AT(::usn::log::Level::Info, cat, __VA_ARGS__)
#define USN_LOG_WARN(cat, ...)  USN_LOG_AT(::usn::log::Level::Warning, cat, __VA_ARGS__)
#define USN_LOG_ERROR(cat, ...) USN_LOG_AT(::usn::log::Level::Error, cat, __VA_ARGS__)
#define USN_LOG_FATAL(cat, ...) USN_LOG_AT(::usn::log::Level::Critical, cat, __VA_ARGS__)
