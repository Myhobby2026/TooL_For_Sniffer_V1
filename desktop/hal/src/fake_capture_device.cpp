// -----------------------------------------------------------------------------
// fake_capture_device.cpp -- see fake_capture_device.h.
//
// The synthetic waveforms are hand-built to be recognisable in a waveform view and
// to break loudly when a sample is lost: the Counter pattern is a monotonic
// increment, so any gap or duplication is immediately visible.
// -----------------------------------------------------------------------------
#include "usn/hal/fake_capture_device.h"

#include <utility>

#include <fmt/format.h>

#include "usn/common/log.h"
#include "usn/trigger/trigger_node.h"
#include "usn_wire.h"

namespace usn::hal {

// --- FakeTriggerEngine -------------------------------------------------------

FakeTriggerEngine::FakeTriggerEngine(TriggerCapability const caps) : m_caps(caps) {}

Status FakeTriggerEngine::compile(const trigger::TriggerNode& ast) {
    auto const valid = trigger::validate(ast);
    if (!valid.ok()) {
        return valid;
    }
    auto const classification = trigger::classifyTree(ast, m_caps);
    if (classification.executability == trigger::Executability::Unsupported) {
        return Status::error(ErrorCode::TriggerUnsupported,
                             fmt::format("trigger cannot be executed by this device: {}",
                                         classification.reason));
    }
    if (classification.executability == trigger::Executability::HostOnly) {
        // Reported rather than silently downgraded. The caller decides whether to
        // run it on the host instead.
        return Status::error(ErrorCode::TriggerUnsupported,
                             fmt::format("trigger is host-only and cannot be armed on the device: {}",
                                         classification.reason))
            .withContext("hostOnlyNodes", std::to_string(classification.hostOnlyNodeCount));
    }
    m_compiled = ast;
    m_firedAt.reset();
    return Status::success();
}

Status FakeTriggerEngine::arm() {
    if (!m_compiled.has_value()) {
        return Status::error(ErrorCode::CaptureNotArmed, "no trigger has been compiled");
    }
    m_armed = true;
    m_firedAt.reset();
    return Status::success();
}

Status FakeTriggerEngine::disarm() {
    m_armed = false;
    return Status::success();
}

void FakeTriggerEngine::setCapabilities(TriggerCapability const caps) noexcept { m_caps = caps; }

void FakeTriggerEngine::fire(SampleIndex const at) noexcept { m_firedAt = at; }

const trigger::TriggerNode* FakeTriggerEngine::compiled() const noexcept {
    return m_compiled.has_value() ? &(*m_compiled) : nullptr;
}

// --- FakeCaptureDevice -------------------------------------------------------

struct FakeCaptureDevice::Impl {
    FakeDeviceOptions options;
    DeviceState state{DeviceState::Disconnected};
    CaptureConfiguration config{};
    DeviceCounters counters{};
    CaptureProgress progress{};
    ISampleSink* sink{nullptr};
    FakeTriggerEngine trigger{TriggerCapability::None};

    std::uint64_t streamId{0};
    std::uint64_t bootNonce{0x1234'5678};
    std::uint32_t sequence{0};
    SampleIndex nextSample{0};
    std::uint64_t deviceTick{0};

    // pending fault injections
    std::uint32_t overflowBlocksRemaining{0};
    std::uint32_t sequenceGapPending{0};
    std::uint64_t sampleIndexGapPending{0};
    bool failsCapabilityQuery{false};
    std::optional<DeviceCapabilities> capsOverride;

    std::vector<DiagnosticEvent> emitted;
};

FakeCaptureDevice::FakeCaptureDevice(FakeDeviceOptions options) : m_impl(std::make_unique<Impl>()) {
    m_impl->options = std::move(options);
    if (m_impl->options.identity.name.empty()) {
        m_impl->options.identity.name = "FakeCaptureDevice";
    }
    m_impl->options.identity.bootNonce = m_impl->bootNonce;
    m_impl->options.identity.firmwareVersion = "fake-1.0.0";
    m_impl->options.identity.hardwareRevision = "fake-rev0";
    m_impl->trigger.setCapabilities(m_impl->options.capabilities.supportedTriggers);
}

FakeCaptureDevice::~FakeCaptureDevice() = default;

DeviceIdentity FakeCaptureDevice::identity() const {
    auto id = m_impl->options.identity;
    id.bootNonce = m_impl->bootNonce;
    return id;
}

StatusOr<DeviceCapabilities> FakeCaptureDevice::queryCapabilities() {
    if (m_impl->failsCapabilityQuery) {
        return Status::error(ErrorCode::DeviceUnsupported,
                             "simulated failure to report capabilities");
    }
    if (m_impl->capsOverride.has_value()) {
        return *m_impl->capsOverride;
    }
    return m_impl->options.capabilities;
}

Status FakeCaptureDevice::connect() {
    if (m_impl->state != DeviceState::Disconnected) {
        return Status::error(ErrorCode::DeviceBusy,
                             fmt::format("cannot connect from state '{}'",
                                         nameOf(m_impl->state)));
    }
    m_impl->state = DeviceState::Connecting;
    m_impl->state = DeviceState::Connected;
    m_impl->streamId = m_impl->bootNonce;
    USN_LOG_INFO(log::cats::kDevice, "fake device connected as '{}'",
                 m_impl->options.identity.name);
    return Status::success();
}

void FakeCaptureDevice::disconnect() {
    if (m_impl->state == DeviceState::Capturing) {
        emitDiagnostic(ErrorCode::UsbDisconnected,
                       "device disconnected while capturing; in-flight samples are lost");
    }
    m_impl->state = DeviceState::Disconnected;
    m_impl->sink = nullptr;
}

DeviceState FakeCaptureDevice::state() const noexcept { return m_impl->state; }

Status FakeCaptureDevice::healthCheck() {
    if (m_impl->state == DeviceState::Disconnected) {
        return Status::error(ErrorCode::DeviceNotConnected, "device is not connected");
    }
    return Status::success();
}

DeviceCounters FakeCaptureDevice::counters() const { return m_impl->counters; }

Status FakeCaptureDevice::configure(const CaptureConfiguration& config) {
    if (m_impl->state != DeviceState::Connected && m_impl->state != DeviceState::Armed) {
        return Status::error(ErrorCode::ConfigurationError,
                             fmt::format("cannot configure from state '{}'", nameOf(m_impl->state)));
    }
    auto caps = queryCapabilities();
    if (!caps.ok()) {
        return caps.status();
    }
    // Validated here, on the host, before anything is sent to a device.
    auto const valid = validateCaptureConfiguration(config, *caps);
    if (!valid.ok()) {
        emitDiagnostic(valid.code(), valid.message());
        return valid;
    }
    m_impl->state = DeviceState::Configuring;
    m_impl->config = config;
    m_impl->state = DeviceState::Connected;
    return Status::success();
}

CaptureConfiguration FakeCaptureDevice::configuration() const { return m_impl->config; }

Status FakeCaptureDevice::arm() {
    if (m_impl->config.channelCount == 0) {
        return Status::error(ErrorCode::CaptureNotArmed, "device has not been configured");
    }
    if (m_impl->config.preTriggerSamples > 0 || m_impl->config.singleShot) {
        auto const armed = m_impl->trigger.arm();
        if (!armed.ok()) {
            // Only an error if a trigger was actually compiled; otherwise arming is
            // a no-op and continuous capture proceeds.
            if (m_impl->trigger.compiled() != nullptr) {
                return armed;
            }
        }
    }
    m_impl->state = DeviceState::Armed;
    return Status::success();
}

Status FakeCaptureDevice::start() {
    if (!canStartCapture(m_impl->state)) {
        return Status::error(ErrorCode::CaptureAlreadyRunning,
                             fmt::format("cannot start capture from state '{}'",
                                         nameOf(m_impl->state)));
    }
    if (m_impl->sink == nullptr) {
        return Status::error(ErrorCode::ConfigurationError,
                             "no sample sink attached; captured data would be discarded");
    }
    if (m_impl->config.channelCount == 0) {
        return Status::error(ErrorCode::ConfigurationError, "device has not been configured");
    }
    m_impl->state = DeviceState::Capturing;
    m_impl->progress.state = DeviceState::Capturing;
    return Status::success();
}

Status FakeCaptureDevice::stop() {
    if (m_impl->state != DeviceState::Capturing) {
        return Status::error(ErrorCode::ConfigurationError,
                             fmt::format("cannot stop from state '{}'", nameOf(m_impl->state)));
    }
    // Distinct from Disconnected/Stopped so "capture ended, data still in flight"
    // is observable and testable.
    m_impl->state = DeviceState::Flushing;
    m_impl->progress.state = DeviceState::Flushing;
    m_impl->state = DeviceState::Connected;
    m_impl->progress.state = DeviceState::Connected;
    if (auto const disarmed = m_impl->trigger.disarm(); !disarmed.ok()) {
        emitDiagnostic(disarmed.code(), disarmed.message());
    }
    return Status::success();
}

Status FakeCaptureDevice::abort() {
    if (m_impl->state == DeviceState::Capturing) {
        emitDiagnostic(ErrorCode::Cancelled,
                       fmt::format("capture aborted; {} samples already delivered, any buffered "
                                   "samples are discarded",
                                   m_impl->counters.samplesReceived));
    }
    m_impl->state = DeviceState::Connected;
    m_impl->progress.state = DeviceState::Connected;
    if (auto const disarmed = m_impl->trigger.disarm(); !disarmed.ok()) {
        emitDiagnostic(disarmed.code(), disarmed.message());
    }
    return Status::success();
}

void FakeCaptureDevice::setSampleSink(ISampleSink* const sink) { m_impl->sink = sink; }

CaptureProgress FakeCaptureDevice::progress() const {
    auto p = m_impl->progress;
    p.state = m_impl->state;
    p.samplesCaptured = m_impl->counters.samplesReceived;
    p.bytesTransferred = m_impl->counters.bytesReceived;
    p.droppedBlocks = m_impl->counters.hostOverflows;
    p.crcErrors = m_impl->counters.crcErrors;
    p.triggerFired = m_impl->trigger.hasFired();
    if (p.triggerFired) {
        p.triggerPosition = *m_impl->trigger.firedAt();
    }
    return p;
}

ITriggerEngine* FakeCaptureDevice::triggerEngine() noexcept { return &m_impl->trigger; }

Status FakeCaptureDevice::runSelfTest(std::uint16_t const testId) {
    if (!m_impl->options.capabilities.selfTestSupported) {
        return Status::error(ErrorCode::DeviceUnsupported,
                             fmt::format("device does not support self test {}", testId));
    }
    return Status::success();
}

Status FakeCaptureDevice::generateTestPattern(std::uint16_t const patternId,
                                              std::uint64_t const rateHz,
                                              std::uint32_t const durationMs) {
    if (!m_impl->options.capabilities.patternGeneratorSupported) {
        return Status::error(ErrorCode::DeviceUnsupported,
                             fmt::format("device has no pattern generator (pattern {})", patternId));
    }
    if (rateHz == 0) {
        return Status::error(ErrorCode::SampleRateUnsupported, "pattern rate is zero");
    }
    USN_LOG_INFO(log::cats::kDevice, "fake pattern {} at {} Hz for {} ms", patternId, rateHz,
                 durationMs);
    return Status::success();
}

void FakeCaptureDevice::injectDeviceOverflow(std::uint32_t const blocks) {
    m_impl->overflowBlocksRemaining = blocks;
}

void FakeCaptureDevice::injectSequenceGap(std::uint32_t const skip) {
    m_impl->sequenceGapPending = skip;
}

void FakeCaptureDevice::injectSampleIndexGap(std::uint64_t const skip) {
    m_impl->sampleIndexGapPending = skip;
}

void FakeCaptureDevice::injectDeviceReset() {
    m_impl->bootNonce += 0x9E37'79B9'7F4A'7C15ULL;  // arbitrary, distinct
    m_impl->streamId = m_impl->bootNonce;
    m_impl->sequence = 0;
    m_impl->counters.deviceResets += 1;
    emitDiagnostic(ErrorCode::DeviceResetDetected,
                   fmt::format("simulated device reset; new streamId {}", m_impl->streamId));
}

void FakeCaptureDevice::setCapabilitiesOverride(DeviceCapabilities caps) {
    m_impl->capsOverride = std::move(caps);
}

void FakeCaptureDevice::setFailsCapabilityQuery(bool const fails) noexcept {
    m_impl->failsCapabilityQuery = fails;
}

std::uint64_t FakeCaptureDevice::currentStreamId() const noexcept { return m_impl->streamId; }
std::uint32_t FakeCaptureDevice::currentSequence() const noexcept { return m_impl->sequence; }
SampleIndex FakeCaptureDevice::nextSampleIndex() const noexcept { return m_impl->nextSample; }

std::uint32_t FakeCaptureDevice::sampleWordAt(std::uint64_t const index) const noexcept {
    using Pattern = FakeDeviceOptions::Pattern;
    switch (m_impl->options.pattern) {
    case Pattern::Zeros:
        return 0;
    case Pattern::Counter:
        return static_cast<std::uint32_t>(index & 0xFFFFFFFFULL);
    case Pattern::Toggle:
        return static_cast<std::uint32_t>(index & 1ULL) ? 0xFFFFFFFFU : 0x00000000U;
    case Pattern::SpiLike: {
        // CS low during a 8-clock burst every 32 samples; clock on bit 0.
        std::uint32_t const phase = static_cast<std::uint32_t>(index % 32U);
        std::uint32_t cs = (phase < 16U) ? 0U : 1U;             // bit 3, active low
        std::uint32_t clk = (phase % 2U == 0U) ? 1U : 0U;        // bit 0
        std::uint32_t mosi = ((phase / 2U) & 1U) << 1;           // bit 1
        return (cs << 3) | (mosi) | clk;
    }
    case Pattern::I2cLike: {
        // SCL bit 0, SDA bit 1; idle high with periodic start/stop.
        std::uint32_t const phase = static_cast<std::uint32_t>(index % 20U);
        std::uint32_t scl = (phase % 2U == 0U) ? 1U : 0U;
        std::uint32_t sda = 1U;
        if (phase == 0U) {
            sda = 0U;  // START: SDA falls while SCL high
        } else if (phase == 19U) {
            sda = 0U;  // STOP approach
        }
        return (sda << 1) | scl;
    }
    case Pattern::UartLike: {
        // 10-bit frames: start(0) 8 data bits stop(1), idle high.
        std::uint32_t const phase = static_cast<std::uint32_t>(index % 20U);
        if (phase >= 10U) {
            return 1U;  // idle
        }
        if (phase == 0U) {
            return 0U;  // start bit
        }
        if (phase == 9U) {
            return 1U;  // stop bit
        }
        return ((0xA5U >> (phase - 1U)) & 1U);
    }
    }
    return 0;
}

void FakeCaptureDevice::emitDiagnostic(ErrorCode const code, std::string_view const message) {
    auto event = diagnosticFromStatus(Status::error(code, std::string(message)));
    event.streamId = m_impl->streamId;
    event.atSample = m_impl->nextSample;
    m_impl->emitted.push_back(event);
    if (m_impl->sink != nullptr) {
        m_impl->sink->onDiagnostic(std::move(event));
    }
    USN_LOG_WARN(log::cats::kDevice, "fake device diagnostic: {}", message);
}

std::size_t FakeCaptureDevice::pump(std::size_t const count) {
    if (m_impl->state != DeviceState::Capturing) {
        return 0;
    }
    if (m_impl->sink == nullptr) {
        return 0;
    }
    std::size_t accepted = 0;
    auto const stride = strideForChannelCount(m_impl->config.channelCount);
    if (stride == SampleStride::Invalid) {
        emitDiagnostic(ErrorCode::ChannelCountUnsupported,
                       fmt::format("cannot produce samples for {} channels",
                                   m_impl->config.channelCount));
        return 0;
    }
    auto const strideBytes = static_cast<std::uint8_t>(stride);

    for (std::size_t b = 0; b < count; ++b) {
        if (m_impl->sequenceGapPending > 0) {
            m_impl->sequence += m_impl->sequenceGapPending;
            m_impl->sequenceGapPending = 0;
        }
        if (m_impl->sampleIndexGapPending > 0) {
            m_impl->nextSample =
                SampleIndex(m_impl->nextSample.value + m_impl->sampleIndexGapPending);
            m_impl->sampleIndexGapPending = 0;
        }

        BlockHeader header;
        header.firstSampleIndex = m_impl->nextSample;
        header.firstTick = DeviceTick(m_impl->deviceTick);
        header.sequence = m_impl->sequence;
        header.streamId = m_impl->streamId;
        header.sampleCount = m_impl->options.samplesPerBlock;
        header.channelCount = m_impl->config.channelCount;
        header.strideBytes = strideBytes;
        header.channelMask = m_impl->config.channelMask;
        header.sampleRateHz = m_impl->config.sampleRateHz;
        header.flags = BlockFlag::None;

        if (m_impl->overflowBlocksRemaining > 0) {
            header.flags = header.flags | BlockFlag::OverflowBefore;
            --m_impl->overflowBlocksRemaining;
            m_impl->counters.deviceOverflows += 1;
            emitDiagnostic(ErrorCode::DmaOverflow,
                           "simulated DMA overflow: samples were lost on the device before this "
                           "block");
        }

        std::vector<std::byte> payload;
        payload.resize(static_cast<std::size_t>(header.sampleCount) * strideBytes);
        for (std::uint32_t i = 0; i < header.sampleCount; ++i) {
            std::uint32_t const word =
                sampleWordAt(m_impl->nextSample.value + i) & m_impl->config.channelMask;
            auto const offset = static_cast<std::size_t>(i) * strideBytes;
            for (std::uint8_t s = 0; s < strideBytes; ++s) {
                payload[offset + s] = static_cast<std::byte>((word >> (8 * s)) & 0xFFU);
            }
        }

        OwningSampleBlock block(header, std::move(payload));
        auto const bp = m_impl->sink->onBlock(std::move(block));

        m_impl->nextSample = SampleIndex(m_impl->nextSample.value + header.sampleCount);
        m_impl->deviceTick += header.sampleCount;
        ++m_impl->sequence;
        m_impl->counters.blocksReceived += 1;
        m_impl->counters.samplesReceived += header.sampleCount;
        m_impl->counters.bytesReceived +=
            USN_WIRE_HEADER_SIZE + USN_WIRE_SIZE_PREFIX +
            static_cast<std::uint64_t>(header.sampleCount) * strideBytes;

        if (bp == Backpressure::RejectedStopCapture) {
            m_impl->counters.hostOverflows += 1;
            emitDiagnostic(ErrorCode::HostQueueOverflow,
                           "host sink rejected a block and asked the capture to stop");
            m_impl->state = DeviceState::Error;
            m_impl->progress.state = DeviceState::Error;
            break;
        }
        ++accepted;
    }
    return accepted;
}

}  // namespace usn::hal
