// -----------------------------------------------------------------------------
// timeline.cpp -- see timeline.h.
// -----------------------------------------------------------------------------
#include "usn/core/timeline.h"

#include <fmt/format.h>

namespace usn::core {

Timeline::Timeline(Timebase const timebase, SampleIndex const origin, DeviceTick const originTick)
    : m_timebase(timebase), m_origin(origin), m_originTick(originTick), m_end(origin) {}

Status Timeline::addSegment(SampleIndex const first, std::uint64_t const sampleRateHz) {
    if (sampleRateHz == 0) {
        return Status::error(ErrorCode::SampleRateUnsupported,
                             "timeline segment has a zero sample rate")
            .withSampleIndex(first.value);
    }
    if (!m_segments.empty() && first.value < m_segments.back().first.value) {
        return Status::error(
                   ErrorCode::ConfigurationError,
                   fmt::format("timeline segment at {} precedes the previous segment at {}",
                               first.value, m_segments.back().first.value))
            .withSampleIndex(first.value);
    }
    if (!m_segments.empty() && first.value == m_segments.back().first.value) {
        return Status::error(ErrorCode::ConfigurationError,
                             fmt::format("duplicate timeline segment at {}", first.value))
            .withSampleIndex(first.value);
    }
    if (m_segments.empty() && first.value != m_origin.value) {
        // A capture can legitimately start after the timeline origin (for example
        // after a pre-trigger rewind). Seed a segment at the origin so that
        // timeAt(origin) is well defined, then add the requested one.
        m_segments.push_back(RateSegment{m_origin, sampleRateHz});
    }
    m_segments.push_back(RateSegment{first, sampleRateHz});
    return Status::success();
}

void Timeline::recordGap(const GapRecord& gap) { m_gaps.push_back(gap); }

std::uint64_t Timeline::totalMissingSamples() const noexcept {
    std::uint64_t total = 0;
    for (const auto& gap : m_gaps) {
        total += gap.missingSamples;
    }
    return total;
}

std::size_t Timeline::segmentIndexFor(SampleIndex const index) const noexcept {
    if (m_segments.empty()) {
        return 0;
    }
    std::size_t best = 0;
    for (std::size_t i = 0; i < m_segments.size(); ++i) {
        if (m_segments[i].first.value <= index.value) {
            best = i;
        } else {
            break;
        }
    }
    return best;
}

std::uint64_t Timeline::sampleRateAt(SampleIndex const index) const noexcept {
    if (m_segments.empty()) {
        return 0;
    }
    return m_segments[segmentIndexFor(index)].sampleRateHz;
}

StatusOr<RationalTime> Timeline::timeAt(SampleIndex const index) const {
    if (m_segments.empty()) {
        return Status::error(ErrorCode::ConfigurationError,
                             "timeline has no rate segments; nothing has been captured yet");
    }
    if (index.value < m_origin.value) {
        return Status::error(ErrorCode::ConfigurationError,
                             fmt::format("sample {} precedes the timeline origin {}", index.value,
                                         m_origin.value))
            .withSampleIndex(index.value);
    }
    // Walk segments accumulating exact rational time, so a rate change mid-capture
    // does not smear across the whole timeline.
    std::int64_t numerator = 0;
    std::uint64_t denominator = 1;
    for (std::size_t i = 0; i < m_segments.size(); ++i) {
        SampleIndex const segStart = m_segments[i].first;
        SampleIndex const segEnd =
            (i + 1 < m_segments.size()) ? m_segments[i + 1].first : SampleIndex(UINT64_MAX);
        if (index.value < segStart.value) {
            break;
        }
        std::uint64_t const upto = index.value < segEnd.value ? index.value : segEnd.value;
        if (upto <= segStart.value) {
            continue;
        }
        std::uint64_t const delta = upto - segStart.value;
        std::uint64_t const rate = m_segments[i].sampleRateHz;
        // Accumulate numerator/denominator += delta/rate, i.e.
        // num/den + delta/rate = (num*rate + delta*den) / (den*rate).
        // Every intermediate is overflow-checked rather than allowed to wrap: a
        // silently wrapped timestamp is worse than an error.
        if (denominator > UINT64_MAX / rate) {
            return Status::error(ErrorCode::OutOfMemory,
                                 "timeline denominators overflowed while accumulating time")
                .withSampleIndex(index.value);
        }
        std::uint64_t const newDen = denominator * rate;
        // num*rate and delta*den must both fit; check before adding.
        if (static_cast<std::uint64_t>(numerator) > UINT64_MAX / rate) {
            return Status::error(ErrorCode::OutOfMemory,
                                 "timeline numerator overflowed while accumulating time")
                .withSampleIndex(index.value);
        }
        std::uint64_t const lhs = static_cast<std::uint64_t>(numerator) * rate;
        if (delta > UINT64_MAX / denominator) {
            return Status::error(ErrorCode::OutOfMemory,
                                 "timeline term overflowed while accumulating time")
                .withSampleIndex(index.value);
        }
        std::uint64_t const rhs = delta * denominator;
        if (lhs > UINT64_MAX - rhs) {
            return Status::error(ErrorCode::OutOfMemory,
                                 "timeline sum overflowed while accumulating time")
                .withSampleIndex(index.value);
        }
        numerator = static_cast<std::int64_t>(lhs + rhs);
        denominator = newDen;
    }
    RationalTime result{numerator, denominator};
    if (!normalizeRational(result.numerator, result.denominator)) {
        return Status::error(ErrorCode::Unknown, "timeline could not normalize the result");
    }
    return result;
}

StatusOr<SampleIndex> Timeline::indexAtTime(const RationalTime& t) const {
    if (m_segments.empty()) {
        return Status::error(ErrorCode::ConfigurationError, "timeline has no rate segments");
    }
    if (t.denominator == 0) {
        return Status::error(ErrorCode::ConfigurationError, "time has a zero denominator");
    }
    if (t.numerator < 0) {
        return Status::error(ErrorCode::ConfigurationError,
                             "requested time precedes the timeline origin");
    }

    // Walk the segments, subtracting each one's exact duration until the requested
    // time falls inside a segment, then convert the remainder to a sample offset.
    // All arithmetic is integer; nothing is rounded until the final floor.
    std::int64_t remNum = t.numerator;
    std::uint64_t remDen = t.denominator;

    for (std::size_t i = 0; i < m_segments.size(); ++i) {
        SampleIndex const segStart = m_segments[i].first;
        SampleIndex const segEnd =
            (i + 1 < m_segments.size()) ? m_segments[i + 1].first : m_end;
        std::uint64_t const rate = m_segments[i].sampleRateHz;
        std::uint64_t const segSamples =
            (segEnd.value > segStart.value) ? segEnd.value - segStart.value : 0;

        // Is the remaining time within this segment?
        //   remNum/remDen <= segSamples/rate   <=>   remNum*rate <= segSamples*remDen
        auto const remScaled = mulDiv64(static_cast<std::uint64_t>(remNum), rate, 1);
        if (!remScaled.ok()) {
            return remScaled.status();
        }
        bool lastSegment = (i + 1 == m_segments.size());
        if (segSamples == 0 || lastSegment || *remScaled <= segSamples * remDen) {
            auto offset =
                mulDiv64(static_cast<std::uint64_t>(remNum), rate, remDen);
            if (!offset.ok()) {
                return offset.status();
            }
            if (!lastSegment && offset.value() > segSamples) {
                // Clamp at the segment boundary: the caller asked for a time inside
                // this segment, so the answer cannot be a sample from the next one.
                offset.value() = segSamples;
            }
            if (lastSegment) {
                // Past the end of the last segment there is no data, so the furthest
                // position that can be named is m_end. Extrapolating instead would
                // invent sample indices that were never captured -- and a view that
                // scrolled off the right edge would report a position beyond the
                // capture as though it were real.
                auto const available = m_end.value > segStart.value ? m_end.value - segStart.value : 0;
                if (offset.value() > available) {
                    offset.value() = available;
                }
            }
            return SampleIndex(segStart.value + offset.value());
        }

        // Consume this segment: remaining -= segSamples/rate.
        //   remNum/remDen - segSamples/rate = (remNum*rate - segSamples*remDen)/(remDen*rate)
        std::uint64_t const newDen = remDen * rate;
        std::int64_t const newNum =
            static_cast<std::int64_t>(*remScaled) - static_cast<std::int64_t>(segSamples * remDen);
        remNum = newNum;
        remDen = newDen;
        if (!normalizeRational(remNum, remDen)) {
            return Status::error(ErrorCode::Unknown,
                                 "timeline could not normalize the remaining time");
        }
    }
    // Past the end of every known segment: the furthest position we can name.
    return m_end;
}

double Timeline::secondsAt(SampleIndex const index) const noexcept {
    auto const t = timeAt(index);
    return t.ok() ? t->toSeconds() : 0.0;
}

void Timeline::extendTo(SampleIndex const endExclusive) noexcept {
    if (endExclusive.value > m_end.value) {
        m_end = endExclusive;
    }
}

std::uint64_t Timeline::sampleCount() const noexcept {
    return m_end.value > m_origin.value ? m_end.value - m_origin.value : 0;
}

Status Timeline::validate() const {
    if (!m_timebase.isValid()) {
        return Status::error(ErrorCode::ConfigurationError, "timeline timebase is invalid");
    }
    for (std::size_t i = 1; i < m_segments.size(); ++i) {
        if (m_segments[i].first.value <= m_segments[i - 1].first.value) {
            return Status::error(ErrorCode::ConfigurationError,
                                 fmt::format("timeline segments are not strictly ordered at index {}",
                                             i));
        }
    }
    for (const auto& gap : m_gaps) {
        if (gap.afterGap.value <= gap.firstMissing.value) {
            return Status::error(
                ErrorCode::ConfigurationError,
                fmt::format("gap record is inconsistent: firstMissing {} afterGap {}",
                            gap.firstMissing.value, gap.afterGap.value));
        }
        if (gap.missingSamples != gap.afterGap.value - gap.firstMissing.value) {
            return Status::error(ErrorCode::ConfigurationError,
                                 fmt::format("gap record claims {} missing samples but the range "
                                             "spans {}",
                                             gap.missingSamples,
                                             gap.afterGap.value - gap.firstMissing.value));
        }
    }
    return Status::success();
}

}  // namespace usn::core
