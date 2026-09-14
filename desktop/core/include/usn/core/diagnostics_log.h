// -----------------------------------------------------------------------------
// diagnostics_log.h -- the integrity audit trail (master spec section 14).
//
// "Never silently discard data" is a property of the whole system, so it needs one
// place where every anomaly is recorded, aggregated and made visible. This is it.
//
// Aggregation: an overflow at 10 MSPS can occur thousands of times per second, so
// identical conditions are merged into one entry with a count and first/last seen.
// The magnitude is preserved and the panel stays usable. Eviction is counted too --
// the log itself never discards silently.
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

#include "usn/model/diagnostics.h"

namespace usn::core {

struct DiagnosticsSummary {
    std::uint64_t totalRecorded{0};     // including merged duplicates
    std::uint64_t distinctConditions{0};
    std::uint64_t evicted{0};
    std::uint64_t bySeverity[4]{0, 0, 0, 0};
    std::uint64_t crcErrors{0};
    std::uint64_t sequenceGaps{0};
    std::uint64_t sampleIndexGaps{0};
    std::uint64_t overflows{0};
    std::uint64_t resyncEvents{0};
    std::uint64_t deviceResets{0};
};

class DiagnosticsLog {
public:
    explicit DiagnosticsLog(std::size_t capacity = 8192);

    // Thread-safe. Merges with an existing identical condition when possible.
    void record(DiagnosticEvent event);
    void record(const Status& status);
    void record(ErrorCode code, std::string message);

    using Listener = std::function<void(const DiagnosticEvent&)>;
    // Listeners run on the recording thread while the lock is NOT held, so a
    // listener may safely call back into the log.
    void addListener(Listener listener);

    [[nodiscard]] std::vector<DiagnosticEvent> snapshot() const;
    [[nodiscard]] std::vector<DiagnosticEvent> snapshotWithSeverityAtLeast(
        DiagnosticSeverity severity) const;
    [[nodiscard]] DiagnosticsSummary summary() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::uint64_t evictedCount() const;

    void clear();

    // Master spec section 14: a capture with zero recorded anomalies and zero
    // counters is the only condition under which data may be called complete.
    [[nodiscard]] bool isClean() const;

private:
    mutable std::mutex m_mutex;
    std::size_t m_capacity;
    std::deque<DiagnosticEvent> m_events;
    std::vector<Listener> m_listeners;
    DiagnosticsSummary m_summary;
};

}  // namespace usn::core
