// -----------------------------------------------------------------------------
// diagnostics.h -- the recorded form of a Status (master spec section 14:
// never silently discard data).
//
// Aggregation matters: an overflow at 10 MSPS can occur thousands of times per
// second. The model stores a count plus first/last occurrence rather than
// thousands of rows, so the Diagnostics panel stays usable AND the true magnitude
// is preserved. Nothing is discarded -- it is aggregated, visibly.
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "usn/common/status.h"
#include "usn/model/time.h"

namespace usn {

enum class DiagnosticSeverity : std::uint8_t { Info = 0, Warning, Error, Critical };
enum class DiagnosticCategory : std::uint8_t {
    Application = 0,
    Device,
    Usb,
    Capture,
    Buffer,
    Decoder,
    Storage,
    Host,
    Plugin,
    Script
};

[[nodiscard]] std::string_view nameOf(DiagnosticSeverity severity) noexcept;
[[nodiscard]] std::string_view nameOf(DiagnosticCategory category) noexcept;
[[nodiscard]] DiagnosticSeverity severityOf(ErrorSeverity severity) noexcept;

struct DiagnosticEvent {
    DiagnosticSeverity severity{DiagnosticSeverity::Info};
    DiagnosticCategory category{DiagnosticCategory::Application};
    ErrorCode code{ErrorCode::Unknown};
    std::string message;

    // Position context. Optional because not every diagnostic has one.
    std::optional<SampleIndex> atSample;
    std::optional<std::uint64_t> streamId;

    // Aggregation: how many times this same condition has been seen.
    std::uint64_t count{1};
    std::uint64_t firstSeenMonotonicNs{0};
    std::uint64_t lastSeenMonotonicNs{0};

    std::int64_t wallClockNs{0};   // human reference only
    std::vector<ContextEntry> context;

    [[nodiscard]] std::string toString() const;

    // Two events aggregate together when they describe the same condition.
    // Sample index is deliberately NOT part of the key: an overflow that moves
    // through the capture is still one problem, reported once with a count.
    [[nodiscard]] bool sameConditionAs(const DiagnosticEvent& other) const noexcept;

    void mergeFrom(const DiagnosticEvent& other) noexcept;
};

// Builds a DiagnosticEvent from a Status, deriving severity and category from the
// error code so no call site has to classify by hand.
[[nodiscard]] DiagnosticEvent diagnosticFromStatus(const Status& status);

[[nodiscard]] DiagnosticCategory categoryForCode(ErrorCode code) noexcept;

}  // namespace usn
