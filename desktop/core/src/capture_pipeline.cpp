// -----------------------------------------------------------------------------
// capture_pipeline.cpp -- see capture_pipeline.h.
// -----------------------------------------------------------------------------
#include "usn/core/capture_pipeline.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include <fmt/format.h>

#include "usn/common/log.h"
#include "usn/common/perf.h"

namespace usn::core {

std::string_view nameOf(QueuePolicy const policy) noexcept {
    switch (policy) {
    case QueuePolicy::BlockProducer:           return "block-producer";
    case QueuePolicy::CoalesceLatest:          return "coalesce-latest";
    case QueuePolicy::DropOldestWithDiagnostic:return "drop-oldest-with-diagnostic";
    }
    return "unknown";
}

// A lane owns its queue and its worker thread. Kept out of the header so the
// threading primitives are an implementation detail.
struct CapturePipeline::Lane {
    LaneConfig config;
    std::mutex mutex;
    std::condition_variable notFull;
    std::condition_variable notEmpty;
    std::deque<OwningSampleBlock> queue;
    std::jthread worker;

    std::atomic<std::uint64_t> queued{0};
    std::atomic<std::uint64_t> delivered{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> blocked{0};
    std::atomic<std::uint64_t> highWater{0};
    std::atomic<bool> running{false};
    // Mirrors queue.size() so counters() can report depth without taking the lane
    // mutex, and therefore without being able to block behind a full lane.
    std::atomic<std::size_t> depth{0};

    [[nodiscard]] std::string label() const {
        return config.name.empty() ? std::string("lane") : config.name;
    }
};

CapturePipeline::CapturePipeline(DiagnosticsLog& diagnostics, PipelineConfig config)
    : m_diagnostics(diagnostics), m_config(std::move(config)) {}

CapturePipeline::~CapturePipeline() {
    // A pipeline destroyed while running would leave worker threads writing to
    // sinks that may already be gone. Stop deterministically instead.
    if (m_running.load(std::memory_order_acquire)) {
        // Being destroyed while running means the owner forgot to stop it. That is a
        // lifecycle bug, and like every other anomaly here it is reported rather than
        // quietly absorbed: the blocks still queued at this point are about to vanish.
        auto const queued = counters();
        std::uint64_t pending = 0;
        for (const auto& lane : queued.lanes) {
            pending += lane.currentDepth;
        }
        m_diagnostics.record(
            ErrorCode::ConfigurationError,
            fmt::format("pipeline '{}' was destroyed while still running; {} block(s) were still "
                        "queued and {} were in flight through the ingress",
                        m_config.name, pending, queued.ingressHighWater));
        USN_LOG_WARN(log::cats::kCapture,
                     "CapturePipeline '{}' destroyed while running; stopping", m_config.name);
        // stop() is [[nodiscard]] and a destructor cannot propagate failure, so the
        // result is recorded rather than discarded: a failed teardown is exactly the
        // kind of thing that must not disappear.
        if (auto const teardown = stop(); !teardown.ok()) {
            m_diagnostics.record(teardown);
            USN_LOG_ERROR(log::cats::kCapture, "teardown of pipeline '{}' failed: {}",
                          m_config.name, teardown.message());
        }
    }
}

Status CapturePipeline::addLane(LaneConfig lane) {
    if (lane.sink == nullptr) {
        return Status::error(ErrorCode::ConfigurationError, "lane has no sink");
    }
    if (m_running.load(std::memory_order_acquire)) {
        // A lane added mid-capture would have an undefined position in the stream:
        // it would see blocks from the middle onwards with no way to know what it
        // missed. Refuse rather than produce a silently incomplete decoder.
        return Status::error(ErrorCode::ConfigurationError,
                             fmt::format("cannot add lane '{}' while the pipeline is running",
                                         lane.name));
    }
    if (lane.capacity == 0) {
        return Status::error(ErrorCode::ConfigurationError,
                             fmt::format("lane '{}' has zero capacity", lane.name));
    }
    if (lane.policy == QueuePolicy::CoalesceLatest && lane.capacity != 1) {
        // Coalescing to anything larger than 1 is just DropOldest with extra steps;
        // forcing capacity 1 keeps the GUI invariant obvious.
        USN_LOG_INFO(log::cats::kCapture,
                     "lane '{}' uses CoalesceLatest; capacity {} reduced to 1", lane.name,
                     lane.capacity);
        lane.capacity = 1;
    }
    auto owned = std::make_unique<Lane>();
    owned->config = std::move(lane);
    std::scoped_lock const lock(m_lanesMutex);
    m_lanes.push_back(std::move(owned));
    return Status::success();
}

std::size_t CapturePipeline::laneCount() const {
    std::scoped_lock const lock(m_lanesMutex);
    return m_lanes.size();
}

Status CapturePipeline::start() {
    if (m_running.load(std::memory_order_acquire)) {
        return Status::error(ErrorCode::CaptureAlreadyRunning,
                             fmt::format("pipeline '{}' is already running", m_config.name));
    }
    {
        std::scoped_lock const lock(m_lanesMutex);
        if (m_lanes.empty()) {
            // Starting with no lanes would silently discard every block. That is
            // exactly the failure master spec section 14 forbids.
            return Status::error(ErrorCode::ConfigurationError,
                                 fmt::format("pipeline '{}' has no lanes; every captured block "
                                             "would be discarded",
                                             m_config.name));
        }
        for (auto& lane : m_lanes) {
            lane->running.store(true, std::memory_order_release);
            lane->worker = std::jthread([this, raw = lane.get()](const std::stop_token&) {
                laneWorker(*raw);
            });
        }
    }
    m_abort.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    m_dispatchThread = std::jthread([this](const std::stop_token&) { drainIngress(); });
    USN_LOG_INFO(log::cats::kCapture, "pipeline '{}' started with {} lane(s)", m_config.name,
                 laneCount());
    return Status::success();
}

Status CapturePipeline::stop() {
    if (!m_running.load(std::memory_order_acquire)) {
        return Status::success();
    }
    m_running.store(false, std::memory_order_release);
    m_ingressCv.notify_all();

    // Upstream first, so no new blocks are produced while lanes drain.
    if (m_dispatchThread.joinable()) {
        m_dispatchThread.join();
    }
    {
        std::scoped_lock const lock(m_lanesMutex);
        for (auto& lane : m_lanes) {
            lane->running.store(false, std::memory_order_release);
            lane->notEmpty.notify_all();
            lane->notFull.notify_all();
        }
        for (auto& lane : m_lanes) {
            if (lane->worker.joinable()) {
                lane->worker.join();
            }
        }
    }
    auto const totals = counters();
    USN_LOG_INFO(log::cats::kCapture, "pipeline '{}' stopped: {} blocks in, {} samples, {} bytes",
                 m_config.name, totals.blocksIn, totals.samplesIn, totals.bytesIn);
    return Status::success();
}

void CapturePipeline::abort() {
    m_abort.store(true, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    m_ingressCv.notify_all();
    {
        std::scoped_lock const lock(m_lanesMutex);
        for (auto& lane : m_lanes) {
            lane->running.store(false, std::memory_order_release);
            lane->notEmpty.notify_all();
            lane->notFull.notify_all();
        }
    }
    if (m_dispatchThread.joinable()) {
        m_dispatchThread.join();
    }
    std::scoped_lock const lock(m_lanesMutex);
    for (auto& lane : m_lanes) {
        if (lane->worker.joinable()) {
            lane->worker.join();
        }
        if (!lane->queue.empty()) {
            auto const discarded = lane->queue.size();
            lane->dropped.fetch_add(discarded, std::memory_order_relaxed);
            lane->queue.clear();
            lane->depth.store(0, std::memory_order_relaxed);
            m_diagnostics.record(
                ErrorCode::Cancelled,
                fmt::format("pipeline '{}' aborted: {} queued block(s) discarded from lane '{}'",
                            m_config.name, discarded, lane->label()));
        }
    }
    std::scoped_lock const ingressLock(m_ingressMutex);
    if (!m_ingress.empty()) {
        auto const discarded = m_ingress.size();
        m_ingressDropped.fetch_add(discarded, std::memory_order_relaxed);
        m_ingress.clear();
        m_diagnostics.record(
            ErrorCode::Cancelled,
            fmt::format("pipeline '{}' aborted: {} queued block(s) discarded from the ingress",
                        m_config.name, discarded));
    }
}

bool CapturePipeline::isRunning() const noexcept { return m_running.load(std::memory_order_acquire); }

hal::Backpressure CapturePipeline::onBlock(OwningSampleBlock block) {
    if (!m_running.load(std::memory_order_acquire)) {
        m_diagnostics.record(
            ErrorCode::CaptureNotArmed,
            fmt::format("pipeline '{}' received a block while not running; it is discarded",
                        m_config.name));
        m_ingressDropped.fetch_add(1, std::memory_order_relaxed);
        return hal::Backpressure::RejectedStopCapture;
    }

    auto const header = block.header();
    std::unique_lock lock(m_ingressMutex);
    bool spaceAcquired = false;
    if (m_ingress.size() >= m_config.ingressCapacity) {
        // Bounded wait rather than an unbounded one: a wedged lane must not hang a
        // capture forever, and the timeout is what turns that into a reportable
        // event instead of a stuck process.
        auto const deadline = std::chrono::milliseconds(m_config.ingressBlockTimeoutMs);
        spaceAcquired = m_ingressCv.wait_for(lock, deadline, [this] {
            return m_ingress.size() < m_config.ingressCapacity ||
                   !m_running.load(std::memory_order_acquire) ||
                   m_abort.load(std::memory_order_acquire);
        });
        if (!spaceAcquired) {
            lock.unlock();
            m_ingressDropped.fetch_add(1, std::memory_order_relaxed);
            m_diagnostics.record(
                ErrorCode::HostQueueOverflow,
                fmt::format("pipeline '{}' ingress full ({} blocks) for {} ms; block at sample {} "
                            "rejected -- the capture cannot keep up",
                            m_config.name, m_config.ingressCapacity,
                            m_config.ingressBlockTimeoutMs, header.firstSampleIndex.value));
            return hal::Backpressure::RejectedStopCapture;
        }
        if (!m_running.load(std::memory_order_acquire) || m_abort.load(std::memory_order_acquire)) {
            lock.unlock();
            m_ingressDropped.fetch_add(1, std::memory_order_relaxed);
            return hal::Backpressure::RejectedStopCapture;
        }
    }
    m_ingress.push_back(std::move(block));
    auto const depth = m_ingress.size();
    lock.unlock();
    m_ingressCv.notify_one();

    std::uint64_t prevHigh = m_ingressHighWater.load(std::memory_order_relaxed);
    while (depth > prevHigh &&
           !m_ingressHighWater.compare_exchange_weak(prevHigh, depth, std::memory_order_relaxed)) {
    }

    {
        std::scoped_lock const lockCounters(m_counterMutex);
        m_blocksIn += 1;
        m_samplesIn += header.sampleCount;
        m_bytesIn += header.payloadBytes();
    }
    return depth * 2 > m_config.ingressCapacity ? hal::Backpressure::AcceptedWithWarning
                                                : hal::Backpressure::Accepted;
}

void CapturePipeline::onDiagnostic(DiagnosticEvent event) { m_diagnostics.record(std::move(event)); }

void CapturePipeline::drainIngress() {
    while (true) {
        OwningSampleBlock block;
        {
            std::unique_lock lock(m_ingressMutex);
            m_ingressCv.wait(lock, [this] {
                return !m_ingress.empty() || !m_running.load(std::memory_order_acquire);
            });
            if (m_abort.load(std::memory_order_acquire)) {
                // Leave the ingress alone. abort() owns the accounting for whatever is
                // still queued and reports it as a Cancelled diagnostic; clearing it
                // here would discard those blocks without a trace, which is exactly
                // the silent loss this class exists to prevent.
                return;
            }
            if (m_ingress.empty()) {
                if (!m_running.load(std::memory_order_acquire)) {
                    return;
                }
                continue;
            }
            block = std::move(m_ingress.front());
            m_ingress.pop_front();
        }
        m_ingressCv.notify_one();  // a producer may be waiting for space

        perf::Probe probe(perf::probes::kPipelineDispatch, block.header().payloadBytes());
        dispatchToLanes(block);
    }
}

std::vector<CapturePipeline::Lane*> CapturePipeline::laneSnapshot() const {
    std::scoped_lock const lock(m_lanesMutex);
    std::vector<Lane*> lanes;
    lanes.reserve(m_lanes.size());
    for (const auto& lane : m_lanes) {
        lanes.push_back(lane.get());
    }
    return lanes;
}

void CapturePipeline::dispatchToLanes(OwningSampleBlock& block) {
    auto const lanes = laneSnapshot();
    for (std::size_t i = 0; i < lanes.size(); ++i) {
        auto& lane = *lanes[i];
        if (!lane.running.load(std::memory_order_acquire)) {
            continue;
        }
        // The last lane takes the block by move; earlier lanes would need a copy,
        // which the no-copy rule forbids. With a single lane (the common Phase 1
        // case) there is never a copy. Multi-lane fan-out therefore passes an
        // immutable shared view downstream; see the note below.
        OwningSampleBlock payload;
        if (i + 1 == lanes.size()) {
            payload = std::move(block);
        } else {
            // Fan-out to more than one lane: give each earlier lane its own copy of
            // the HEADER and let them share the payload through a refcounted buffer.
            // Phase 1 keeps this simple and explicit -- copying is measurable and
            // correct, and section 12 of the review defers zero-copy fan-out until
            // it has been benchmarked.
            payload = OwningSampleBlock(block.header(), block.payload());
        }

        std::unique_lock laneLock(lane.mutex);
        switch (lane.config.policy) {
        case QueuePolicy::BlockProducer:
            while (lane.queue.size() >= lane.config.capacity) {
                if (!lane.running.load(std::memory_order_acquire)) {
                    break;
                }
                lane.blocked.fetch_add(1, std::memory_order_relaxed);
                lane.notFull.wait(laneLock);
            }
            if (!lane.running.load(std::memory_order_acquire)) {
                // The lane stopped while this block was waiting for space (only abort()
                // does that; a graceful stop() drains first). Do not push into a queue
                // nobody will drain -- count it and report it instead.
                auto const discardedIndex = payload.header().firstSampleIndex.value;
                laneLock.unlock();
                lane.dropped.fetch_add(1, std::memory_order_relaxed);
                m_diagnostics.record(
                    ErrorCode::Cancelled,
                    fmt::format("lane '{}' stopped before the block at sample {} could be "
                                "delivered; it is discarded",
                                lane.label(), discardedIndex));
                continue;
            }
            break;
        case QueuePolicy::CoalesceLatest:
            if (!lane.queue.empty()) {
                lane.dropped.fetch_add(lane.queue.size(), std::memory_order_relaxed);
                lane.queue.clear();
                // Counted and reported. A coalescing lane is allowed to lose items by
                // design, but "by design" does not mean "silently".
                m_diagnostics.record(
                    ErrorCode::HostQueueOverflow,
                    fmt::format("lane '{}' coalesced away a stale block because the consumer is "
                                "not keeping up",
                                lane.label()));
            }
            break;
        case QueuePolicy::DropOldestWithDiagnostic:
            while (lane.queue.size() >= lane.config.capacity) {
                lane.queue.pop_front();
                lane.dropped.fetch_add(1, std::memory_order_relaxed);
                m_diagnostics.record(
                    ErrorCode::HostQueueOverflow,
                    fmt::format("lane '{}' is full ({} blocks); the oldest block was dropped "
                                "because the consumer cannot keep up",
                                lane.label(), lane.config.capacity));
            }
            break;
        }
        lane.queue.push_back(std::move(payload));
        lane.queued.fetch_add(1, std::memory_order_relaxed);
        auto const depth = lane.queue.size();
        lane.depth.store(depth, std::memory_order_relaxed);
        std::uint64_t prevHigh = lane.highWater.load(std::memory_order_relaxed);
        while (depth > prevHigh &&
               !lane.highWater.compare_exchange_weak(prevHigh, depth, std::memory_order_relaxed)) {
        }
        laneLock.unlock();
        lane.notEmpty.notify_one();
    }
}

void CapturePipeline::laneWorker(Lane& lane) {
    while (true) {
        OwningSampleBlock block;
        {
            std::unique_lock lock(lane.mutex);
            lane.notEmpty.wait(lock, [&lane] {
                return !lane.queue.empty() || !lane.running.load(std::memory_order_acquire);
            });
            if (lane.queue.empty()) {
                if (!lane.running.load(std::memory_order_acquire)) {
                    return;
                }
                continue;
            }
            block = std::move(lane.queue.front());
            lane.queue.pop_front();
            lane.depth.store(lane.queue.size(), std::memory_order_relaxed);
        }
        lane.notFull.notify_one();
        if (lane.config.sink == nullptr) {
            continue;
        }
        auto const bp = lane.config.sink->onBlock(std::move(block));
        lane.delivered.fetch_add(1, std::memory_order_relaxed);
        if (bp == hal::Backpressure::RejectedStopCapture) {
            m_diagnostics.record(
                ErrorCode::HostQueueOverflow,
                fmt::format("lane '{}' rejected a block and asked the capture to stop",
                            lane.label()));
            m_running.store(false, std::memory_order_release);
            m_ingressCv.notify_all();
            return;
        }
    }
}

PipelineCounters CapturePipeline::counters() const {
    PipelineCounters out;
    {
        std::scoped_lock const lock(m_counterMutex);
        out.blocksIn = m_blocksIn;
        out.samplesIn = m_samplesIn;
        out.bytesIn = m_bytesIn;
    }
    out.ingressDropped = m_ingressDropped.load(std::memory_order_relaxed);
    out.ingressHighWater = m_ingressHighWater.load(std::memory_order_relaxed);
    for (const auto& lanePtr : laneSnapshot()) {
        LaneCounters lc;
        lc.name = lanePtr->label();
        lc.policy = lanePtr->config.policy;
        lc.capacity = lanePtr->config.capacity;
        lc.queued = lanePtr->queued.load(std::memory_order_relaxed);
        lc.delivered = lanePtr->delivered.load(std::memory_order_relaxed);
        lc.dropped = lanePtr->dropped.load(std::memory_order_relaxed);
        lc.blocked = lanePtr->blocked.load(std::memory_order_relaxed);
        lc.highWater = lanePtr->highWater.load(std::memory_order_relaxed);
        lc.running = lanePtr->running.load(std::memory_order_relaxed);
        lc.currentDepth = lanePtr->depth.load(std::memory_order_relaxed);
        out.lanes.push_back(std::move(lc));
    }
    return out;
}

}  // namespace usn::core
