// -----------------------------------------------------------------------------
// capture_pipeline_test.cpp -- the two invariants the whole threading model rests
// on, plus the accounting that makes "no silent loss" checkable.
//
//   1. A slow GUI can never stall the capture path.
//   2. Nothing is discarded without being counted and reported.
//
// Both are tested by putting a lane into a known "consumer is not keeping up"
// state with a gate rather than by racing a sleep, so the tests are deterministic
// and do not flake on a loaded CI machine.
// -----------------------------------------------------------------------------
#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

#include "usn/core/capture_pipeline.h"
#include "usn/test/test_fixtures.h"

using usn::DiagnosticEvent;
using usn::ErrorCode;
using usn::OwningSampleBlock;
using usn::SampleIndex;
using usn::core::CapturePipeline;
using usn::core::DiagnosticsLog;
using usn::core::LaneConfig;
using usn::core::PipelineConfig;
using usn::core::QueuePolicy;
using usn::hal::Backpressure;
using usn::test::GatedSink;
using usn::test::makeCounterBlock;

namespace {

constexpr std::uint32_t kBlockSamples = 64;

OwningSampleBlock blockAt(std::uint64_t const index) {
    return makeCounterBlock(SampleIndex(index), kBlockSamples, 2, 16, 1'000'000);
}

CapturePipeline makePipeline(DiagnosticsLog& log, std::size_t const ingressCapacity = 512,
                             std::uint32_t const timeoutMs = 2000) {
    PipelineConfig config;
    config.ingressCapacity = ingressCapacity;
    config.ingressBlockTimeoutMs = timeoutMs;
    config.name = "test";
    return CapturePipeline(log, config);
}

LaneConfig makeLane(GatedSink& sink, QueuePolicy const policy, std::size_t const capacity,
                    std::string name) {
    LaneConfig lane;
    lane.sink = &sink;
    lane.policy = policy;
    lane.capacity = capacity;
    lane.name = std::move(name);
    return lane;
}

// Total count recorded across every diagnostic carrying `code`.
std::uint64_t totalDiagnosticCount(const DiagnosticsLog& log, ErrorCode const code) {
    std::uint64_t total = 0;
    for (const auto& event : log.snapshot()) {
        if (event.code == code) {
            total += event.count;
        }
    }
    return total;
}

TEST(CapturePipelineTest, StartRefusesToRunWithNoLanes) {
    DiagnosticsLog log;
    auto pipeline = makePipeline(log);
    // Starting with no lanes would discard every captured block, which is exactly
    // the silent loss master spec section 14 forbids.
    EXPECT_FALSE(pipeline.start().ok());
    EXPECT_FALSE(pipeline.isRunning());
}

TEST(CapturePipelineTest, LaneConfigurationIsValidated) {
    DiagnosticsLog log;
    auto pipeline = makePipeline(log);
    LaneConfig noSink;
    noSink.sink = nullptr;
    EXPECT_FALSE(pipeline.addLane(noSink).ok());

    GatedSink sink;
    EXPECT_FALSE(pipeline.addLane(makeLane(sink, QueuePolicy::BlockProducer, 0, "zero")).ok());

    // Coalescing to more than one item is just DropOldest with extra steps; the
    // pipeline forces capacity 1 so the GUI invariant stays obvious.
    ASSERT_TRUE(pipeline.addLane(makeLane(sink, QueuePolicy::CoalesceLatest, 32, "gui")).ok());
    EXPECT_EQ(pipeline.laneCount(), 1u);
}

TEST(CapturePipelineTest, LanesCannotBeAddedWhileRunning) {
    DiagnosticsLog log;
    GatedSink sink;
    auto pipeline = makePipeline(log);
    ASSERT_TRUE(pipeline.addLane(makeLane(sink, QueuePolicy::BlockProducer, 8, "storage")).ok());
    ASSERT_TRUE(pipeline.start().ok());
    // A lane added mid-capture would see blocks from the middle onwards with no way
    // to know what it missed.
    EXPECT_FALSE(pipeline.addLane(makeLane(sink, QueuePolicy::BlockProducer, 8, "late")).ok());
    EXPECT_EQ(pipeline.laneCount(), 1u);
    EXPECT_TRUE(pipeline.stop().ok());
}

TEST(CapturePipelineTest, BlocksReceivedWhileStoppedAreRejectedAndReported) {
    DiagnosticsLog log;
    GatedSink sink;
    auto pipeline = makePipeline(log);
    ASSERT_TRUE(pipeline.addLane(makeLane(sink, QueuePolicy::BlockProducer, 8, "storage")).ok());
    EXPECT_EQ(pipeline.onBlock(blockAt(0)), Backpressure::RejectedStopCapture);
    EXPECT_EQ(sink.received(), 0u);
    EXPECT_GT(totalDiagnosticCount(log, ErrorCode::CaptureNotArmed), 0u);
    EXPECT_EQ(pipeline.counters().ingressDropped, 1u);
}

TEST(CapturePipelineTest, CoalescingLaneNeverStallsTheProducer) {
    DiagnosticsLog log;
    GatedSink sink;
    sink.closeGate();     // the "GUI is not keeping up" state, held deterministically

    auto pipeline = makePipeline(log);
    ASSERT_TRUE(pipeline.addLane(makeLane(sink, QueuePolicy::CoalesceLatest, 1, "gui")).ok());
    ASSERT_TRUE(pipeline.start().ok());

    constexpr int kBlocks = 500;
    auto const started = std::chrono::steady_clock::now();
    for (int i = 0; i < kBlocks; ++i) {
        auto const bp = pipeline.onBlock(blockAt(static_cast<std::uint64_t>(i) * kBlockSamples));
        // Not one of these may be rejected: the producer is the capture path, and a
        // wedged consumer must never be allowed to push back on it. AcceptedWithWarning
        // (ingress more than half full) is still an acceptance.
        EXPECT_NE(bp, Backpressure::RejectedStopCapture);
    }
    auto const elapsed = std::chrono::steady_clock::now() - started;
    auto const elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    // Generous bound: the point is that it is bounded at all while the consumer is
    // gated indefinitely. A blocking policy would take forever here.
    EXPECT_LT(elapsedMs, 5000) << "producer was stalled by a gated coalescing lane";

    sink.release();
    ASSERT_TRUE(pipeline.stop().ok());

    auto const counters = pipeline.counters();
    ASSERT_EQ(counters.lanes.size(), 1u);
    const auto& lane = counters.lanes[0];
    EXPECT_EQ(lane.queued, static_cast<std::uint64_t>(kBlocks));
    EXPECT_EQ(lane.capacity, 1u);
    // The consumer was gated, so most blocks must have been coalesced away.
    EXPECT_GT(lane.dropped, 0u);
    // The bound is relative, not absolute: after release() the worker races the
    // dispatch thread, which is still draining the ingress backlog, so exactly how
    // many it manages to consume before stop() is scheduling-dependent. What is not
    // scheduling-dependent is that a capacity-1 coalescing lane cannot deliver
    // everything it was offered.
    EXPECT_LT(lane.delivered, lane.queued);
    // Invariant: every queued block is accounted for.
    EXPECT_EQ(lane.queued, lane.delivered + lane.dropped + lane.currentDepth);
}

TEST(CapturePipelineTest, CoalescedBlocksAreReportedNotSilentlyLost) {
    DiagnosticsLog log;
    GatedSink sink;
    sink.closeGate();

    auto pipeline = makePipeline(log);
    ASSERT_TRUE(pipeline.addLane(makeLane(sink, QueuePolicy::CoalesceLatest, 1, "gui")).ok());
    ASSERT_TRUE(pipeline.start().ok());
    for (int i = 0; i < 100; ++i) {
        EXPECT_NE(pipeline.onBlock(blockAt(static_cast<std::uint64_t>(i) * kBlockSamples)),
                  Backpressure::RejectedStopCapture);
    }
    sink.release();
    ASSERT_TRUE(pipeline.stop().ok());

    auto const lane = pipeline.counters().lanes[0];
    auto const reported = totalDiagnosticCount(log, ErrorCode::HostQueueOverflow);
    // The count reported to the user must equal the count actually discarded. If
    // these two ever diverge, the diagnostics panel is lying.
    EXPECT_EQ(reported, lane.dropped);
    EXPECT_GT(reported, 0u);
}

TEST(CapturePipelineTest, DropOldestCountsEveryDrop) {
    DiagnosticsLog log;
    GatedSink sink;
    sink.closeGate();

    auto pipeline = makePipeline(log);
    ASSERT_TRUE(
        pipeline.addLane(makeLane(sink, QueuePolicy::DropOldestWithDiagnostic, 4, "decode")).ok());
    ASSERT_TRUE(pipeline.start().ok());
    for (int i = 0; i < 50; ++i) {
        EXPECT_NE(pipeline.onBlock(blockAt(static_cast<std::uint64_t>(i) * kBlockSamples)),
                  Backpressure::RejectedStopCapture);
    }
    sink.release();
    ASSERT_TRUE(pipeline.stop().ok());

    auto const lane = pipeline.counters().lanes[0];
    EXPECT_EQ(lane.queued, 50u);
    EXPECT_GT(lane.dropped, 0u);
    EXPECT_EQ(lane.queued, lane.delivered + lane.dropped + lane.currentDepth);
    EXPECT_EQ(totalDiagnosticCount(log, ErrorCode::HostQueueOverflow), lane.dropped);
}

TEST(CapturePipelineTest, BlockingLaneLosesNothingEvenWhenSlow) {
    DiagnosticsLog log;
    GatedSink sink;   // gate open: the consumer works, just slowly

    auto pipeline = makePipeline(log);
    ASSERT_TRUE(
        pipeline.addLane(makeLane(sink, QueuePolicy::BlockProducer, 4, "storage")).ok());
    ASSERT_TRUE(pipeline.start().ok());

    constexpr int kBlocks = 200;
    for (int i = 0; i < kBlocks; ++i) {
        auto const bp = pipeline.onBlock(blockAt(static_cast<std::uint64_t>(i) * kBlockSamples));
        // A blocking lane may push back, but it must never discard.
        EXPECT_NE(bp, Backpressure::RejectedStopCapture);
    }
    ASSERT_TRUE(pipeline.stop().ok());

    auto const lane = pipeline.counters().lanes[0];
    EXPECT_EQ(lane.queued, static_cast<std::uint64_t>(kBlocks));
    EXPECT_EQ(lane.dropped, 0u);
    EXPECT_EQ(lane.delivered, static_cast<std::uint64_t>(kBlocks));
    EXPECT_EQ(sink.received(), static_cast<std::uint64_t>(kBlocks));
    // Storage is the one lane where a drop is never acceptable, so the diagnostics
    // log must be empty for it.
    EXPECT_TRUE(log.isClean());

    // Every block arrived exactly once, in order, with its own first-sample index.
    auto const indices = sink.receivedIndices();
    ASSERT_EQ(indices.size(), static_cast<std::size_t>(kBlocks));
    for (std::size_t i = 0; i < indices.size(); ++i) {
        EXPECT_EQ(indices[i].value, i * kBlockSamples) << "block " << i;
    }
}

TEST(CapturePipelineTest, ARejectingLaneStopsTheCaptureAndSaysSo) {
    DiagnosticsLog log;
    GatedSink sink;
    sink.setRejects(true);

    auto pipeline = makePipeline(log);
    ASSERT_TRUE(pipeline.addLane(makeLane(sink, QueuePolicy::BlockProducer, 16, "storage")).ok());
    ASSERT_TRUE(pipeline.start().ok());
    for (int i = 0; i < 10; ++i) {
        pipeline.onBlock(blockAt(static_cast<std::uint64_t>(i) * kBlockSamples));
    }
    // Give the worker a moment to observe the rejection and stop the pipeline.
    for (int i = 0; i < 200 && pipeline.isRunning(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_FALSE(pipeline.isRunning()) << "a lane asked the capture to stop and it kept going";
    EXPECT_TRUE(pipeline.stop().ok());
    EXPECT_GT(totalDiagnosticCount(log, ErrorCode::HostQueueOverflow), 0u);
}

TEST(CapturePipelineTest, IngressBackpressureIsBoundedAndReported) {
    DiagnosticsLog log;
    GatedSink sink;
    sink.closeGate();

    // Tiny ingress and a short timeout: the producer must be told the pipeline
    // cannot keep up rather than hanging for as long as the gate stays closed.
    auto pipeline = makePipeline(log, /*ingressCapacity=*/2, /*timeoutMs=*/100);
    ASSERT_TRUE(
        pipeline.addLane(makeLane(sink, QueuePolicy::BlockProducer, 1, "storage")).ok());
    ASSERT_TRUE(pipeline.start().ok());

    int rejected = 0;
    auto const started = std::chrono::steady_clock::now();
    for (int i = 0; i < 40; ++i) {
        if (pipeline.onBlock(blockAt(static_cast<std::uint64_t>(i) * kBlockSamples)) ==
            Backpressure::RejectedStopCapture) {
            ++rejected;
        }
    }
    auto const elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - started)
                               .count();

    EXPECT_GT(rejected, 0) << "a full pipeline accepted everything and lost it silently";
    EXPECT_GE(elapsedMs, 100) << "the producer did not wait for the declared timeout";
    EXPECT_LT(elapsedMs, 60'000) << "the producer waited far longer than the timeout allows";
    EXPECT_GT(totalDiagnosticCount(log, ErrorCode::HostQueueOverflow), 0u);
    EXPECT_GT(pipeline.counters().ingressDropped, 0u);

    sink.release();
    EXPECT_TRUE(pipeline.stop().ok());
}

TEST(CapturePipelineTest, AbortDiscardsQueuedBlocksAndReportsThem) {
    DiagnosticsLog log;
    GatedSink sink;
    sink.closeGate();

    auto pipeline = makePipeline(log, /*ingressCapacity=*/512);
    ASSERT_TRUE(
        pipeline.addLane(makeLane(sink, QueuePolicy::BlockProducer, 1, "storage")).ok());
    ASSERT_TRUE(pipeline.start().ok());

    constexpr int kBlocks = 20;
    for (int i = 0; i < kBlocks; ++i) {
        pipeline.onBlock(blockAt(static_cast<std::uint64_t>(i) * kBlockSamples));
    }
    // The lane has capacity 1 and its worker is gated, so at most two blocks can
    // have left the ingress by now; the rest are guaranteed still queued there and
    // therefore guaranteed to be discarded by the abort.
    //
    // abort() joins its workers, and a worker sitting inside a sink call cannot be
    // preempted -- C++ has no safe way to do that. The watchdog releases the gate so
    // the in-flight call can finish; this is a property of the design, documented on
    // CapturePipeline::abort(), not a workaround for the test.
    std::thread watchdog([&sink] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        sink.release();
    });
    pipeline.abort();
    watchdog.join();

    EXPECT_FALSE(pipeline.isRunning());
    EXPECT_LT(sink.received(), static_cast<std::uint64_t>(kBlocks));
    // An abort is a user action, but the discarded blocks still have to be named.
    EXPECT_GT(totalDiagnosticCount(log, ErrorCode::Cancelled), 0u);
    // abort() joins its own threads, so stop() afterwards must be a no-op success.
    EXPECT_TRUE(pipeline.stop().ok());
}

TEST(CapturePipelineTest, CountersAccountForEveryBlock) {
    DiagnosticsLog log;
    GatedSink storageSink;
    GatedSink guiSink;
    guiSink.closeGate();

    auto pipeline = makePipeline(log);
    ASSERT_TRUE(
        pipeline.addLane(makeLane(storageSink, QueuePolicy::BlockProducer, 16, "storage")).ok());
    ASSERT_TRUE(pipeline.addLane(makeLane(guiSink, QueuePolicy::CoalesceLatest, 1, "gui")).ok());
    ASSERT_TRUE(pipeline.start().ok());

    constexpr int kBlocks = 60;
    for (int i = 0; i < kBlocks; ++i) {
        pipeline.onBlock(blockAt(static_cast<std::uint64_t>(i) * kBlockSamples));
    }
    guiSink.release();
    ASSERT_TRUE(pipeline.stop().ok());

    auto const counters = pipeline.counters();
    EXPECT_EQ(counters.blocksIn, static_cast<std::uint64_t>(kBlocks));
    EXPECT_EQ(counters.samplesIn, static_cast<std::uint64_t>(kBlocks) * kBlockSamples);
    EXPECT_EQ(counters.bytesIn,
              static_cast<std::uint64_t>(kBlocks) * kBlockSamples * 2);
    ASSERT_EQ(counters.lanes.size(), 2u);
    for (const auto& lane : counters.lanes) {
        EXPECT_EQ(lane.queued, lane.delivered + lane.dropped + lane.currentDepth)
            << "lane " << lane.name << " lost track of a block";
        EXPECT_EQ(lane.currentDepth, 0u) << "lane " << lane.name << " was not drained";
    }
    // The blocking lane saw everything; the coalescing lane did not, and says so.
    EXPECT_EQ(counters.lanes[0].delivered, static_cast<std::uint64_t>(kBlocks));
    EXPECT_LT(counters.lanes[1].delivered, static_cast<std::uint64_t>(kBlocks));
}

TEST(CapturePipelineTest, ForwardedDiagnosticsReachTheLog) {
    DiagnosticsLog log;
    GatedSink sink;
    auto pipeline = makePipeline(log);
    ASSERT_TRUE(pipeline.addLane(makeLane(sink, QueuePolicy::BlockProducer, 8, "storage")).ok());

    DiagnosticEvent event;
    event.severity = usn::DiagnosticSeverity::Error;
    event.code = ErrorCode::CrcMismatch;
    event.message = "forwarded from the transport";
    pipeline.onDiagnostic(event);
    EXPECT_EQ(totalDiagnosticCount(log, ErrorCode::CrcMismatch), 1u);
}

TEST(CapturePipelineTest, DestroyingARunningPipelineStopsItCleanly) {
    DiagnosticsLog log;
    GatedSink sink;
    {
        auto pipeline = makePipeline(log);
        ASSERT_TRUE(
            pipeline.addLane(makeLane(sink, QueuePolicy::BlockProducer, 8, "storage")).ok());
        ASSERT_TRUE(pipeline.start().ok());
        ASSERT_EQ(pipeline.onBlock(blockAt(0)), Backpressure::Accepted);
        // Deliberately no stop(): the destructor must not leave a worker thread
        // writing to a sink that is about to go out of scope.
    }
    // Tearing down a running pipeline is a lifecycle bug in the owner, and it must
    // leave a trace: blocks that were still queued are about to disappear.
    EXPECT_GT(totalDiagnosticCount(log, ErrorCode::ConfigurationError), 0u);
}

}  // namespace
