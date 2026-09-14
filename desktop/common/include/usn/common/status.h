// -----------------------------------------------------------------------------
// status.h -- the error model (docs/architecture_review.md section 4.2).
//
// No exceptions cross module boundaries. Every fallible operation returns
// Status or StatusOr<T>. Every error carries enough context to be diagnosed
// without a debugger: a code, a message, key/value context, and -- where
// meaningful -- the SampleIndex and stream it relates to.
//
// This is a hand-rolled minimal StatusOr rather than std::expected because the
// C++ floor is C++20 and std::expected needs GCC 13 / MSVC 19.36 (deviation D4).
// The API is intentionally shaped like std::expected so it can become an alias
// later without touching call sites.
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace usn {

enum class ErrorCode : std::uint16_t {
    Ok = 0,

    // --- device -------------------------------------------------------------
    DeviceNotFound,
    DeviceBusy,
    DeviceUnsupported,
    DeviceResetDetected,
    DeviceFirmwareIncompatible,
    DeviceConfigurationRejected,
    DeviceNotConnected,

    // --- usb / transport ----------------------------------------------------
    UsbOpenFailed,
    UsbReadFailed,
    UsbWriteFailed,
    UsbTimeout,
    UsbDisconnected,

    // --- framing / integrity ------------------------------------------------
    CrcMismatch,
    InvalidMagic,
    InvalidPacketLength,
    HeaderVersionUnsupported,
    SequenceGap,
    SampleIndexGap,
    StreamIdMismatch,
    FramingResync,
    InvalidPacketType,
    ReservedFlagSet,

    // --- capture / buffers --------------------------------------------------
    CaptureNotArmed,
    CaptureAlreadyRunning,
    DmaOverflow,
    RingBufferOverflow,
    HostQueueOverflow,
    SampleRateUnsupported,
    ChannelCountUnsupported,
    StrideUnsupported,
    TriggerUnsupported,

    // --- decoder / protocol -------------------------------------------------
    DecoderNotFound,
    DecoderConfigurationInvalid,
    DecoderInternalError,
    DecoderAlreadyRegistered,
    ProtocolFramingError,
    ProtocolChecksumError,
    ProtocolTimeout,

    // --- file ---------------------------------------------------------------
    FileOpenFailed,
    FileReadFailed,
    FileWriteFailed,
    FileFormatUnsupported,
    FileCorrupt,
    FileTruncated,
    FileVersionUnsupported,
    IndexRebuilt,

    // --- system -------------------------------------------------------------
    PluginLoadFailed,
    PluginApiMismatch,
    ScriptError,
    ScriptSandboxViolation,
    ConfigurationError,
    OutOfMemory,
    Cancelled,
    NotImplemented,
    Unknown
};

enum class ErrorSeverity : std::uint8_t { Info, Warning, Error, Critical };

// Severity is derived from the code by table, so callers cannot forget to
// classify and two modules cannot disagree about how bad something is.
[[nodiscard]] ErrorSeverity severityForCode(ErrorCode code) noexcept;

// Stable, human-readable name. Used by logs, the Diagnostics panel and tests.
[[nodiscard]] std::string_view nameOf(ErrorCode code) noexcept;

struct ContextEntry {
    std::string key;
    std::string value;
};

class Status {
public:
    Status() noexcept = default;

    // Named constructor for the success case. It cannot be called ok() because
    // that collides with the ok() predicate below -- overload resolution does not
    // consider return type.
    [[nodiscard]] static Status success() noexcept { return Status(); }

    [[nodiscard]] static Status error(ErrorCode code, std::string message);

    // Builder-style context. Returns *this by value so calls chain.
    [[nodiscard]] Status withContext(std::string key, std::string value) const&;
    [[nodiscard]] Status withContext(std::string key, std::string value) &&;

    [[nodiscard]] Status withSampleIndex(std::uint64_t sampleIndex) const&;
    [[nodiscard]] Status withSampleIndex(std::uint64_t sampleIndex) &&;

    [[nodiscard]] Status withStreamId(std::uint64_t streamId) const&;
    [[nodiscard]] Status withStreamId(std::uint64_t streamId) &&;

    [[nodiscard]] bool ok() const noexcept { return m_code == ErrorCode::Ok; }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] ErrorCode code() const noexcept { return m_code; }
    [[nodiscard]] ErrorSeverity severity() const noexcept { return severityForCode(m_code); }
    [[nodiscard]] std::string_view message() const noexcept { return m_message; }
    [[nodiscard]] const std::vector<ContextEntry>& context() const noexcept { return m_context; }
    [[nodiscard]] std::optional<std::uint64_t> sampleIndex() const noexcept { return m_sampleIndex; }
    [[nodiscard]] std::optional<std::uint64_t> streamId() const noexcept { return m_streamId; }

    // Deterministic single-line rendering: "Code: message [k=v, k=v] @sample=N".
    // Stability matters because tests and log analysis depend on it.
    [[nodiscard]] std::string toString() const;

private:
    ErrorCode m_code{ErrorCode::Ok};
    std::string m_message;
    std::vector<ContextEntry> m_context;
    std::optional<std::uint64_t> m_sampleIndex;
    std::optional<std::uint64_t> m_streamId;
};

// -----------------------------------------------------------------------------
// StatusOr<T>
// -----------------------------------------------------------------------------
template <class T>
class StatusOr {
    static_assert(!std::is_reference_v<T>, "StatusOr<T&> is not supported");
    static_assert(!std::is_same_v<std::remove_cv_t<T>, Status>, "StatusOr<Status> is ambiguous");

public:
    StatusOr(Status status) : m_storage(std::move(status)) {}  // NOLINT implicit by design

    StatusOr(T value) : m_storage(std::move(value)) {}         // NOLINT implicit by design

    [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(m_storage); }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const T& value() const& { return std::get<T>(m_storage); }
    [[nodiscard]] T& value() & { return std::get<T>(m_storage); }
    [[nodiscard]] T&& value() && { return std::get<T>(std::move(m_storage)); }

    [[nodiscard]] const T& operator*() const& { return value(); }
    [[nodiscard]] T& operator*() & { return value(); }
    [[nodiscard]] const T* operator->() const { return &value(); }
    [[nodiscard]] T* operator->() { return &value(); }

    // Total, not throwing. An earlier version forwarded straight to std::get<Status>,
    // which raises std::bad_variant_access when the alternative holding the value is
    // active -- so asking a SUCCESSFUL StatusOr for its status, a perfectly natural
    // thing to write in a validation helper, threw out of a noexcept-looking path.
    // A successful result now reports Status::success() instead.
    [[nodiscard]] const Status& status() const& {
        static const Status kSuccess;
        return ok() ? kSuccess : std::get<Status>(m_storage);
    }
    [[nodiscard]] Status takeStatus() && {
        return ok() ? Status::success() : std::get<Status>(std::move(m_storage));
    }

    // value_or-style access for code paths that have a sensible fallback.
    [[nodiscard]] T valueOr(T fallback) const& {
        return ok() ? value() : std::move(fallback);
    }

private:
    std::variant<T, Status> m_storage;
};

// Small helper for the very common "propagate the error" pattern.
#define USN_RETURN_IF_ERROR(expr)                    \
    do {                                             \
        ::usn::Status const usn_st_ = (expr);        \
        if (!usn_st_.ok()) {                         \
            return usn_st_;                          \
        }                                            \
    } while (false)

#define USN_ASSIGN_OR_RETURN(lhs, expr)                             \
    auto&& usn_so_ = (expr);                                        \
    if (!usn_so_.ok()) {                                            \
        return std::move(usn_so_).takeStatus();                     \
    }                                                               \
    lhs = std::move(usn_so_).value()

}  // namespace usn
