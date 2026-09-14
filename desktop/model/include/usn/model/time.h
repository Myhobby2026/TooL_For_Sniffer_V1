// -----------------------------------------------------------------------------
// time.h -- the timestamp model (docs/architecture_review.md section 9,
// master spec section 15).
//
// Three clocks, three jobs:
//   SampleIndex   authoritative position key. Exact, integer, monotonic.
//   DeviceTick    exact rational time math via a Timebase.
//   WallClockUtc  human reference only; NEVER used for arithmetic, because the
//                 host clock is not monotonic across NTP steps.
//
// No floating point appears in storage or in event records. Conversion to double
// happens only at the presentation edge (usn::app / gui), and the UI must also
// show the exact SampleIndex so a rounded display value can never be mistaken
// for ground truth.
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <compare>
#include <string>

#include "usn/common/status.h"

namespace usn {

// --- strong integer types ----------------------------------------------------
// Explicit constructors and no implicit conversion to/from integers: a
// SampleIndex must never be silently confused with a byte offset or a tick.

struct SampleIndex {
    std::uint64_t value{0};

    constexpr SampleIndex() noexcept = default;
    constexpr explicit SampleIndex(std::uint64_t v) noexcept : value(v) {}

    constexpr SampleIndex operator+(SampleIndex other) const noexcept {
        return SampleIndex(value + other.value);
    }
    constexpr SampleIndex operator-(SampleIndex other) const noexcept {
        return SampleIndex(value - other.value);
    }
    constexpr SampleIndex& operator+=(SampleIndex other) noexcept {
        value += other.value;
        return *this;
    }
    // Difference between two positions is a COUNT, not a position.
    constexpr std::uint64_t delta(SampleIndex other) const noexcept {
        return value >= other.value ? value - other.value : other.value - value;
    }

    friend constexpr auto operator<=>(SampleIndex, SampleIndex) noexcept = default;

    static constexpr SampleIndex invalid() noexcept { return SampleIndex(UINT64_MAX); }
    constexpr bool isValid() const noexcept { return value != UINT64_MAX; }
};

struct DeviceTick {
    std::uint64_t value{0};

    constexpr DeviceTick() noexcept = default;
    constexpr explicit DeviceTick(std::uint64_t v) noexcept : value(v) {}
    friend constexpr auto operator<=>(DeviceTick, DeviceTick) noexcept = default;
};

// --- exact rational time -----------------------------------------------------

// One tick == numerator/denominator seconds. Default is nanosecond ticks.
struct Timebase {
    std::uint64_t numerator{1};
    std::uint64_t denominator{1'000'000'000};

    [[nodiscard]] static constexpr Timebase nanoseconds() noexcept { return {1, 1'000'000'000}; }
    [[nodiscard]] static constexpr Timebase hertz(std::uint64_t hz) noexcept {
        return {1, hz == 0 ? 1 : hz};
    }
    [[nodiscard]] constexpr bool isValid() const noexcept {
        return numerator != 0 && denominator != 0;
    }
    friend constexpr auto operator<=>(Timebase, Timebase) noexcept = default;
};

// An exact point in time as a reduced rational number of seconds.
// Exact comparison of n1/d1 against n2/d2 (d1, d2 non-zero). Returns -1, 0 or 1.
// See RationalTime::compare for why this is not a cross-multiplication.
[[nodiscard]] constexpr int compareRational(std::int64_t n1, std::uint64_t d1, std::int64_t n2,
                                            std::uint64_t d2) noexcept {
    if (d1 == 0 || d2 == 0) {
        // A zero denominator is not a valid time; comparing the numerators keeps the
        // function total and constexpr rather than invoking undefined behaviour.
        return (n1 < n2) ? -1 : ((n1 > n2) ? 1 : 0);
    }
    // Opposite signs settle it immediately.
    if (n1 < 0 && n2 >= 0) {
        return -1;
    }
    if (n1 >= 0 && n2 < 0) {
        return 1;
    }
    bool const negative = n1 < 0;
    // Negate through -(x+1)+1 so INT64_MIN does not overflow.
    std::uint64_t a = negative ? static_cast<std::uint64_t>(-(n1 + 1)) + 1U
                               : static_cast<std::uint64_t>(n1);
    std::uint64_t b = negative ? static_cast<std::uint64_t>(-(n2 + 1)) + 1U
                               : static_cast<std::uint64_t>(n2);
    int flip = 1;
    for (;;) {
        std::uint64_t const q1 = a / d1;
        std::uint64_t const q2 = b / d2;
        if (q1 != q2) {
            return flip * (q1 < q2 ? -1 : 1);
        }
        std::uint64_t const r1 = a % d1;
        std::uint64_t const r2 = b % d2;
        if (r1 == 0 && r2 == 0) {
            return 0;
        }
        if (r1 == 0) {
            return flip * -1;   // left side is exactly q1, right side has more
        }
        if (r2 == 0) {
            return flip * 1;
        }
        // Compare r1/d1 against r2/d2, which is the inverse of d1/r1 vs d2/r2.
        std::uint64_t const nextA = d1;
        std::uint64_t const nextB = d2;
        d1 = r1;
        d2 = r2;
        a = nextA;
        b = nextB;
        flip = -flip;
    }
}

struct RationalTime {
    std::int64_t numerator{0};
    std::uint64_t denominator{1};

    [[nodiscard]] constexpr bool isZero() const noexcept { return numerator == 0; }
    // Presentation edge ONLY. Documented as lossy.
    [[nodiscard]] double toSeconds() const noexcept {
        return static_cast<double>(numerator) / static_cast<double>(denominator);
    }
    [[nodiscard]] std::string toString() const;

    // Exact ordering of two rationals.
    //
    // This is NOT the defaulted three-way comparison. A defaulted operator<=>
    // compares members in declaration order, i.e. numerator first and denominator
    // second, which is lexicographic and not a comparison of the two fractions:
    // 9973/500000 and 9973/1000000 have equal numerators but the first is twice
    // the second. Timebase positions produced by Timeline::timeAt() are normalized
    // (divided through by their gcd), so exactly this pair arises in practice, and
    // a monotonicity check over a capture would report time going backwards.
    //
    // Cross-multiplication (n1*d2 vs n2*d1) is the obvious fix but overflows
    // int64/uint64 for large numerators, and __int128 is not available on MSVC,
    // which is the primary toolchain. Instead this descends Euclidean-style:
    // compare the integer parts, then recurse on the remainders with the
    // comparison inverted. Every intermediate is a quotient or remainder of a
    // uint64 division, so nothing can overflow, and the denominators strictly
    // decrease so it terminates.
    [[nodiscard]] constexpr int compare(const RationalTime& other) const noexcept {
        return compareRational(numerator, denominator, other.numerator, other.denominator);
    }

    friend constexpr bool operator==(const RationalTime& a, const RationalTime& b) noexcept {
        return a.compare(b) == 0;
    }
    friend constexpr std::strong_ordering operator<=>(const RationalTime& a,
                                                      const RationalTime& b) noexcept {
        auto const order = a.compare(b);
        if (order < 0) {
            return std::strong_ordering::less;
        }
        return order > 0 ? std::strong_ordering::greater : std::strong_ordering::equal;
    }
};

// Identifies a clock source. Phase 1 has exactly one domain, but the type exists
// now so that adding analog or multi-device capture later does not change any
// signature (master spec section 35).
struct ClockDomain {
    std::uint32_t id{0};
    friend constexpr auto operator<=>(ClockDomain, ClockDomain) noexcept = default;
};

inline constexpr ClockDomain kPrimaryClockDomain{0};

// --- conversions -------------------------------------------------------------

// Reduces a rational in place; returns false if it cannot be represented.
[[nodiscard]] bool normalizeRational(std::int64_t& numerator, std::uint64_t& denominator) noexcept;

// Exact time of `at` relative to `origin` at a constant sample rate.
// deltaSamples * 1 is the numerator, sampleRateHz the denominator.
[[nodiscard]] StatusOr<RationalTime> sampleIndexToTime(SampleIndex origin, SampleIndex at,
                                                      std::uint64_t sampleRateHz);

// Inverse: the largest SampleIndex whose time is <= t. Exact, no floating point.
[[nodiscard]] StatusOr<SampleIndex> timeToSampleIndex(SampleIndex origin, const RationalTime& t,
                                                     std::uint64_t sampleRateHz);

[[nodiscard]] StatusOr<RationalTime> tickToTime(DeviceTick origin, DeviceTick at,
                                                const Timebase& timebase);

// Exact a*b/c for the ranges this project needs, with overflow detection rather
// than silent wraparound. Deliberately avoids __int128 so MSVC can compile it.
[[nodiscard]] StatusOr<std::uint64_t> mulDiv64(std::uint64_t a, std::uint64_t b,
                                              std::uint64_t c);

// Formats nanoseconds as e.g. "1.234567890 s" / "123.456 us" for display.
[[nodiscard]] std::string formatDurationNs(std::uint64_t ns);

}  // namespace usn
