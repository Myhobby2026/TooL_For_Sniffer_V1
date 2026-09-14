// -----------------------------------------------------------------------------
// test_fixtures.cpp -- see include/usn/test/test_fixtures.h.
// -----------------------------------------------------------------------------
#include "usn/test/test_fixtures.h"

#include <cstring>
#include <utility>

namespace usn::test {

DeviceCapabilities makeTeensyLikeCapabilities() {
    DeviceCapabilities caps;
    caps.deviceName = "FakeTeensy41";
    caps.deviceSerial = "TEST-0001";
    caps.firmwareVersion = "0.1.0-fake";
    caps.hardwareRevision = "rev-A";
    caps.wireProtocolVersion = USN_WIRE_VERSION;
    caps.channelCountMax = 16;
    // Vendor-claim envelope. `measured` stays false: these numbers come from the
    // device's own report, not from a characterisation run, and the GUI must label
    // them accordingly.
    caps.rateEnvelope = {
        ChannelCountRatePoint{16, 10'000'000, false},
        ChannelCountRatePoint{8, 20'000'000, false},
        ChannelCountRatePoint{4, 40'000'000, false},
        ChannelCountRatePoint{1, 100'000'000, false},
    };
    caps.onboardBufferBytes = 256u * 1024u;   // OCRAM-sized, as reported
    caps.psramBytes = 8u * 1024u * 1024u;     // optional part, fitted here
    caps.usbSpeed = UsbSpeedClass::FullSpeed;
    caps.measuredLinkBytesPerSec = 0;         // not measured: never a made-up number
    caps.supportedTriggers = TriggerCapability::RisingEdge | TriggerCapability::FallingEdge |
                             TriggerCapability::AnyEdge | TriggerCapability::Level;
    caps.rleSupported = false;
    caps.deviceTriggerSupported = true;
    caps.selfTestSupported = true;
    caps.patternGeneratorSupported = true;
    return caps;
}

DeviceCapabilities makeUncharacterisedCapabilities() {
    DeviceCapabilities caps = makeTeensyLikeCapabilities();
    caps.rateEnvelope.clear();                // the honest "we do not know yet" state
    caps.measuredLinkBytesPerSec = 0;
    return caps;
}

hal::DeviceIdentity makeIdentity(std::string const endpoint) {
    hal::DeviceIdentity id;
    id.name = "FakeTeensy41";
    id.serial = "TEST-0001";
    id.endpoint = endpoint;
    id.firmwareVersion = "0.1.0-fake";
    id.hardwareRevision = "rev-A";
    id.bootNonce = 1;
    return id;
}

hal::FakeDeviceOptions makeFakeDeviceOptions(std::uint32_t const samplesPerBlock,
                                             hal::FakeDeviceOptions::Pattern const pattern) {
    hal::FakeDeviceOptions options;
    options.identity = makeIdentity();
    options.capabilities = makeTeensyLikeCapabilities();
    options.samplesPerBlock = samplesPerBlock;
    options.pattern = pattern;
    return options;
}

CaptureConfiguration makeSupportedCaptureConfiguration() {
    CaptureConfiguration config;
    config.channelCount = 16;
    config.sampleRateHz = 1'000'000;
    config.channelMask = 0xFFFFu;
    config.requestedBufferBytes = 64u * 1024u;   // inside the reported onboard buffer
    config.preTriggerSamples = 0;
    config.postTriggerSamples = 0;               // continuous
    config.enableRle = false;
    config.singleShot = false;
    return config;
}

OwningSampleBlock makeCounterBlock(SampleIndex const first, std::uint32_t const sampleCount,
                                   std::uint8_t const strideBytes,
                                   std::uint16_t const channelCount,
                                   std::uint64_t const sampleRateHz) {
    BlockHeader header;
    header.firstSampleIndex = first;
    header.firstTick = DeviceTick(first.value);
    header.sequence = 0;
    header.streamId = 1;
    header.sampleCount = sampleCount;
    header.channelCount = channelCount;
    header.strideBytes = strideBytes;
    header.flags = BlockFlag::None;
    header.channelMask = channelCount >= 64 ? ~std::uint64_t{0}
                                            : (std::uint64_t{1} << channelCount) - 1;
    header.sampleRateHz = sampleRateHz;

    std::vector<std::byte> payload(static_cast<std::size_t>(sampleCount) * strideBytes);
    for (std::uint32_t i = 0; i < sampleCount; ++i) {
        // A counter that wraps at the stride width, so a dropped or duplicated
        // sample breaks the sequence visibly.
        auto const value = static_cast<std::uint32_t>(first.value + i);
        auto* dst = payload.data() + static_cast<std::size_t>(i) * strideBytes;
        switch (strideBytes) {
        case 1:
            dst[0] = static_cast<std::byte>(value & 0xFFu);
            break;
        case 2:
            dst[0] = static_cast<std::byte>(value & 0xFFu);
            dst[1] = static_cast<std::byte>((value >> 8) & 0xFFu);
            break;
        default:
            dst[0] = static_cast<std::byte>(value & 0xFFu);
            dst[1] = static_cast<std::byte>((value >> 8) & 0xFFu);
            dst[2] = static_cast<std::byte>((value >> 16) & 0xFFu);
            dst[3] = static_cast<std::byte>((value >> 24) & 0xFFu);
            break;
        }
    }
    return OwningSampleBlock(header, std::move(payload));
}

hal::Backpressure GatedSink::onBlock(OwningSampleBlock block) {
    {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [this] { return !m_gateClosed; });
        if (m_rejects) {
            return hal::Backpressure::RejectedStopCapture;
        }
        m_received += 1;
        m_firstSampleSum += block.header().firstSampleIndex.value;
        m_indices.push_back(block.header().firstSampleIndex);
    }
    return hal::Backpressure::Accepted;
}

void GatedSink::onDiagnostic(DiagnosticEvent /*event*/) {
    std::scoped_lock const lock(m_mutex);
    m_diagnostics += 1;
}

void GatedSink::closeGate() {
    std::scoped_lock const lock(m_mutex);
    m_gateClosed = true;
}

void GatedSink::release() {
    {
        std::scoped_lock const lock(m_mutex);
        m_gateClosed = false;
    }
    m_cv.notify_all();
}

void GatedSink::setRejects(bool const rejects) noexcept {
    std::scoped_lock const lock(m_mutex);
    m_rejects = rejects;
}

std::uint64_t GatedSink::received() const noexcept {
    std::scoped_lock const lock(m_mutex);
    return m_received;
}

std::uint64_t GatedSink::firstSampleSum() const noexcept {
    std::scoped_lock const lock(m_mutex);
    return m_firstSampleSum;
}

std::vector<SampleIndex> GatedSink::receivedIndices() const {
    std::scoped_lock const lock(m_mutex);
    return m_indices;
}

std::uint64_t GatedSink::diagnostics() const noexcept {
    std::scoped_lock const lock(m_mutex);
    return m_diagnostics;
}

}  // namespace usn::test
