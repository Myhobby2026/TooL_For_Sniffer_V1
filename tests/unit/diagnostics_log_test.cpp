// -----------------------------------------------------------------------------
// diagnostics_log_test.cpp -- the audit trail must aggregate without losing the
// magnitude, and must never evict silently.
// -----------------------------------------------------------------------------
#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "usn/core/diagnostics_log.h"
#include "usn/test/test_fixtures.h"

using usn::DiagnosticEvent;
using usn::DiagnosticSeverity;
using usn::ErrorCode;
using usn::SampleIndex;
using usn::Status;
using usn::core::DiagnosticsLog;

namespace {

DiagnosticEvent overflowEvent(std::uint64_t const atSample) {
    DiagnosticEvent e;
    e.severity = DiagnosticSeverity::Error;
    e.code = ErrorCode::HostQueueOverflow;
    e.message = "lane 'gui' coalesced away a stale block";
    e.atSample = SampleIndex(atSample);
    return e;
}

TEST(DiagnosticsLogTest, IdenticalConditionsMergeAndPreserveMagnitude) {
    DiagnosticsLog log;
    for (int i = 0; i < 1000; ++i) {
        log.record(overflowEvent(static_cast<std::uint64_t>(i)));
    }
    auto const events = log.snapshot();
    // One entry, not a thousand: a panel with a thousand identical rows is unreadable.
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].count, 1000u);
    EXPECT_TRUE(events[0].firstSeenMonotonicNs <= events[0].lastSeenMonotonicNs);

    auto const summary = log.summary();
    EXPECT_EQ(summary.totalRecorded, 1000u);
    EXPECT_EQ(summary.distinctConditions, 1u);
    EXPECT_EQ(summary.overflows, 1000u);
    EXPECT_EQ(summary.bySeverity[static_cast<std::size_t>(DiagnosticSeverity::Error)], 1000u);
}

TEST(DiagnosticsLogTest, DifferentConditionsStaySeparate) {
    DiagnosticsLog log;
    log.record(ErrorCode::CrcMismatch, "body crc failed");
    log.record(ErrorCode::SequenceGap, "sequence jumped 4 -> 9");
    log.record(ErrorCode::HostQueueOverflow, "gui lane full");
    EXPECT_EQ(log.size(), 3u);
    auto const summary = log.summary();
    EXPECT_EQ(summary.crcErrors, 1u);
    EXPECT_EQ(summary.sequenceGaps, 1u);
    EXPECT_EQ(summary.overflows, 1u);
}

TEST(DiagnosticsLogTest, EvictionIsCountedNeverSilent) {
    DiagnosticsLog log(4);
    for (int i = 0; i < 10; ++i) {
        DiagnosticEvent e;
        e.severity = DiagnosticSeverity::Warning;
        e.code = ErrorCode::Unknown;
        // Distinct messages so nothing merges and the capacity really is exercised.
        e.message = "distinct condition " + std::to_string(i);
        log.record(std::move(e));
    }
    EXPECT_EQ(log.size(), 4u);
    EXPECT_EQ(log.evictedCount(), 6u);
    EXPECT_EQ(log.summary().evicted, 6u);
    // A log that dropped entries is by definition not clean.
    EXPECT_FALSE(log.isClean());
}

TEST(DiagnosticsLogTest, CleanMeansNothingWasRecordedOrDropped) {
    DiagnosticsLog log;
    EXPECT_TRUE(log.isClean());
    log.record(ErrorCode::Ok, "informational only");
    EXPECT_FALSE(log.isClean());
    log.clear();
    EXPECT_TRUE(log.isClean());
    EXPECT_EQ(log.summary().totalRecorded, 0u);
}

TEST(DiagnosticsLogTest, OkStatusIsNotRecorded) {
    DiagnosticsLog log;
    log.record(Status::success());
    EXPECT_TRUE(log.isClean());
    log.record(Status::error(ErrorCode::CrcMismatch, "bad crc"));
    EXPECT_EQ(log.size(), 1u);
    EXPECT_EQ(log.summary().crcErrors, 1u);
}

TEST(DiagnosticsLogTest, SeverityFilterWorks) {
    DiagnosticsLog log;
    DiagnosticEvent info;
    info.severity = DiagnosticSeverity::Info;
    info.code = ErrorCode::Ok;
    info.message = "info";
    log.record(info);
    log.record(Status::error(ErrorCode::DeviceResetDetected, "device reset"));

    EXPECT_EQ(log.snapshot().size(), 2u);
    auto const errors = log.snapshotWithSeverityAtLeast(DiagnosticSeverity::Error);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].code, ErrorCode::DeviceResetDetected);
}

TEST(DiagnosticsLogTest, ListenerSeesEveryEventAndMayReenter) {
    DiagnosticsLog log;
    std::uint64_t seen = 0;
    std::uint64_t totalCount = 0;
    log.addListener([&](const DiagnosticEvent& e) {
        seen += 1;
        totalCount += e.count;
        // Re-entrant: the log must not be holding its lock while notifying, or this
        // deadlocks.
        if (seen == 1) {
            EXPECT_EQ(log.size(), 1u);
        }
    });
    for (int i = 0; i < 5; ++i) {
        log.record(overflowEvent(static_cast<std::uint64_t>(i)));
    }
    EXPECT_EQ(seen, 5u);
    EXPECT_EQ(totalCount, 5u);
}

TEST(DiagnosticsLogTest, ConcurrentRecordingLosesNothing) {
    DiagnosticsLog log(1'000'000);
    constexpr int kThreads = 8;
    constexpr int kPerThread = 500;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&log, t] {
            for (int i = 0; i < kPerThread; ++i) {
                DiagnosticEvent e;
                e.severity = DiagnosticSeverity::Warning;
                e.code = ErrorCode::Unknown;
                e.message = "thread " + std::to_string(t);
                log.record(std::move(e));
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    // kThreads distinct conditions, kThreads * kPerThread total occurrences.
    EXPECT_EQ(log.size(), static_cast<std::size_t>(kThreads));
    EXPECT_EQ(log.summary().totalRecorded,
              static_cast<std::uint64_t>(kThreads) * kPerThread);
    EXPECT_EQ(log.evictedCount(), 0u);
}

}  // namespace
