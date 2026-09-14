// -----------------------------------------------------------------------------
// capabilities.cpp -- see capabilities.h.
// -----------------------------------------------------------------------------
#include "usn/model/capabilities.h"

#include <fmt/format.h>

namespace usn {

std::string_view nameOf(UsbSpeedClass const speed) noexcept {
    switch (speed) {
    case UsbSpeedClass::Unknown:    return "unknown";
    case UsbSpeedClass::LowSpeed:   return "low-speed (1.5 Mbit/s)";
    case UsbSpeedClass::FullSpeed:  return "full-speed (12 Mbit/s)";
    case UsbSpeedClass::HighSpeed:  return "high-speed (480 Mbit/s)";
    case UsbSpeedClass::SuperSpeed: return "super-speed (5 Gbit/s)";
    }
    return "unknown";
}

std::uint64_t DeviceCapabilities::maxRateForChannels(std::uint16_t const channelCount) const noexcept {
    std::uint64_t best = 0;
    for (const auto& point : rateEnvelope) {
        // A point measured at N channels also certifies any smaller count at that
        // rate, because fewer channels never costs more bandwidth per sample than
        // the next stride boundary. Take the best rate among points whose channel
        // count covers the request.
        if (point.channelCount >= channelCount && point.maxSampleRateHz > best) {
            best = point.maxSampleRateHz;
        }
    }
    return best;
}

bool DeviceCapabilities::supports(std::uint16_t const channelCount,
                                  std::uint64_t const rateHz) const noexcept {
    if (channelCount == 0 || channelCount > channelCountMax) {
        return false;
    }
    auto const limit = maxRateForChannels(channelCount);
    return limit != 0 && rateHz <= limit;
}

Status validateCaptureConfiguration(const CaptureConfiguration& config,
                                    const DeviceCapabilities& caps) {
    if (config.channelCount == 0) {
        return Status::error(ErrorCode::ConfigurationError, "no channels requested");
    }
    if (config.channelCount > caps.channelCountMax) {
        return Status::error(
                   ErrorCode::ChannelCountUnsupported,
                   fmt::format("requested {} channels, device reports a maximum of {}",
                               config.channelCount, caps.channelCountMax))
            .withContext("device", caps.deviceName);
    }
    if (config.sampleRateHz == 0) {
        return Status::error(ErrorCode::SampleRateUnsupported, "requested sample rate is zero");
    }
    if (!caps.hasAnyRateData()) {
        // Deliberately an error rather than a permissive pass. A device that has
        // not reported a rate envelope has not been characterised, and the honest
        // answer is "we do not know if this works" (section 16.3).
        return Status::error(
            ErrorCode::SampleRateUnsupported,
            "device reported no rate envelope; capture rate has not been characterised")
            .withContext("device", caps.deviceName)
            .withContext("firmware", caps.firmwareVersion);
    }
    auto const limit = caps.maxRateForChannels(config.channelCount);
    if (config.sampleRateHz > limit) {
        return Status::error(
                   ErrorCode::SampleRateUnsupported,
                   fmt::format("{} channels at {} Hz exceeds the reported envelope (max {} Hz)",
                               config.channelCount, config.sampleRateHz, limit))
            .withContext("channels", std::to_string(config.channelCount))
            .withContext("requestedHz", std::to_string(config.sampleRateHz))
            .withContext("maxHz", std::to_string(limit));
    }
    if (config.enableRle && !caps.rleSupported) {
        return Status::error(ErrorCode::DeviceUnsupported,
                             "RLE requested but the device does not support it");
    }
    if (config.preTriggerSamples > 0 && !caps.deviceTriggerSupported) {
        return Status::error(ErrorCode::TriggerUnsupported,
                             "pre-trigger requested but the device has no trigger support");
    }
    if (caps.onboardBufferBytes != 0 &&
        config.requestedBufferBytes > caps.onboardBufferBytes) {
        // Not fatal on its own -- the device will use what it has -- but the caller
        // must know the request was reduced, so report it as a warning-level
        // status the caller can downgrade. Here it is an error with full context.
        return Status::error(
                   ErrorCode::ConfigurationError,
                   fmt::format("requested buffer {} bytes exceeds the {} bytes the device reports",
                               config.requestedBufferBytes, caps.onboardBufferBytes))
            .withContext("psramBytes", std::to_string(caps.psramBytes));
    }
    return Status::success();
}

}  // namespace usn
