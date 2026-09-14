// -----------------------------------------------------------------------------
// capabilities.h -- the device capability model (master spec section 10).
//
// The host ASKS the device what it supports. Nothing here may be hard-coded from
// an assumption about a particular board, because the whole point of the model is
// that the GUI never offers something the hardware cannot honour.
//
// rateEnvelope is a LIST of {channelCount, maxSampleRateHz} points rather than a
// single number, because on Teensy 4.1 channel count and maximum rate are coupled
// through pin selection: only some GPIO modules are DMA-reachable, and a capture
// spanning two modules cannot use a single 32-bit register read
// (docs/architecture_review.md section 16.2, L2).
//
// Every value in a populated DeviceCapabilities is either device-reported or
// measured. A field the device could not report is left at its "unknown" sentinel
// and MUST NOT be presented to the user as a number.
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "usn/common/status.h"

namespace usn {

enum class UsbSpeedClass : std::uint8_t {
    Unknown = 0,
    LowSpeed,       // 1.5 Mbit/s
    FullSpeed,      // 12 Mbit/s
    HighSpeed,      // 480 Mbit/s
    SuperSpeed      // 5 Gbit/s
};

[[nodiscard]] std::string_view nameOf(UsbSpeedClass speed) noexcept;

// Trigger features the device can execute locally. Host-only triggers (protocol
// field match, search expressions) are NOT listed here; see the trigger layer's
// classify() which combines these flags with the AST node kind.
enum class TriggerCapability : std::uint32_t {
    None = 0,
    RisingEdge = 1U << 0,
    FallingEdge = 1U << 1,
    AnyEdge = 1U << 2,
    Level = 1U << 3,
    DigitalPattern = 1U << 4,
    PulseWidth = 1U << 5,
    Timeout = 1U << 6,
    ByteSequence = 1U << 7,
    Count = 1U << 8,
    Sequential = 1U << 9,
    And = 1U << 10,
    Or = 1U << 11,
    Not = 1U << 12,
    PreTrigger = 1U << 13,
    PostTrigger = 1U << 14,
    SingleShot = 1U << 15,
    Continuous = 1U << 16,
    ReArm = 1U << 17
};

[[nodiscard]] constexpr TriggerCapability operator|(TriggerCapability a,
                                                    TriggerCapability b) noexcept {
    return static_cast<TriggerCapability>(static_cast<std::uint32_t>(a) |
                                          static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr TriggerCapability operator&(TriggerCapability a,
                                                    TriggerCapability b) noexcept {
    return static_cast<TriggerCapability>(static_cast<std::uint32_t>(a) &
                                          static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr bool hasCapability(TriggerCapability set, TriggerCapability flag) noexcept {
    return flag != TriggerCapability::None && (set & flag) == flag;
}

struct ChannelCountRatePoint {
    std::uint16_t channelCount{0};
    std::uint64_t maxSampleRateHz{0};
    bool measured{false};   // false => vendor claim only, must be labelled as such
};

struct DeviceCapabilities {
    // --- identity ---
    std::string deviceName;
    std::string deviceSerial;
    std::string firmwareVersion;
    std::string hardwareRevision;
    std::uint32_t wireProtocolVersion{0};

    // --- digital capture ---
    std::uint16_t channelCountMax{0};
    std::vector<ChannelCountRatePoint> rateEnvelope;

    // --- analog (future; 0 means not available, and the UI must say so rather
    //     than implying that GPIO capture can measure rise/fall times) ---
    std::uint16_t adcChannels{0};
    std::uint16_t adcResolutionBits{0};
    std::uint64_t adcMaxSampleRateHz{0};

    // --- buffering: the real limit on burst depth (section 16.2 L3) ---
    std::uint64_t onboardBufferBytes{0};
    std::uint64_t psramBytes{0};   // 0 when the optional PSRAM is not fitted

    // --- transport ---
    UsbSpeedClass usbSpeed{UsbSpeedClass::Unknown};
    std::uint64_t measuredLinkBytesPerSec{0};   // 0 == not yet measured

    // --- features ---
    TriggerCapability supportedTriggers{TriggerCapability::None};
    std::vector<std::string> deviceSideProtocols;  // device pre-processing only
    bool rleSupported{false};
    bool deviceTriggerSupported{false};
    bool selfTestSupported{false};
    bool patternGeneratorSupported{false};

    // Largest rate the device will accept for a given channel count, or 0 when
    // the request is outside the envelope. This is what the GUI calls instead of
    // comparing against a hard-coded constant.
    [[nodiscard]] std::uint64_t maxRateForChannels(std::uint16_t channelCount) const noexcept;

    // Whether a (channelCount, rate) pair is inside the envelope.
    [[nodiscard]] bool supports(std::uint16_t channelCount, std::uint64_t rateHz) const noexcept;

    [[nodiscard]] bool hasAnyRateData() const noexcept { return !rateEnvelope.empty(); }
};

// Validates a requested configuration against reported capabilities and returns
// a precise Status when it is outside the envelope. Used by hal before anything
// is sent to the device, so the user gets "32 channels at 50 MSPS exceeds the
// reported envelope (max 5 MSPS)" rather than a silent clamp or a device NAK.
struct CaptureConfiguration {
    std::uint16_t channelCount{0};
    std::uint64_t sampleRateHz{0};
    std::uint64_t channelMask{0};
    std::uint64_t requestedBufferBytes{0};
    std::uint32_t preTriggerSamples{0};
    std::uint32_t postTriggerSamples{0};   // 0 == unlimited (continuous)
    bool enableRle{false};
    bool singleShot{false};
};

[[nodiscard]] Status validateCaptureConfiguration(const CaptureConfiguration& config,
                                                  const DeviceCapabilities& caps);

}  // namespace usn
