// -----------------------------------------------------------------------------
// time.cpp -- see time.h.
// -----------------------------------------------------------------------------
#include "usn/model/time.h"

#include <numeric>
#include <sstream>

#include <fmt/format.h>

namespace usn {

bool normalizeRational(std::int64_t& numerator, std::uint64_t& denominator) noexcept {
    if (denominator == 0) {
        return false;
    }
    if (numerator == 0) {
        denominator = 1;
        return true;
    }
    auto const absNum = numerator < 0 ? static_cast<std::uint64_t>(-(numerator + 1)) + 1
                                      : static_cast<std::uint64_t>(numerator);
    auto const g = std::gcd(absNum, denominator);
    if (g == 0) {
        return false;
    }
    numerator /= static_cast<std::int64_t>(g);
    denominator /= g;
    return true;
}

StatusOr<std::uint64_t> mulDiv64(std::uint64_t const a, std::uint64_t const b,
                                 std::uint64_t const c) {
    if (c == 0) {
        return Status::error(ErrorCode::ConfigurationError, "mulDiv64: divisor is zero");
    }
    if (a == 0 || b == 0) {
        return std::uint64_t{0};
    }
    // Overflow check without __int128 so MSVC compiles this unchanged.
    if (a > UINT64_MAX / b) {
        return Status::error(ErrorCode::OutOfMemory,
                             "mulDiv64: intermediate product exceeds 64 bits")
            .withContext("a", std::to_string(a))
            .withContext("b", std::to_string(b))
            .withContext("c", std::to_string(c));
    }
    return (a * b) / c;
}

StatusOr<RationalTime> sampleIndexToTime(SampleIndex const origin, SampleIndex const at,
                                        std::uint64_t const sampleRateHz) {
    if (sampleRateHz == 0) {
        return Status::error(ErrorCode::ConfigurationError,
                             "sampleIndexToTime: sample rate is zero");
    }
    RationalTime result;
    // Signed so that asking for a time before the origin yields a negative value
    // rather than wrapping.
    result.numerator = at.value >= origin.value
                           ? static_cast<std::int64_t>(at.value - origin.value)
                           : -static_cast<std::int64_t>(origin.value - at.value);
    result.denominator = sampleRateHz;
    if (!normalizeRational(result.numerator, result.denominator)) {
        return Status::error(ErrorCode::Unknown, "sampleIndexToTime: normalization failed");
    }
    return result;
}

StatusOr<SampleIndex> timeToSampleIndex(SampleIndex const origin, const RationalTime& t,
                                       std::uint64_t const sampleRateHz) {
    if (sampleRateHz == 0) {
        return Status::error(ErrorCode::ConfigurationError,
                             "timeToSampleIndex: sample rate is zero");
    }
    if (t.denominator == 0) {
        return Status::error(ErrorCode::ConfigurationError,
                             "timeToSampleIndex: zero denominator");
    }
    if (t.numerator < 0) {
        // Before the origin of the timeline. Clamp to the origin rather than
        // wrapping a uint64 -- and report it, because it means the caller's
        // request was out of range.
        return Status::error(ErrorCode::ConfigurationError,
                             "timeToSampleIndex: requested time precedes the timeline origin")
            .withContext("origin", std::to_string(origin.value));
    }
    // samples = floor( (num/den) * rate )
    auto const samples = mulDiv64(static_cast<std::uint64_t>(t.numerator), sampleRateHz,
                                  t.denominator);
    if (!samples.ok()) {
        return samples.status();
    }
    return SampleIndex(origin.value + *samples);
}

StatusOr<RationalTime> tickToTime(DeviceTick const origin, DeviceTick const at,
                                 const Timebase& timebase) {
    if (!timebase.isValid()) {
        return Status::error(ErrorCode::ConfigurationError, "tickToTime: invalid timebase");
    }
    RationalTime result;
    result.numerator = at.value >= origin.value
                           ? static_cast<std::int64_t>(at.value - origin.value)
                           : -static_cast<std::int64_t>(origin.value - at.value);
    // seconds = deltaTicks * numerator / denominator
    if (timebase.numerator != 1) {
        auto const scaled = mulDiv64(static_cast<std::uint64_t>(result.numerator < 0
                                                                   ? -result.numerator
                                                                   : result.numerator),
                                     timebase.numerator, 1);
        if (!scaled.ok()) {
            return scaled.status();
        }
        result.numerator = result.numerator < 0 ? -static_cast<std::int64_t>(*scaled)
                                                : static_cast<std::int64_t>(*scaled);
    }
    result.denominator = timebase.denominator;
    if (!normalizeRational(result.numerator, result.denominator)) {
        return Status::error(ErrorCode::Unknown, "tickToTime: normalization failed");
    }
    return result;
}

std::string RationalTime::toString() const {
    return fmt::format("{}/{}", numerator, denominator);
}

std::string formatDurationNs(std::uint64_t const ns) {
    if (ns >= 1'000'000'000ULL) {
        return fmt::format("{:.9} s", static_cast<double>(ns) / 1e9);
    }
    if (ns >= 1'000'000ULL) {
        return fmt::format("{:.6} ms", static_cast<double>(ns) / 1e6);
    }
    if (ns >= 1'000ULL) {
        return fmt::format("{:.3} us", static_cast<double>(ns) / 1e3);
    }
    return fmt::format("{} ns", ns);
}

}  // namespace usn
