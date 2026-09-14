// -----------------------------------------------------------------------------
// capture_session.h -- the orchestrator that binds device, pipeline, storage,
// timeline and diagnostics into one capture.
//
// This is the object the (Phase 2) application layer drives. It owns the rules that
// keep a capture honest:
//
//   * Configuration is validated against the capabilities the DEVICE REPORTS, on the
//     host, before anything is sent. Nothing is hardcoded per model.
//   * Every block that arrives is checked for index continuity. A hole becomes a
//     GapRecord plus a DiagnosticEvent; it never becomes a shifted timeline.
//   * Storage and timeline are updated from the same call, so they cannot disagree.
//   * stop() is graceful (flush, then close) and abort() is immediate but reports
//     exactly what was discarded.
// -----------------------------------------------------------------------------
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "usn/common/status.h"
#include "usn/core/capture_pipeline.h"
#include "usn/core/config.h"
#include "usn/core/diagnostics_log.h"
#include "usn/core/istorage.h"
#include "usn/core/timeline.h"
#include "usn/hal/icapture_device.h"
#include "usn/hal/idevice.h"
#include "usn/model/capabilities.h"
#include "usn/trigger/trigger_node.h"

namespace usn::core {

enum class SessionState : std::uint8_t {
    Idle = 0,
    Attached,       // a device is bound but not configured
    Prepared,       // config validated, storage open, pipeline lanes built
    Armed,          // trigger compiled and armed
    Running,
    Flushing,       // device stopped, pipeline still draining
    Stopped,
    Aborted,
    Failed
};

[[nodiscard]] std::string_view nameOf(SessionState state) noexcept;

struct SessionStatistics {
    std::uint64_t blocksReceived{0};
    std::uint64_t blocksStored{0};
    std::uint64_t blocksRejected{0};
    std::uint64_t samplesStored{0};
    std::uint64_t bytesStored{0};
    std::uint64_t gapsDetected{0};
    std::uint64_t missingSamples{0};
    std::uint64_t firstSampleIndex{0};
    std::uint64_t lastSampleIndexExclusive{0};
    bool complete{0};          // true only when stored range is contiguous and flushed
};

struct SessionOptions {
    StorageTarget storageTarget{StorageTarget::InMemory};
    // When true, a rejected block stops the capture instead of being logged and
    // skipped. Default true: continuing after the pipeline says it cannot keep up
    // would produce a capture with unreported holes.
    bool stopOnRejectedBlock{true};
    // When true, reaching maxSamples ends the capture on its own.
    bool enforceMaxSamples{true};
};

class CaptureSession final : public hal::ISampleSink {
public:
    CaptureSession(DiagnosticsLog& diagnostics, AppConfig config, SessionOptions options = {});
    ~CaptureSession() final;

    CaptureSession(const CaptureSession&) = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;

    // Binds a device. The device must outlive the session; it is never owned here.
    [[nodiscard]] Status attach(hal::ICaptureDevice& device);
    void detach();
    [[nodiscard]] hal::ICaptureDevice* device() const noexcept;

    // Queries the device's reported capabilities and validates `request` against
    // them. Opens storage and builds the pipeline lanes on success.
    [[nodiscard]] Status prepare(const CaptureConfiguration& request);

    // Compiles and arms a trigger. A device without a trigger engine is an error,
    // not a silent "trigger disabled".
    [[nodiscard]] Status arm(const trigger::TriggerNode& ast);
    [[nodiscard]] Status disarm();

    [[nodiscard]] Status start();
    [[nodiscard]] Status stop();      // graceful
    [[nodiscard]] Status abort();     // immediate

    [[nodiscard]] SessionState state() const noexcept;
    [[nodiscard]] SessionStatistics statistics() const;
    [[nodiscard]] const AppConfig& configuration() const noexcept { return m_config; }
    [[nodiscard]] const CaptureConfiguration& captureConfiguration() const noexcept {
        return m_captureConfig;
    }
    [[nodiscard]] const DeviceCapabilities& capabilities() const noexcept { return m_capabilities; }
    [[nodiscard]] bool hasCapabilities() const noexcept { return m_hasCapabilities; }

    [[nodiscard]] Timeline& timeline() noexcept { return m_timeline; }
    [[nodiscard]] const Timeline& timeline() const noexcept { return m_timeline; }
    [[nodiscard]] IStorage* storage() const noexcept { return m_storage.get(); }
    [[nodiscard]] CapturePipeline* pipeline() const noexcept { return m_pipeline.get(); }
    [[nodiscard]] PipelineCounters pipelineCounters() const;

    // True once the session has decided the capture should end on its own (sample
    // limit reached, or a lane asked to stop). The application layer polls this and
    // calls stop(); the session does not stop itself from inside a pipeline worker,
    // because stop() joins that worker.
    [[nodiscard]] bool stopRequested() const noexcept {
        return m_stopRequested.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::string stopReason() const;

    // ISampleSink -- this session is the storage lane's consumer.
    hal::Backpressure onBlock(OwningSampleBlock block) final;
    void onDiagnostic(DiagnosticEvent event) final;

private:
    [[nodiscard]] Status openStorage();
    [[nodiscard]] Status buildPipeline();
    void setState(SessionState state);
    void recordGap(SampleIndex expected, SampleIndex actual, bool deviceReported);
    [[nodiscard]] bool reachedSampleLimit() const noexcept;

    DiagnosticsLog& m_diagnostics;
    AppConfig m_config;
    SessionOptions m_options;

    hal::ICaptureDevice* m_device{nullptr};
    DeviceCapabilities m_capabilities{};
    bool m_hasCapabilities{false};
    CaptureConfiguration m_captureConfig{};

    std::unique_ptr<IStorage> m_storage;
    std::unique_ptr<CapturePipeline> m_pipeline;
    Timeline m_timeline;

    mutable std::mutex m_statsMutex;
    SessionStatistics m_stats;
    SampleIndex m_expectedNext{0};
    bool m_expectationValid{false};

    std::atomic<SessionState> m_state{SessionState::Idle};
    std::atomic<bool> m_stopRequested{false};
    mutable std::mutex m_reasonMutex;
    std::string m_stopReason;
};

}  // namespace usn::core
