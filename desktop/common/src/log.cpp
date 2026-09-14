// -----------------------------------------------------------------------------
// log.cpp -- see log.h.
// -----------------------------------------------------------------------------
#include "usn/common/log.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <utility>

namespace usn::log {

std::string_view nameOf(Level const level) noexcept {
    switch (level) {
    case Level::Trace:    return "TRACE";
    case Level::Debug:    return "DEBUG";
    case Level::Info:     return "INFO";
    case Level::Warning:  return "WARN";
    case Level::Error:    return "ERROR";
    case Level::Critical: return "CRIT";
    case Level::Off:      return "OFF";
    }
    return "?";
}

namespace {

std::uint64_t monotonicNs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::int64_t wallClockNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

// --- RingBufferSink ----------------------------------------------------------

RingBufferSink::RingBufferSink(std::size_t const capacity)
    : m_capacity(capacity == 0U ? 1U : capacity) {}

void RingBufferSink::write(const Record& record) {
    if (m_records.size() >= m_capacity) {
        m_records.pop_front();
        ++m_evicted;  // counted, never silent
    }
    m_records.push_back(record);
}

std::vector<Record> RingBufferSink::snapshot() const {
    return {m_records.begin(), m_records.end()};
}

std::size_t RingBufferSink::size() const { return m_records.size(); }

void RingBufferSink::clear() {
    m_records.clear();
    m_evicted = 0;
}

// --- FileSink ----------------------------------------------------------------

struct FileSink::Impl {
    std::ofstream stream;
    std::string path;
};

FileSink::FileSink(std::string path) : m_impl(std::make_unique<Impl>()) {
    m_impl->path = std::move(path);
    m_impl->stream.open(m_impl->path, std::ios::out | std::ios::app | std::ios::binary);
}

FileSink::~FileSink() = default;

bool FileSink::isOpen() const noexcept {
    return m_impl != nullptr && m_impl->stream.is_open();
}

void FileSink::write(const Record& record) {
    if (!isOpen()) {
        return;
    }
    m_impl->stream << record.monotonicNs << " " << nameOf(record.level) << " ["
                   << record.category << "] " << record.message << '\n';
}

// --- ConsoleSink -------------------------------------------------------------

void ConsoleSink::write(const Record& record) {
    std::fprintf(stdout, "%-5s [%-7s] %s\n", std::string(nameOf(record.level)).c_str(),
                 record.category, record.message.c_str());
    std::fflush(stdout);
}

// --- Logger ------------------------------------------------------------------

Logger::Logger() noexcept {
    for (auto& level : m_levels) {
        level.store(Level::Info, std::memory_order_relaxed);
    }
}

Logger::~Logger() = default;

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::setLevel(Category const category, Level const level) noexcept {
    if (category.id() < kMaxCategories) {
        m_levels[category.id()].store(level, std::memory_order_relaxed);
    }
}

void Logger::setLevelForAll(Level const level) noexcept {
    for (auto& entry : m_levels) {
        entry.store(level, std::memory_order_relaxed);
    }
}

Level Logger::level(Category const category) const noexcept {
    if (category.id() < kMaxCategories) {
        return m_levels[category.id()].load(std::memory_order_relaxed);
    }
    return Level::Off;
}

void Logger::addSink(std::shared_ptr<ISink> sink) {
    if (sink == nullptr) {
        return;
    }
    std::scoped_lock const lock(m_mutex);
    m_sinks.push_back(std::move(sink));
}

void Logger::removeAllSinks() {
    std::scoped_lock const lock(m_mutex);
    m_sinks.clear();
}

std::size_t Logger::sinkCount() const {
    std::scoped_lock const lock(m_mutex);
    return m_sinks.size();
}

void Logger::log(Level const level, Category const category, std::string_view const message) {
    Record record;
    record.level = level;
    record.category = category.name();
    record.message = std::string(message);
    record.wallClockNs = wallClockNs();
    record.monotonicNs = monotonicNs();
    record.thread = std::this_thread::get_id();

    m_recordCount.fetch_add(1, std::memory_order_relaxed);

    std::scoped_lock const lock(m_mutex);
    for (const auto& sink : m_sinks) {
        sink->write(record);
    }
}

void Logger::resetForTesting() {
    removeAllSinks();
    setLevelForAll(Level::Info);
    m_recordCount.store(0, std::memory_order_relaxed);
}

}  // namespace usn::log
