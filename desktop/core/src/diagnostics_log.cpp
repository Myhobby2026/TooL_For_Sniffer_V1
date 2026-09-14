// -----------------------------------------------------------------------------
// diagnostics_log.cpp -- see diagnostics_log.h.
// -----------------------------------------------------------------------------
#include "usn/core/diagnostics_log.h"

#include <utility>

#include "usn/common/log.h"

namespace usn::core {

DiagnosticsLog::DiagnosticsLog(std::size_t const capacity)
    : m_capacity(capacity == 0 ? 1 : capacity) {}

void DiagnosticsLog::record(DiagnosticEvent event) {
    std::vector<Listener> listeners;
    {
        std::scoped_lock const lock(m_mutex);

        // Merge with an existing identical condition instead of appending.
        bool merged = false;
        for (auto& existing : m_events) {
            if (existing.sameConditionAs(event)) {
                existing.mergeFrom(event);
                merged = true;
                break;
            }
        }
        if (!merged) {
            if (m_events.size() >= m_capacity) {
                m_events.pop_front();
                m_summary.evicted += 1;   // counted, never silent
            }
            m_events.push_back(event);
        }

        m_summary.totalRecorded += event.count;
        m_summary.distinctConditions = m_events.size();
        auto const sevIndex = static_cast<std::size_t>(event.severity);
        if (sevIndex < 4) {
            m_summary.bySeverity[sevIndex] += event.count;
        }
        switch (event.code) {
        case ErrorCode::CrcMismatch:            m_summary.crcErrors += event.count; break;
        case ErrorCode::SequenceGap:            m_summary.sequenceGaps += event.count; break;
        case ErrorCode::SampleIndexGap:         m_summary.sampleIndexGaps += event.count; break;
        case ErrorCode::DmaOverflow:
        case ErrorCode::RingBufferOverflow:
        case ErrorCode::HostQueueOverflow:      m_summary.overflows += event.count; break;
        case ErrorCode::FramingResync:          m_summary.resyncEvents += event.count; break;
        case ErrorCode::DeviceResetDetected:    m_summary.deviceResets += event.count; break;
        default: break;
        }
        listeners = m_listeners;
    }
    // Notify outside the lock so a listener can call back into the log.
    for (const auto& listener : listeners) {
        listener(event);
    }
    if (event.severity >= DiagnosticSeverity::Warning) {
        USN_LOG_WARN(log::cats::kCapture, "diagnostic: {}", event.toString());
    }
}

void DiagnosticsLog::record(const Status& status) {
    if (status.ok()) {
        return;
    }
    record(diagnosticFromStatus(status));
}

void DiagnosticsLog::record(ErrorCode const code, std::string message) {
    record(diagnosticFromStatus(Status::error(code, std::move(message))));
}

void DiagnosticsLog::addListener(Listener listener) {
    if (!listener) {
        return;
    }
    std::scoped_lock const lock(m_mutex);
    m_listeners.push_back(std::move(listener));
}

std::vector<DiagnosticEvent> DiagnosticsLog::snapshot() const {
    std::scoped_lock const lock(m_mutex);
    return {m_events.begin(), m_events.end()};
}

std::vector<DiagnosticEvent> DiagnosticsLog::snapshotWithSeverityAtLeast(
    DiagnosticSeverity const severity) const {
    std::scoped_lock const lock(m_mutex);
    std::vector<DiagnosticEvent> out;
    for (const auto& event : m_events) {
        if (event.severity >= severity) {
            out.push_back(event);
        }
    }
    return out;
}

DiagnosticsSummary DiagnosticsLog::summary() const {
    std::scoped_lock const lock(m_mutex);
    auto summary = m_summary;
    summary.distinctConditions = m_events.size();
    return summary;
}

std::size_t DiagnosticsLog::size() const {
    std::scoped_lock const lock(m_mutex);
    return m_events.size();
}

std::uint64_t DiagnosticsLog::evictedCount() const {
    std::scoped_lock const lock(m_mutex);
    return m_summary.evicted;
}

void DiagnosticsLog::clear() {
    std::scoped_lock const lock(m_mutex);
    m_events.clear();
    m_summary = DiagnosticsSummary{};
}

bool DiagnosticsLog::isClean() const {
    std::scoped_lock const lock(m_mutex);
    return m_events.empty() && m_summary.evicted == 0;
}

}  // namespace usn::core
