// -----------------------------------------------------------------------------
// capture_pipeline.h -- threads, fan-out and backpressure
// (docs/architecture_review.md section 6).
//
// The invariants this class exists to enforce, each of which is a test rather than
// a comment:
//
//   1. The GUI path can never stall the RX path. A lane with QueuePolicy
//      CoalesceLatest holds one item; a wedged consumer loses snapshots, and the
//      loss is counted and reported.
//   2. There is no QueuePolicy that drops silently. DropOldestWithDiagnostic
//      emits a DiagnosticEvent per drop and increments a visible counter.
//   3. Every cross-thread payload is a move-only ownership transfer. No raw
//      pointer to mutable data crosses a thread boundary.
//   4. Threads are joined in a deterministic order at shutdown so no thread can
//      write to a destroyed sink.
// -----------------------------------------------------------------------------
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "usn/common/status.h"
#include "usn/core/diagnostics_log.h"
#include "usn/hal/idevice.h"
#include "usn/model/sample.h"

namespace usn::core {

enum class QueuePolicy : std::uint8_t {
    // Storage: never lose data. Blocks the producer when full, which is
    // intentional and reported.
    BlockProducer = 0,
    // GUI: never stall the capture. Holds the latest item only.
    CoalesceLatest,
    // Analysis/decode: bounded, but every drop is reported.
    DropOldestWithDiagnostic
};

[[nodiscard]] std::string_view nameOf(QueuePolicy policy) noexcept;

struct LaneCounters {
    std::string name;
    QueuePolicy policy{QueuePolicy::BlockProducer};
    std::uint64_t queued{0};            // total accepted
    std::uint64_t delivered{0};         // total handed to the sink
    std::uint64_t dropped{0};           // total discarded by policy
    std::uint64_t blocked{0};           // times the producer had to wait
    std::uint64_t highWater{0};
    std::size_t capacity{0};
    std::size_t currentDepth{0};
    bool running{false};
};

struct PipelineCounters {
    std::uint64_t blocksIn{0};
    std::uint64_t samplesIn{0};
    std::uint64_t bytesIn{0};
    std::uint64_t ingressDropped{0};    // rejected at the ingress queue
    std::uint64_t ingressHighWater{0};
    std::vector<LaneCounters> lanes;
};

struct LaneConfig {
    hal::ISampleSink* sink{nullptr};    // non-owning; must outlive the pipeline
    QueuePolicy policy{QueuePolicy::BlockProducer};
    std::size_t capacity{256};
    std::string name;
};

struct PipelineConfig {
    std::size_t ingressCapacity{512};
    // How long onBlock() waits for ingress space before declaring the pipeline
    // unable to keep up. Bounded so a wedged lane cannot hang a capture forever.
    std::uint32_t ingressBlockTimeoutMs{2000};
    std::string name{"capture"};
};

// Implements ISampleSink so a device or replay source can push into it directly.
class CapturePipeline final : public hal::ISampleSink {
public:
    explicit CapturePipeline(DiagnosticsLog& diagnostics, PipelineConfig config = {});

    // Stops the pipeline if it is still running, and reports a failure to do so
    // rather than discarding it. Like abort(), this joins the lane workers, so a
    // sink that blocks indefinitely inside onBlock() will block teardown: lane
    // sinks are required to make progress on their own.
    ~CapturePipeline() final;

    CapturePipeline(const CapturePipeline&) = delete;
    CapturePipeline& operator=(const CapturePipeline&) = delete;

    // Must be called before start(). Lanes cannot be added while running: a lane
    // added mid-capture would have an undefined position in the stream.
    [[nodiscard]] Status addLane(LaneConfig lane);
    [[nodiscard]] std::size_t laneCount() const;

    [[nodiscard]] Status start();
    [[nodiscard]] Status stop();       // graceful: drains, then joins

    // Discards queued blocks and reports each discard as a DiagnosticEvent.
    //
    // "Immediate" applies to work still in a queue. A worker that is already inside
    // sink->onBlock() cannot be preempted -- C++ offers no safe way to interrupt a
    // thread -- so abort() waits for that one in-flight call to return before it
    // joins. A sink that blocks forever will therefore block abort() forever, which
    // is why every lane policy in this pipeline is required to make progress on its
    // own and why the ingress wait is bounded by ingressBlockTimeoutMs.
    void abort();

    [[nodiscard]] bool isRunning() const noexcept;

    // hal::ISampleSink
    hal::Backpressure onBlock(OwningSampleBlock block) final;
    void onDiagnostic(DiagnosticEvent event) final;

    [[nodiscard]] PipelineCounters counters() const;

private:
    struct Lane;
    void laneWorker(Lane& lane);
    void dispatchToLanes(OwningSampleBlock& block);
    // Snapshot of the lane list. m_lanesMutex guards the vector itself, not the
    // lanes: it is taken only long enough to copy the pointers, because holding it
    // across a blocking lane wait would let a full lane stall counters() -- the very
    // telemetry call that exists to report that the lane is full. Lanes are created
    // before start() and never removed while threads run, so the pointers stay valid.
    [[nodiscard]] std::vector<Lane*> laneSnapshot() const;
    void drainIngress();

    DiagnosticsLog& m_diagnostics;
    PipelineConfig m_config;

    mutable std::mutex m_ingressMutex;
    std::condition_variable m_ingressCv;
    std::deque<OwningSampleBlock> m_ingress;
    std::atomic<std::uint64_t> m_ingressHighWater{0};
    std::atomic<std::uint64_t> m_ingressDropped{0};

    std::vector<std::unique_ptr<Lane>> m_lanes;
    mutable std::mutex m_lanesMutex;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_abort{false};
    std::jthread m_dispatchThread;

    mutable std::mutex m_counterMutex;
    std::uint64_t m_blocksIn{0};
    std::uint64_t m_samplesIn{0};
    std::uint64_t m_bytesIn{0};
};

}  // namespace usn::core
