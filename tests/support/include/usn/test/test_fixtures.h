// -----------------------------------------------------------------------------
// test_fixtures.h -- shared builders for tests.
//
// Every fixture here produces values that are internally consistent (a capability
// set that actually supports the configuration built alongside it). Tests that need
// an INCONSISTENT pair say so explicitly at the call site, because "the device
// reports X but we asked for Y" is the interesting case, not the default one.
// -----------------------------------------------------------------------------
#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "usn_wire.h"

#include "usn/core/capture_session.h"
#include "usn/hal/fake_capture_device.h"
#include "usn/hal/idevice.h"
#include "usn/model/capabilities.h"
#include "usn/model/sample.h"

namespace usn::test {

// A capability set shaped like a Teensy 4.1 with PSRAM fitted, but with every
// number marked `measured = false` unless a comment says otherwise. Nothing here
// claims a rate that has been characterised on real hardware, because none has.
[[nodiscard]] DeviceCapabilities makeTeensyLikeCapabilities();

// A capability set with an EMPTY rate envelope: the "we do not know yet" state a
// freshly flashed device reports. Tests use it to prove the host refuses to guess.
[[nodiscard]] DeviceCapabilities makeUncharacterisedCapabilities();

[[nodiscard]] hal::DeviceIdentity makeIdentity(std::string endpoint = "loopback");

[[nodiscard]] hal::FakeDeviceOptions makeFakeDeviceOptions(
    std::uint32_t samplesPerBlock = 256,
    hal::FakeDeviceOptions::Pattern pattern = hal::FakeDeviceOptions::Pattern::Counter);

// A configuration the capabilities above genuinely support: 16 channels, 2-byte
// stride, 1 MHz.
[[nodiscard]] CaptureConfiguration makeSupportedCaptureConfiguration();

// Builds a packed sample block whose payload is a counter pattern, so lost,
// duplicated or reordered samples are visible by inspection.
[[nodiscard]] OwningSampleBlock makeCounterBlock(SampleIndex first, std::uint32_t sampleCount,
                                                 std::uint8_t strideBytes = 2,
                                                 std::uint16_t channelCount = 16,
                                                 std::uint64_t sampleRateHz = 1'000'000);

// A sink that records what it receives and can be gated, so tests can put a lane
// into a known "consumer is not keeping up" state deterministically rather than by
// racing a sleep.
class GatedSink final : public hal::ISampleSink {
public:
    GatedSink() = default;

    hal::Backpressure onBlock(OwningSampleBlock block) final;
    void onDiagnostic(DiagnosticEvent event) final;

    // Blocks the consumer until release() is called.
    void closeGate();
    void release();

    // When set, the sink answers with RejectedStopCapture instead of consuming.
    void setRejects(bool rejects) noexcept;

    [[nodiscard]] std::uint64_t received() const noexcept;
    [[nodiscard]] std::uint64_t firstSampleSum() const noexcept;
    [[nodiscard]] std::vector<SampleIndex> receivedIndices() const;
    [[nodiscard]] std::uint64_t diagnostics() const noexcept;

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_gateClosed{false};
    bool m_rejects{false};
    std::uint64_t m_received{0};
    std::uint64_t m_firstSampleSum{0};
    std::uint64_t m_diagnostics{0};
    std::vector<SampleIndex> m_indices;
};

}  // namespace usn::test
