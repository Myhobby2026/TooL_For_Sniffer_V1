// -----------------------------------------------------------------------------
// status.cpp -- ErrorCode name/severity tables.
//
// Generated from the ErrorCode enumerator list in status.h and kept in the same
// order. tests/unit/common_status_test.cpp asserts that EVERY enumerator has a
// non-empty name and a severity, so a newly added code that is forgotten here
// fails the build rather than silently reporting "Unknown".
// -----------------------------------------------------------------------------
#include "usn/common/status.h"

#include <sstream>

namespace usn {
namespace {

struct CodeInfo {
    ErrorCode code;
    std::string_view name;
    ErrorSeverity severity;
};

// Ordered exactly as the enumerator, so index == static_cast<size_t>(code).
constexpr CodeInfo kCodes[] = {
    {ErrorCode::Ok, "Ok", ErrorSeverity::Info},
    {ErrorCode::DeviceNotFound, "DeviceNotFound", ErrorSeverity::Error},
    {ErrorCode::DeviceBusy, "DeviceBusy", ErrorSeverity::Error},
    {ErrorCode::DeviceUnsupported, "DeviceUnsupported", ErrorSeverity::Error},
    {ErrorCode::DeviceResetDetected, "DeviceResetDetected", ErrorSeverity::Critical},
    {ErrorCode::DeviceFirmwareIncompatible, "DeviceFirmwareIncompatible", ErrorSeverity::Error},
    {ErrorCode::DeviceConfigurationRejected, "DeviceConfigurationRejected", ErrorSeverity::Error},
    {ErrorCode::DeviceNotConnected, "DeviceNotConnected", ErrorSeverity::Error},
    {ErrorCode::UsbOpenFailed, "UsbOpenFailed", ErrorSeverity::Error},
    {ErrorCode::UsbReadFailed, "UsbReadFailed", ErrorSeverity::Error},
    {ErrorCode::UsbWriteFailed, "UsbWriteFailed", ErrorSeverity::Error},
    {ErrorCode::UsbTimeout, "UsbTimeout", ErrorSeverity::Error},
    {ErrorCode::UsbDisconnected, "UsbDisconnected", ErrorSeverity::Error},
    {ErrorCode::CrcMismatch, "CrcMismatch", ErrorSeverity::Warning},
    {ErrorCode::InvalidMagic, "InvalidMagic", ErrorSeverity::Error},
    {ErrorCode::InvalidPacketLength, "InvalidPacketLength", ErrorSeverity::Warning},
    {ErrorCode::HeaderVersionUnsupported, "HeaderVersionUnsupported", ErrorSeverity::Warning},
    {ErrorCode::SequenceGap, "SequenceGap", ErrorSeverity::Warning},
    {ErrorCode::SampleIndexGap, "SampleIndexGap", ErrorSeverity::Warning},
    {ErrorCode::StreamIdMismatch, "StreamIdMismatch", ErrorSeverity::Warning},
    {ErrorCode::FramingResync, "FramingResync", ErrorSeverity::Warning},
    {ErrorCode::InvalidPacketType, "InvalidPacketType", ErrorSeverity::Warning},
    {ErrorCode::ReservedFlagSet, "ReservedFlagSet", ErrorSeverity::Warning},
    {ErrorCode::CaptureNotArmed, "CaptureNotArmed", ErrorSeverity::Error},
    {ErrorCode::CaptureAlreadyRunning, "CaptureAlreadyRunning", ErrorSeverity::Error},
    {ErrorCode::DmaOverflow, "DmaOverflow", ErrorSeverity::Warning},
    {ErrorCode::RingBufferOverflow, "RingBufferOverflow", ErrorSeverity::Warning},
    {ErrorCode::HostQueueOverflow, "HostQueueOverflow", ErrorSeverity::Warning},
    {ErrorCode::SampleRateUnsupported, "SampleRateUnsupported", ErrorSeverity::Error},
    {ErrorCode::ChannelCountUnsupported, "ChannelCountUnsupported", ErrorSeverity::Error},
    {ErrorCode::StrideUnsupported, "StrideUnsupported", ErrorSeverity::Error},
    {ErrorCode::TriggerUnsupported, "TriggerUnsupported", ErrorSeverity::Error},
    {ErrorCode::DecoderNotFound, "DecoderNotFound", ErrorSeverity::Error},
    {ErrorCode::DecoderConfigurationInvalid, "DecoderConfigurationInvalid", ErrorSeverity::Error},
    {ErrorCode::DecoderInternalError, "DecoderInternalError", ErrorSeverity::Error},
    {ErrorCode::DecoderAlreadyRegistered, "DecoderAlreadyRegistered", ErrorSeverity::Error},
    {ErrorCode::ProtocolFramingError, "ProtocolFramingError", ErrorSeverity::Error},
    {ErrorCode::ProtocolChecksumError, "ProtocolChecksumError", ErrorSeverity::Error},
    {ErrorCode::ProtocolTimeout, "ProtocolTimeout", ErrorSeverity::Error},
    {ErrorCode::FileOpenFailed, "FileOpenFailed", ErrorSeverity::Error},
    {ErrorCode::FileReadFailed, "FileReadFailed", ErrorSeverity::Error},
    {ErrorCode::FileWriteFailed, "FileWriteFailed", ErrorSeverity::Error},
    {ErrorCode::FileFormatUnsupported, "FileFormatUnsupported", ErrorSeverity::Error},
    {ErrorCode::FileCorrupt, "FileCorrupt", ErrorSeverity::Critical},
    {ErrorCode::FileTruncated, "FileTruncated", ErrorSeverity::Warning},
    {ErrorCode::FileVersionUnsupported, "FileVersionUnsupported", ErrorSeverity::Error},
    {ErrorCode::IndexRebuilt, "IndexRebuilt", ErrorSeverity::Warning},
    {ErrorCode::PluginLoadFailed, "PluginLoadFailed", ErrorSeverity::Error},
    {ErrorCode::PluginApiMismatch, "PluginApiMismatch", ErrorSeverity::Error},
    {ErrorCode::ScriptError, "ScriptError", ErrorSeverity::Error},
    {ErrorCode::ScriptSandboxViolation, "ScriptSandboxViolation", ErrorSeverity::Error},
    {ErrorCode::ConfigurationError, "ConfigurationError", ErrorSeverity::Error},
    {ErrorCode::OutOfMemory, "OutOfMemory", ErrorSeverity::Critical},
    {ErrorCode::Cancelled, "Cancelled", ErrorSeverity::Info},
    {ErrorCode::NotImplemented, "NotImplemented", ErrorSeverity::Info},
    {ErrorCode::Unknown, "Unknown", ErrorSeverity::Error},
};

}  // namespace

std::string_view nameOf(ErrorCode const code) noexcept {
    auto const idx = static_cast<std::size_t>(code);
    if (idx < std::size(kCodes) && kCodes[idx].code == code) {
        return kCodes[idx].name;
    }
    return "InvalidErrorCode";
}

ErrorSeverity severityForCode(ErrorCode const code) noexcept {
    auto const idx = static_cast<std::size_t>(code);
    if (idx < std::size(kCodes) && kCodes[idx].code == code) {
    return kCodes[idx].severity;
    }
    return ErrorSeverity::Error;
}

Status Status::error(ErrorCode const code, std::string message) {
    Status s;
    s.m_code = code;
    s.m_message = std::move(message);
    return s;
}

Status Status::withContext(std::string key, std::string value) const& {
    Status copy = *this;
    copy.m_context.push_back(ContextEntry{std::move(key), std::move(value)});
    return copy;
}

Status Status::withContext(std::string key, std::string value) && {
    m_context.push_back(ContextEntry{std::move(key), std::move(value)});
    return std::move(*this);
}

Status Status::withSampleIndex(std::uint64_t const sampleIndex) const& {
    Status copy = *this;
    copy.m_sampleIndex = sampleIndex;
    return copy;
}

Status Status::withSampleIndex(std::uint64_t const sampleIndex) && {
    m_sampleIndex = sampleIndex;
    return std::move(*this);
}

Status Status::withStreamId(std::uint64_t const streamId) const& {
    Status copy = *this;
    copy.m_streamId = streamId;
    return copy;
}

Status Status::withStreamId(std::uint64_t const streamId) && {
    m_streamId = streamId;
    return std::move(*this);
}

std::string Status::toString() const {
    if (ok()) {
        return "Ok";
    }
    std::ostringstream os;
    os << nameOf(m_code) << ": " << m_message;
    if (!m_context.empty()) {
        os << " [";
        bool first = true;
        for (const auto& entry : m_context) {
            if (!first) {
                os << ", ";
            }
            first = false;
            os << entry.key << "=" << entry.value;
        }
        os << "]";
    }
    if (m_sampleIndex.has_value()) {
        os << " @sample=" << *m_sampleIndex;
    }
    if (m_streamId.has_value()) {
        os << " stream=" << *m_streamId;
    }
    return os.str();
}

}  // namespace usn
