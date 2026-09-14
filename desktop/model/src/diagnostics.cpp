// -----------------------------------------------------------------------------
// diagnostics.cpp -- see diagnostics.h.
// -----------------------------------------------------------------------------
#include "usn/model/diagnostics.h"

#include <fmt/format.h>

namespace usn {

std::string_view nameOf(DiagnosticSeverity const severity) noexcept {
    switch (severity) {
    case DiagnosticSeverity::Info:     return "info";
    case DiagnosticSeverity::Warning:  return "warning";
    case DiagnosticSeverity::Error:    return "error";
    case DiagnosticSeverity::Critical: return "critical";
    }
    return "unknown";
}

std::string_view nameOf(DiagnosticCategory const category) noexcept {
    switch (category) {
    case DiagnosticCategory::Application: return "application";
    case DiagnosticCategory::Device:      return "device";
    case DiagnosticCategory::Usb:         return "usb";
    case DiagnosticCategory::Capture:     return "capture";
    case DiagnosticCategory::Buffer:      return "buffer";
    case DiagnosticCategory::Decoder:     return "decoder";
    case DiagnosticCategory::Storage:     return "storage";
    case DiagnosticCategory::Host:        return "host";
    case DiagnosticCategory::Plugin:      return "plugin";
    case DiagnosticCategory::Script:      return "script";
    }
    return "unknown";
}

DiagnosticSeverity severityOf(ErrorSeverity const severity) noexcept {
    switch (severity) {
    case ErrorSeverity::Info:     return DiagnosticSeverity::Info;
    case ErrorSeverity::Warning:  return DiagnosticSeverity::Warning;
    case ErrorSeverity::Error:    return DiagnosticSeverity::Error;
    case ErrorSeverity::Critical: return DiagnosticSeverity::Critical;
    }
    return DiagnosticSeverity::Error;
}

DiagnosticCategory categoryForCode(ErrorCode const code) noexcept {
    switch (code) {
    case ErrorCode::DeviceNotFound:
    case ErrorCode::DeviceBusy:
    case ErrorCode::DeviceUnsupported:
    case ErrorCode::DeviceResetDetected:
    case ErrorCode::DeviceFirmwareIncompatible:
    case ErrorCode::DeviceConfigurationRejected:
    case ErrorCode::DeviceNotConnected:
        return DiagnosticCategory::Device;

    case ErrorCode::UsbOpenFailed:
    case ErrorCode::UsbReadFailed:
    case ErrorCode::UsbWriteFailed:
    case ErrorCode::UsbTimeout:
    case ErrorCode::UsbDisconnected:
        return DiagnosticCategory::Usb;

    case ErrorCode::DmaOverflow:
    case ErrorCode::RingBufferOverflow:
    case ErrorCode::HostQueueOverflow:
        return DiagnosticCategory::Buffer;

    case ErrorCode::CrcMismatch:
    case ErrorCode::InvalidMagic:
    case ErrorCode::InvalidPacketLength:
    case ErrorCode::HeaderVersionUnsupported:
    case ErrorCode::SequenceGap:
    case ErrorCode::SampleIndexGap:
    case ErrorCode::StreamIdMismatch:
    case ErrorCode::FramingResync:
    case ErrorCode::InvalidPacketType:
    case ErrorCode::ReservedFlagSet:
        return DiagnosticCategory::Usb;

    case ErrorCode::CaptureNotArmed:
    case ErrorCode::CaptureAlreadyRunning:
    case ErrorCode::SampleRateUnsupported:
    case ErrorCode::ChannelCountUnsupported:
    case ErrorCode::StrideUnsupported:
    case ErrorCode::TriggerUnsupported:
        return DiagnosticCategory::Capture;

    case ErrorCode::DecoderNotFound:
    case ErrorCode::DecoderConfigurationInvalid:
    case ErrorCode::DecoderInternalError:
    case ErrorCode::DecoderAlreadyRegistered:
    case ErrorCode::ProtocolFramingError:
    case ErrorCode::ProtocolChecksumError:
    case ErrorCode::ProtocolTimeout:
        return DiagnosticCategory::Decoder;

    case ErrorCode::FileOpenFailed:
    case ErrorCode::FileReadFailed:
    case ErrorCode::FileWriteFailed:
    case ErrorCode::FileFormatUnsupported:
    case ErrorCode::FileCorrupt:
    case ErrorCode::FileTruncated:
    case ErrorCode::FileVersionUnsupported:
    case ErrorCode::IndexRebuilt:
        return DiagnosticCategory::Storage;

    case ErrorCode::PluginLoadFailed:
    case ErrorCode::PluginApiMismatch:
        return DiagnosticCategory::Plugin;

    case ErrorCode::ScriptError:
    case ErrorCode::ScriptSandboxViolation:
        return DiagnosticCategory::Script;

    default:
        return DiagnosticCategory::Application;
    }
}

std::string DiagnosticEvent::toString() const {
    std::string out = fmt::format("[{}] {}: {} (x{})", nameOf(severity), nameOf(category),
                                  message, count);
    if (atSample.has_value()) {
        out += fmt::format(" @sample={}", atSample->value);
    }
    if (streamId.has_value()) {
        out += fmt::format(" stream={}", *streamId);
    }
    return out;
}

bool DiagnosticEvent::sameConditionAs(const DiagnosticEvent& other) const noexcept {
    return code == other.code && severity == other.severity && category == other.category &&
           message == other.message;
}

void DiagnosticEvent::mergeFrom(const DiagnosticEvent& other) noexcept {
    count += other.count;
    if (firstSeenMonotonicNs == 0 || other.firstSeenMonotonicNs < firstSeenMonotonicNs) {
        firstSeenMonotonicNs = other.firstSeenMonotonicNs;
    }
    if (other.lastSeenMonotonicNs > lastSeenMonotonicNs) {
        lastSeenMonotonicNs = other.lastSeenMonotonicNs;
    }
    if (!other.atSample.has_value()) {
        return;
    }
    if (!atSample.has_value()) {
        atSample = other.atSample;
        return;
    }
    // Keep the most recent occurrence; the count already records the magnitude.
    if (other.atSample->value > atSample->value) {
        atSample = other.atSample;
    }
}

DiagnosticEvent diagnosticFromStatus(const Status& status) {
    DiagnosticEvent event;
    event.severity = severityOf(status.severity());
    event.category = categoryForCode(status.code());
    event.code = status.code();
    event.message = std::string(status.message());
    event.atSample = status.sampleIndex().has_value()
                         ? std::optional<SampleIndex>(SampleIndex(*status.sampleIndex()))
                         : std::nullopt;
    event.streamId = status.streamId();
    event.context = status.context();
    return event;
}

}  // namespace usn
