// -----------------------------------------------------------------------------
// capture_session.cpp -- see capture_session.h.
// -----------------------------------------------------------------------------
#include "usn/core/capture_session.h"

#include <utility>

#include <fmt/format.h>

#include "usn/common/log.h"
#include "usn/model/diagnostics.h"

namespace usn::core {

std::string_view nameOf(SessionState const state) noexcept {
    switch (state) {
    case SessionState::Idle:     return "idle";
    case SessionState::Attached: return "attached";
    case SessionState::Prepared: return "prepared";
    case SessionState::Armed:    return "armed";
    case SessionState::Running:  return "running";
    case SessionState::Flushing: return "flushing";
    case SessionState::Stopped:  return "stopped";
    case SessionState::Aborted:  return "aborted";
    case SessionState::Failed:   return "failed";
    }
    return "unknown";
}

CaptureSession::CaptureSession(DiagnosticsLog& diagnostics, AppConfig config,
                               SessionOptions options)
    : m_diagnostics(diagnostics), m_config(std::move(config)), m_options(options) {}

CaptureSession::~CaptureSession() {
    if (m_state.load(std::memory_order_acquire) == SessionState::Running ||
        m_state.load(std::memory_order_acquire) == SessionState::Flushing) {
        USN_LOG_WARN(log::cats::kCapture,
                     "CaptureSession destroyed while {}; aborting", nameOf(state()));
        (void)abort();
    }
}

void CaptureSession::setState(SessionState const state) {
    auto const previous = m_state.exchange(state, std::memory_order_acq_rel);
    if (previous != state) {
        USN_LOG_INFO(log::cats::kCapture, "session {} -> {}", nameOf(previous), nameOf(state));
    }
}

Status CaptureSession::attach(hal::ICaptureDevice& device) {
    auto const current = m_state.load(std::memory_order_acquire);
    if (current == SessionState::Running || current == SessionState::Flushing) {
        return Status::error(ErrorCode::DeviceBusy,
                             fmt::format("cannot attach a device while the session is {}",
                                         nameOf(current)));
    }
    if (m_device != nullptr && m_device != &device) {
        return Status::error(ErrorCode::DeviceBusy,
                             "another device is already attached; call detach() first");
    }
    m_device = &device;
    m_hasCapabilities = false;
    m_capabilities = DeviceCapabilities{};
    setState(SessionState::Attached);
    USN_LOG_INFO(log::cats::kDevice, "attached device '{}'", device.identity().name);
    return Status::success();
}

void CaptureSession::detach() {
    if (m_device != nullptr) {
        m_device->setSampleSink(nullptr);
    }
    m_device = nullptr;
    m_hasCapabilities = false;
    m_pipeline.reset();
    m_storage.reset();
    m_timeline = Timeline{};
    m_expectationValid = false;
    {
        std::scoped_lock const lock(m_statsMutex);
        m_stats = SessionStatistics{};
    }
    m_stopRequested.store(false, std::memory_order_release);
    setState(SessionState::Idle);
}

hal::ICaptureDevice* CaptureSession::device() const noexcept { return m_device; }

SessionState CaptureSession::state() const noexcept { return m_state.load(std::memory_order_acquire); }

Status CaptureSession::prepare(const CaptureConfiguration& request) {
    if (m_device == nullptr) {
        return Status::error(ErrorCode::DeviceNotConnected,
                             "no device is attached; call attach() before prepare()");
    }
    auto const current = m_state.load(std::memory_order_acquire);
    if (current == SessionState::Running || current == SessionState::Flushing) {
        return Status::error(ErrorCode::CaptureAlreadyRunning,
                             fmt::format("cannot prepare while the session is {}", nameOf(current)));
    }

    // connect() only when the device is not already connected. A prepare() that
    // failed validation leaves the device connected, and calling connect() again
    // would answer DeviceBusy -- which would make an invalid request un-retryable
    // and turn one bad configuration into a stuck session.
    auto const deviceState = m_device->state();
    if (deviceState == hal::DeviceState::Disconnected ||
        deviceState == hal::DeviceState::Error) {
        auto connectResult = m_device->connect();
        if (!connectResult.ok()) {
            m_diagnostics.record(connectResult);
            setState(SessionState::Failed);
            return connectResult;
        }
    }

    // Capabilities come from the device, never from a table keyed by product id.
    auto caps = m_device->queryCapabilities();
    if (!caps.ok()) {
        m_diagnostics.record(caps.status());
        setState(SessionState::Failed);
        return caps.status();
    }
    m_capabilities = *caps;
    m_hasCapabilities = true;

    auto const validity = validateCaptureConfiguration(request, m_capabilities);
    if (!validity.ok()) {
        // Validating on the host first means the failure message names the exact
        // unsupported value instead of the device rejecting an opaque blob.
        m_diagnostics.record(validity);
        return validity;
    }

    auto const configured = m_device->configure(request);
    if (!configured.ok()) {
        m_diagnostics.record(configured);
        setState(SessionState::Failed);
        return configured;
    }
    m_captureConfig = m_device->configuration();

    auto const storageOpened = openStorage();
    if (!storageOpened.ok()) {
        m_diagnostics.record(storageOpened);
        setState(SessionState::Failed);
        return storageOpened;
    }

    auto const pipelineBuilt = buildPipeline();
    if (!pipelineBuilt.ok()) {
        m_diagnostics.record(pipelineBuilt);
        setState(SessionState::Failed);
        return pipelineBuilt;
    }

    m_device->setSampleSink(m_pipeline.get());
    m_timeline = Timeline{};
    m_expectationValid = false;
    m_stopRequested.store(false, std::memory_order_release);
    {
        std::scoped_lock const lock(m_statsMutex);
        m_stats = SessionStatistics{};
    }
    setState(SessionState::Prepared);
    USN_LOG_INFO(log::cats::kCapture,
                 "prepared: {} ch @ {} Hz, stride {} B, mask 0x{:x}, pre-trigger {} samples",
                 m_captureConfig.channelCount, m_captureConfig.sampleRateHz,
                 m_config.capture.strideBytes, m_captureConfig.channelMask,
                 m_captureConfig.preTriggerSamples);
    return Status::success();
}

Status CaptureSession::openStorage() {
    m_storage = IStorage::create(m_options.storageTarget);
    if (m_storage == nullptr) {
        return Status::error(ErrorCode::NotImplemented,
                             fmt::format("storage target '{}' is not implemented in this phase",
                                         nameOf(m_options.storageTarget)));
    }
    // Stride is a host-side packing decision, not a device capability: the device
    // reports channelCountMax and the rate envelope, and the host chooses the
    // narrowest word that holds the enabled channels. Refusing here means a
    // mis-set stride can never reach storage or a .usn file.
    auto const stride = m_config.capture.strideBytes;
    if (static_cast<std::uint64_t>(stride) * 8 < m_captureConfig.channelCount) {
        return Status::error(
            ErrorCode::StrideUnsupported,
            fmt::format("stride {} bytes cannot hold {} enabled channels (needs at least {} "
                        "bytes)",
                        stride, m_captureConfig.channelCount,
                        (m_captureConfig.channelCount + 7) / 8));
    }

    StorageMetadata meta;
    meta.formatName = "usn-inmemory";
    meta.formatVersion = 1;
    meta.strideBytes = stride;
    meta.channelCount = static_cast<std::uint8_t>(m_captureConfig.channelCount);
    meta.channelMask = m_captureConfig.channelMask;
    meta.sampleRateHz = m_captureConfig.sampleRateHz;
    meta.createdUnixNs = 0;   // filled by the app layer, which owns wall-clock policy
    if (m_device != nullptr) {
        auto const id = m_device->identity();
        meta.deviceName = id.name;
        meta.firmwareVersion = id.firmwareVersion;
    }
    return m_storage->open(meta);
}

Status CaptureSession::buildPipeline() {
    PipelineConfig pc;
    pc.ingressCapacity = m_config.threading.ingressCapacity;
    pc.ingressBlockTimeoutMs = m_config.threading.ingressBlockTimeoutMs;
    pc.name = "session";
    m_pipeline = std::make_unique<CapturePipeline>(m_diagnostics, pc);

    LaneConfig storageLane;
    storageLane.sink = this;
    // BlockProducer: storage must never lose a block. If it cannot keep up, the
    // producer is told, which surfaces as a reported overflow rather than a hole
    // nobody knows about.
    storageLane.policy = QueuePolicy::BlockProducer;
    storageLane.capacity = m_config.threading.storageLaneCapacity;
    storageLane.name = "storage";
    return m_pipeline->addLane(std::move(storageLane));
}

Status CaptureSession::arm(const trigger::TriggerNode& ast) {
    if (m_device == nullptr) {
        return Status::error(ErrorCode::DeviceNotConnected, "no device is attached");
    }
    if (m_state.load(std::memory_order_acquire) != SessionState::Prepared &&
        m_state.load(std::memory_order_acquire) != SessionState::Armed) {
        return Status::error(ErrorCode::CaptureNotArmed,
                             fmt::format("session must be prepared before arming (it is {})",
                                         nameOf(state())));
    }
    auto* engine = m_device->triggerEngine();
    if (engine == nullptr) {
        // "No trigger engine" and "trigger disabled" are different facts. Guessing
        // the second would let a user believe a trigger is active when it is not.
        return Status::error(ErrorCode::TriggerUnsupported,
                             "the attached device reports no trigger engine");
    }
    auto compiled = engine->compile(ast);
    if (!compiled.ok()) {
        m_diagnostics.record(compiled);
        return compiled;
    }
    auto armed = engine->arm();
    if (!armed.ok()) {
        m_diagnostics.record(armed);
        return armed;
    }
    auto deviceArmed = m_device->arm();
    if (!deviceArmed.ok()) {
        (void)engine->disarm();
        m_diagnostics.record(deviceArmed);
        setState(SessionState::Failed);
        return deviceArmed;
    }
    setState(SessionState::Armed);
    return Status::success();
}

Status CaptureSession::disarm() {
    if (m_device == nullptr) {
        return Status::success();
    }
    auto* engine = m_device->triggerEngine();
    if (engine == nullptr) {
        return Status::error(ErrorCode::TriggerUnsupported, "the device has no trigger engine");
    }
    auto result = engine->disarm();
    if (!result.ok()) {
        m_diagnostics.record(result);
    }
    if (m_state.load(std::memory_order_acquire) == SessionState::Armed) {
        setState(SessionState::Prepared);
    }
    return result;
}

Status CaptureSession::start() {
    auto const current = m_state.load(std::memory_order_acquire);
    if (current != SessionState::Prepared && current != SessionState::Armed) {
        return Status::error(ErrorCode::CaptureNotArmed,
                             fmt::format("cannot start from state '{}'", nameOf(current)));
    }
    if (m_pipeline == nullptr || m_storage == nullptr || m_device == nullptr) {
        return Status::error(ErrorCode::ConfigurationError,
                             "session is not fully prepared (pipeline, storage or device missing)");
    }
    auto started = m_pipeline->start();
    if (!started.ok()) {
        m_diagnostics.record(started);
        setState(SessionState::Failed);
        return started;
    }
    auto deviceStarted = m_device->start();
    if (!deviceStarted.ok()) {
        (void)m_pipeline->stop();
        m_diagnostics.record(deviceStarted);
        setState(SessionState::Failed);
        return deviceStarted;
    }
    setState(SessionState::Running);
    return Status::success();
}

Status CaptureSession::stop() {
    auto const current = m_state.load(std::memory_order_acquire);
    if (current == SessionState::Stopped || current == SessionState::Idle) {
        return Status::success();
    }
    if (current != SessionState::Running && current != SessionState::Flushing &&
        current != SessionState::Armed && current != SessionState::Prepared &&
        current != SessionState::Failed) {
        return Status::error(ErrorCode::ConfigurationError,
                             fmt::format("cannot stop from state '{}'", nameOf(current)));
    }
    setState(SessionState::Flushing);

    Status deviceResult = Status::success();
    if (m_device != nullptr && (current == SessionState::Running || current == SessionState::Armed)) {
        deviceResult = m_device->stop();
        if (!deviceResult.ok()) {
            m_diagnostics.record(deviceResult);
        }
    }
    // Drain the pipeline AFTER the device has stopped producing, so the last blocks
    // are not left in a queue.
    if (m_pipeline != nullptr) {
        auto const pipelineResult = m_pipeline->stop();
        if (!pipelineResult.ok()) {
            m_diagnostics.record(pipelineResult);
        }
    }
    Status storageResult = Status::success();
    if (m_storage != nullptr) {
        storageResult = m_storage->close();
        if (!storageResult.ok()) {
            m_diagnostics.record(storageResult);
        }
    }

    {
        std::scoped_lock const lock(m_statsMutex);
        if (m_storage != nullptr) {
            auto const stats = m_storage->statistics();
            m_stats.blocksStored = stats.blocksWritten;
            m_stats.samplesStored = stats.samplesWritten;
            m_stats.bytesStored = stats.bytesWritten;
        }
        // "Complete" means: nothing rejected, no gaps, storage durable. Anything less
        // is reported as less.
        m_stats.complete = deviceResult.ok() && storageResult.ok() &&
                           m_stats.blocksRejected == 0 && m_stats.gapsDetected == 0 &&
                           m_stats.blocksStored == m_stats.blocksReceived;
        m_stats.missingSamples = m_timeline.totalMissingSamples();
    }
    setState(SessionState::Stopped);
    auto const totals = statistics();
    USN_LOG_INFO(log::cats::kCapture,
                 "capture stopped: {} blocks, {} samples, {} bytes, complete={}",
                 totals.blocksStored, totals.samplesStored, totals.bytesStored, totals.complete);
    if (!deviceResult.ok()) {
        return deviceResult;
    }
    return storageResult;
}

Status CaptureSession::abort() {
    auto const current = m_state.load(std::memory_order_acquire);
    if (current == SessionState::Idle) {
        return Status::success();
    }
    if (m_device != nullptr && current == SessionState::Running) {
        auto const deviceResult = m_device->abort();
        if (!deviceResult.ok()) {
            m_diagnostics.record(deviceResult);
        }
    }
    if (m_pipeline != nullptr) {
        m_pipeline->abort();
    }
    if (m_storage != nullptr) {
        auto const storageResult = m_storage->close();
        if (!storageResult.ok()) {
            m_diagnostics.record(storageResult);
        }
    }
    {
        std::scoped_lock const lock(m_statsMutex);
        m_stats.complete = false;   // an aborted capture is never complete
        if (m_storage != nullptr) {
            auto const stats = m_storage->statistics();
            m_stats.blocksStored = stats.blocksWritten;
            m_stats.samplesStored = stats.samplesWritten;
            m_stats.bytesStored = stats.bytesWritten;
        }
    }
    setState(SessionState::Aborted);
    return Status::success();
}

std::string CaptureSession::stopReason() const {
    std::scoped_lock const lock(m_reasonMutex);
    return m_stopReason;
}

void CaptureSession::recordGap(SampleIndex const expected, SampleIndex const actual,
                               bool const deviceReported) {
    GapRecord gap;
    gap.firstMissing = expected;
    gap.afterGap = actual;
    gap.missingSamples = actual.value > expected.value ? actual.value - expected.value : 0;
    gap.deviceReported = deviceReported;
    m_timeline.recordGap(gap);

    DiagnosticEvent event;
    event.severity = DiagnosticSeverity::Error;
    event.code = ErrorCode::SampleIndexGap;
    event.atSample = expected;
    event.message = fmt::format(
        "{} sample(s) missing between index {} and {}{}", gap.missingSamples, expected.value,
        actual.value, deviceReported ? " (device reported an overflow)" : "");
    m_diagnostics.record(std::move(event));

    {
        std::scoped_lock const lock(m_statsMutex);
        m_stats.gapsDetected += 1;
        m_stats.missingSamples += gap.missingSamples;
    }
}

bool CaptureSession::reachedSampleLimit() const noexcept {
    if (!m_options.enforceMaxSamples || m_config.capture.maxSamples == 0) {
        return false;
    }
    std::scoped_lock const lock(m_statsMutex);
    return m_stats.samplesStored >= m_config.capture.maxSamples;
}

hal::Backpressure CaptureSession::onBlock(OwningSampleBlock block) {
    auto const current = m_state.load(std::memory_order_acquire);
    if (current != SessionState::Running && current != SessionState::Flushing) {
        m_diagnostics.record(
            ErrorCode::CaptureNotArmed,
            fmt::format("session received a block in state '{}'; it is discarded",
                        nameOf(current)));
        return hal::Backpressure::RejectedStopCapture;
    }

    auto const validity = block.validate();
    if (!validity.ok()) {
        {
            std::scoped_lock const lock(m_statsMutex);
            m_stats.blocksRejected += 1;
        }
        m_diagnostics.record(validity);
        return m_options.stopOnRejectedBlock ? hal::Backpressure::RejectedStopCapture
                                             : hal::Backpressure::AcceptedWithWarning;
    }
    const auto& header = block.header();

    {
        std::scoped_lock const lock(m_statsMutex);
        m_stats.blocksReceived += 1;
    }

    // Continuity: a hole must stay a hole. Rewriting history or shifting the
    // timeline would make every later SampleIndex lie about its position.
    if (!m_expectationValid) {
        m_expectedNext = header.firstSampleIndex;
        m_expectationValid = true;
        auto const segResult = m_timeline.addSegment(header.firstSampleIndex, header.sampleRateHz);
        if (!segResult.ok()) {
            m_diagnostics.record(segResult);
        }
    } else if (header.firstSampleIndex.value != m_expectedNext.value) {
        if (header.firstSampleIndex.value > m_expectedNext.value) {
            recordGap(m_expectedNext, header.firstSampleIndex, hasFlag(header.flags, BlockFlag::OverflowBefore));
        } else {
            // Going backwards means the producer re-sent data. That is a protocol bug,
            // not a gap; recording it as a gap would understate the problem.
            DiagnosticEvent event;
            event.severity = DiagnosticSeverity::Error;
            event.code = ErrorCode::SampleIndexGap;
            event.atSample = header.firstSampleIndex;
            event.message = fmt::format(
                "block at sample {} precedes the expected next sample {}; the producer is "
                "re-sending or mis-ordering data",
                header.firstSampleIndex.value, m_expectedNext.value);
            m_diagnostics.record(std::move(event));
            {
                std::scoped_lock const lock(m_statsMutex);
                m_stats.blocksRejected += 1;
            }
            return m_options.stopOnRejectedBlock ? hal::Backpressure::RejectedStopCapture
                                                 : hal::Backpressure::AcceptedWithWarning;
        }
        auto const segResult = m_timeline.addSegment(header.firstSampleIndex, header.sampleRateHz);
        if (!segResult.ok()) {
            m_diagnostics.record(segResult);
        }
    }

    auto const appended = m_storage != nullptr ? m_storage->append(block.view())
                                               : Status::error(ErrorCode::ConfigurationError,
                                                               "session has no storage backend");
    if (!appended.ok()) {
        {
            std::scoped_lock const lock(m_statsMutex);
            m_stats.blocksRejected += 1;
        }
        m_diagnostics.record(appended);
        // Do not advance the expectation: the block was not stored.
        return m_options.stopOnRejectedBlock ? hal::Backpressure::RejectedStopCapture
                                             : hal::Backpressure::AcceptedWithWarning;
    }

    m_expectedNext = header.lastSampleIndexExclusive();
    m_timeline.extendTo(header.lastSampleIndexExclusive());
    {
        std::scoped_lock const lock(m_statsMutex);
        m_stats.blocksStored += 1;
        m_stats.samplesStored += header.sampleCount;
        m_stats.bytesStored += header.payloadBytes();
        m_stats.firstSampleIndex = m_timeline.origin().value;
        m_stats.lastSampleIndexExclusive = header.lastSampleIndexExclusive().value;
    }

    if (reachedSampleLimit()) {
        // A planned end, not an error: say so, then use the pipeline's own stop
        // mechanism. Calling stop() here would join this thread from itself.
        {
            std::scoped_lock const lock(m_reasonMutex);
            m_stopReason = fmt::format("reached maxSamples ({})", m_config.capture.maxSamples);
        }
        m_stopRequested.store(true, std::memory_order_release);
        DiagnosticEvent event;
        event.severity = DiagnosticSeverity::Info;
        event.code = ErrorCode::Ok;
        event.atSample = header.lastSampleIndexExclusive();
        event.message = fmt::format("capture reached the configured maximum of {} samples; "
                                    "requesting a graceful stop",
                                    m_config.capture.maxSamples);
        m_diagnostics.record(std::move(event));
        return hal::Backpressure::RejectedStopCapture;
    }
    return hal::Backpressure::Accepted;
}

void CaptureSession::onDiagnostic(DiagnosticEvent event) {
    m_diagnostics.record(std::move(event));
}

SessionStatistics CaptureSession::statistics() const {
    std::scoped_lock const lock(m_statsMutex);
    auto stats = m_stats;
    if (m_storage != nullptr) {
        auto const storageStats = m_storage->statistics();
        stats.blocksStored = storageStats.blocksWritten;
        stats.samplesStored = storageStats.samplesWritten;
        stats.bytesStored = storageStats.bytesWritten;
        stats.blocksRejected += storageStats.blocksRejected;
    }
    stats.missingSamples = m_timeline.totalMissingSamples();
    stats.gapsDetected = m_timeline.gaps().size();
    stats.lastSampleIndexExclusive = m_timeline.endExclusive().value;
    return stats;
}

PipelineCounters CaptureSession::pipelineCounters() const {
    if (m_pipeline == nullptr) {
        return PipelineCounters{};
    }
    return m_pipeline->counters();
}

}  // namespace usn::core
