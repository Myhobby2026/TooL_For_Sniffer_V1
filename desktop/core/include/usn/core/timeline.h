// -----------------------------------------------------------------------------
// timeline.h -- the master timeline (docs/architecture_review.md section 9.3).
//
// Position is a piecewise-linear function of SampleIndex, built from block
// boundaries, so a sample-rate change mid-capture stays exactly reconstructible.
//
// gaps() is a first-class list on purpose: a capture that lost a packet must be
// VISIBLY discontinuous in every view -- waveform, hex, transaction table --
// rather than silently time-shifted (master spec section 14).
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <vector>

#include "usn/common/status.h"
#include "usn/model/time.h"

namespace usn::core {

struct RateSegment {
    SampleIndex first{};
    std::uint64_t sampleRateHz{0};
};

struct GapRecord {
    SampleIndex firstMissing{};
    SampleIndex afterGap{};
    std::uint64_t missingSamples{0};
    std::uint32_t sequenceAtGap{0};
    bool deviceReported{false};   // true when the device itself flagged an overflow
};

class Timeline {
public:
    Timeline() = default;
    Timeline(Timebase timebase, SampleIndex origin, DeviceTick originTick);

    [[nodiscard]] const Timebase& timebase() const noexcept { return m_timebase; }
    [[nodiscard]] SampleIndex origin() const noexcept { return m_origin; }
    [[nodiscard]] DeviceTick originTick() const noexcept { return m_originTick; }
    [[nodiscard]] ClockDomain domain() const noexcept { return m_domain; }
    void setDomain(ClockDomain domain) noexcept { m_domain = domain; }

    // Adds a rate segment. Segments must be added in increasing SampleIndex order;
    // an out-of-order segment is an error rather than a silent re-sort, because it
    // means the caller's block stream is not what it thinks it is.
    [[nodiscard]] Status addSegment(SampleIndex first, std::uint64_t sampleRateHz);

    void recordGap(const GapRecord& gap);
    [[nodiscard]] const std::vector<GapRecord>& gaps() const noexcept { return m_gaps; }
    [[nodiscard]] bool isContiguous() const noexcept { return m_gaps.empty(); }
    [[nodiscard]] std::uint64_t totalMissingSamples() const noexcept;

    // Exact, integer-only.
    [[nodiscard]] StatusOr<RationalTime> timeAt(SampleIndex index) const;
    [[nodiscard]] StatusOr<SampleIndex> indexAtTime(const RationalTime& t) const;
    [[nodiscard]] std::uint64_t sampleRateAt(SampleIndex index) const noexcept;

    // Presentation ONLY. Lossy by construction; callers must also show the exact
    // SampleIndex so a rounded value is never mistaken for ground truth.
    [[nodiscard]] double secondsAt(SampleIndex index) const noexcept;

    [[nodiscard]] SampleIndex endExclusive() const noexcept { return m_end; }
    void extendTo(SampleIndex endExclusive) noexcept;
    [[nodiscard]] std::uint64_t sampleCount() const noexcept;

    [[nodiscard]] Status validate() const;

private:
    std::size_t segmentIndexFor(SampleIndex index) const noexcept;

    Timebase m_timebase{Timebase::nanoseconds()};
    SampleIndex m_origin{};
    DeviceTick m_originTick{};
    ClockDomain m_domain{kPrimaryClockDomain};
    std::vector<RateSegment> m_segments;
    std::vector<GapRecord> m_gaps;
    SampleIndex m_end{};
};

}  // namespace usn::core
