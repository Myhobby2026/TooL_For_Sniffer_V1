// -----------------------------------------------------------------------------
// idevice.h -- device abstraction (docs/architecture_review.md section 4.6,
// master spec sections 9, 10, 40).
//
// The rest of the application depends on these interfaces, never on a
// Teensy-specific implementation. TeensyDevice is one implementation;
// FakeCaptureDevice is another, and it is the one the tests actually use.
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "usn/common/status.h"
#include "usn/model/capabilities.h"
#include "usn/model/diagnostics.h"
#include "usn/model/sample.h"

namespace usn::hal {

struct DeviceIdentity {
    std::string name;
    std::string serial;
    std::string endpoint;          // "COM7", "/dev/ttyACM0", "loopback"
    std::string firmwareVersion;
    std::string hardwareRevision;
    std::uint64_t bootNonce{0};    // changes on every device boot => reset detection
};

enum class DeviceState : std::uint8_t {
    Disconnected = 0,
    Connecting,
    Connected,
    Configuring,
    Armed,
    Capturing,
    Flushing,
    Stopping,
    Error
};

[[nodiscard]] std::string_view nameOf(DeviceState state) noexcept;
[[nodiscard]] bool isTerminal(DeviceState state) noexcept;
[[nodiscard]] bool canStartCapture(DeviceState state) noexcept;

// Cumulative counters. Every one of these must be visible in the Diagnostics
// panel: master spec section 14 forbids silently discarding data, and a counter
// that nobody can see is a silent discard with extra steps.
struct DeviceCounters {
    std::uint64_t blocksReceived{0};
    std::uint64_t samplesReceived{0};
    std::uint64_t bytesReceived{0};
    std::uint64_t deviceOverflows{0};      // DMA / ring overflow reported by the device
    std::uint64_t hostOverflows{0};        // host queue full
    std::uint64_t crcErrors{0};
    std::uint64_t sequenceGaps{0};
    std::uint64_t sampleIndexGaps{0};
    std::uint64_t resyncEvents{0};
    std::uint64_t deviceResets{0};
    std::uint64_t reconnectAttempts{0};
};

// Backpressure answer from a sink. The device acts on it rather than guessing.
enum class Backpressure : std::uint8_t {
    Accepted = 0,
    AcceptedWithWarning,        // queue above its high-water mark
    RejectedStopCapture         // sink cannot keep up; the capture must stop
};

[[nodiscard]] std::string_view nameOf(Backpressure bp) noexcept;

class ISampleSink {
public:
    ISampleSink() = default;
    ISampleSink(const ISampleSink&) = delete;
    ISampleSink& operator=(const ISampleSink&) = delete;
    ISampleSink(ISampleSink&&) = delete;
    ISampleSink& operator=(ISampleSink&&) = delete;
    virtual ~ISampleSink() = default;

    // Takes ownership of the block by move. Must not block longer than the
    // pipeline's budget; the return value tells the producer what to do.
    virtual Backpressure onBlock(OwningSampleBlock block) = 0;

    // Every anomaly surfaces here. Implementations must aggregate rather than
    // flood, but must never drop.
    virtual void onDiagnostic(DiagnosticEvent event) = 0;
};

// Live progress, for the Capture Control panel.
struct CaptureProgress {
    DeviceState state{DeviceState::Disconnected};
    std::uint64_t samplesCaptured{0};
    std::uint64_t bytesTransferred{0};
    std::uint64_t measuredBytesPerSec{0};   // 0 == not yet measured, never a guess
    std::uint64_t measuredSamplesPerSec{0};
    std::uint64_t droppedBlocks{0};
    std::uint64_t crcErrors{0};
    bool triggerFired{false};
    SampleIndex triggerPosition{};
};

class IDevice {
public:
    IDevice() = default;
    IDevice(const IDevice&) = delete;
    IDevice& operator=(const IDevice&) = delete;
    IDevice(IDevice&&) = delete;
    IDevice& operator=(IDevice&&) = delete;
    virtual ~IDevice() = default;

    [[nodiscard]] virtual DeviceIdentity identity() const = 0;

    // ASK the device. Never assume. A device that cannot answer returns an error,
    // and the caller must present "unknown" rather than a default.
    [[nodiscard]] virtual StatusOr<DeviceCapabilities> queryCapabilities() = 0;

    virtual Status connect() = 0;
    virtual void disconnect() = 0;
    [[nodiscard]] virtual DeviceState state() const noexcept = 0;

    // Ping + version + counter sanity. Cheap enough to poll.
    [[nodiscard]] virtual Status healthCheck() = 0;
    [[nodiscard]] virtual DeviceCounters counters() const = 0;
};

}  // namespace usn::hal
