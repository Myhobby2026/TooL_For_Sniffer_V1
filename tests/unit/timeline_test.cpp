// -----------------------------------------------------------------------------
// timeline_test.cpp -- position is a function of SampleIndex, exactly.
//
// The property under test is not "the time looks right". It is that timeAt() and
// indexAtTime() are exact inverses over the integers, that a rate change does not
// perturb positions before it, and that gaps stay visible as gaps.
// -----------------------------------------------------------------------------
#include <gtest/gtest.h>

#include "usn/core/timeline.h"
#include "usn/test/test_fixtures.h"

using usn::DeviceTick;
using usn::RationalTime;
using usn::SampleIndex;
using usn::Timebase;
using usn::core::GapRecord;
using usn::core::Timeline;

namespace {

Timeline singleRate(std::uint64_t const rateHz) {
    Timeline t(Timebase{}, SampleIndex(0), DeviceTick(0));
    EXPECT_TRUE(t.addSegment(SampleIndex(0), rateHz).ok());
    t.extendTo(SampleIndex(1'000'000));
    return t;
}

TEST(TimelineTest, EmptyTimelineRejectsQueries) {
    Timeline t;
    EXPECT_FALSE(t.timeAt(SampleIndex(0)).ok());
    EXPECT_FALSE(t.indexAtTime(RationalTime{0, 1}).ok());
    EXPECT_EQ(t.sampleCount(), 0u);
    EXPECT_TRUE(t.isContiguous());
}

TEST(TimelineTest, PositionIsExactIntegerRationalAtOneMegahertz) {
    auto t = singleRate(1'000'000);
    // sample 1 at 1 MHz is exactly 1/1000000 s. Represented as a rational, no
    // rounding is involved, so this must be an exact equality rather than a
    // comparison within epsilon.
    auto const one = t.timeAt(SampleIndex(1));
    ASSERT_TRUE(one.ok());
    EXPECT_EQ(one->numerator, 1);
    EXPECT_EQ(one->denominator, 1'000'000u);

    auto const million = t.timeAt(SampleIndex(1'000'000));
    ASSERT_TRUE(million.ok());
    // 1e6 samples at 1 MHz == exactly 1 second.
    EXPECT_EQ(million->numerator, 1);
    EXPECT_EQ(million->denominator, 1u);
}

TEST(TimelineTest, TimeAndIndexAreExactInverses) {
    for (std::uint64_t rate : {1'000'000ull, 10'000'000ull, 48'000'000ull, 3ull}) {
        auto t = singleRate(rate);
        for (std::uint64_t index : {0ull, 1ull, 7ull, 4096ull, 999'999ull}) {
            auto const time = t.timeAt(SampleIndex(index));
            ASSERT_TRUE(time.ok());
            auto const back = t.indexAtTime(*time);
            ASSERT_TRUE(back.ok());
            EXPECT_EQ(back->value, index) << "rate=" << rate << " index=" << index;
        }
    }
}

TEST(TimelineTest, OddRateDoesNotAccumulateRoundingError) {
    // 3 Hz is chosen because it is not a power of two and not a multiple of ten:
    // any float-based accumulation would drift measurably over 999999 samples.
    auto t = singleRate(3);
    auto const time = t.timeAt(SampleIndex(999'999));
    ASSERT_TRUE(time.ok());
    // 999999 / 3 == 333333 exactly.
    EXPECT_EQ(time->numerator, 333'333u);
    EXPECT_EQ(time->denominator, 1u);
    auto const back = t.indexAtTime(*time);
    ASSERT_TRUE(back.ok());
    EXPECT_EQ(back->value, 999'999u);
}

TEST(TimelineTest, RateChangeDoesNotMoveEarlierSamples) {
    Timeline t(Timebase{}, SampleIndex(0), DeviceTick(0));
    ASSERT_TRUE(t.addSegment(SampleIndex(0), 1'000'000).ok());
    ASSERT_TRUE(t.addSegment(SampleIndex(500'000), 2'000'000).ok());
    t.extendTo(SampleIndex(1'000'000));

    // Before the change: unaffected by the second segment existing at all.
    auto const before = t.timeAt(SampleIndex(250'000));
    ASSERT_TRUE(before.ok());
    EXPECT_EQ(before->numerator, 1u);
    EXPECT_EQ(before->denominator, 4u);   // 0.25 s

    // At the change point: 0.5 s.
    auto const at = t.timeAt(SampleIndex(500'000));
    ASSERT_TRUE(at.ok());
    EXPECT_EQ(at->numerator, 1u);
    EXPECT_EQ(at->denominator, 2u);

    // After: 0.5 s + 500000/2000000 s == 0.75 s.
    auto const after = t.timeAt(SampleIndex(1'000'000));
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after->numerator, 3u);
    EXPECT_EQ(after->denominator, 4u);

    EXPECT_EQ(t.sampleRateAt(SampleIndex(499'999)), 1'000'000u);
    EXPECT_EQ(t.sampleRateAt(SampleIndex(500'000)), 2'000'000u);
}

TEST(TimelineTest, OutOfOrderSegmentIsRejected) {
    Timeline t(Timebase{}, SampleIndex(0), DeviceTick(0));
    ASSERT_TRUE(t.addSegment(SampleIndex(1000), 1'000'000).ok());
    // Re-sorting silently would hide a producer that is emitting blocks out of
    // order, which is exactly the bug this layer exists to make visible.
    EXPECT_FALSE(t.addSegment(SampleIndex(500), 1'000'000).ok());
    EXPECT_FALSE(t.addSegment(SampleIndex(1000), 0).ok());
}

TEST(TimelineTest, TimeIsMonotonicAcrossTheWholeRange) {
    Timeline t(Timebase{}, SampleIndex(0), DeviceTick(0));
    ASSERT_TRUE(t.addSegment(SampleIndex(0), 1'000'000).ok());
    ASSERT_TRUE(t.addSegment(SampleIndex(300'000), 500'000).ok());
    ASSERT_TRUE(t.addSegment(SampleIndex(700'000), 8'000'000).ok());
    t.extendTo(SampleIndex(900'000));

    RationalTime previous{0, 1};
    for (std::uint64_t index = 0; index <= 900'000; index += 9973) {
        auto const time = t.timeAt(SampleIndex(index));
        ASSERT_TRUE(time.ok()) << "index=" << index;
        EXPECT_TRUE(*time >= previous) << "time went backwards at index " << index;
        previous = *time;
    }
}

TEST(TimelineTest, GapsAreRecordedAndCounted) {
    Timeline t(Timebase{}, SampleIndex(0), DeviceTick(0));
    ASSERT_TRUE(t.addSegment(SampleIndex(0), 1'000'000).ok());
    EXPECT_TRUE(t.isContiguous());
    EXPECT_EQ(t.totalMissingSamples(), 0u);

    GapRecord gap;
    gap.firstMissing = SampleIndex(100);
    gap.afterGap = SampleIndex(250);
    gap.missingSamples = 150;
    gap.sequenceAtGap = 7;
    gap.deviceReported = true;
    t.recordGap(gap);
    t.extendTo(SampleIndex(400));

    EXPECT_FALSE(t.isContiguous());
    ASSERT_EQ(t.gaps().size(), 1u);
    EXPECT_EQ(t.gaps()[0].missingSamples, 150u);
    EXPECT_TRUE(t.gaps()[0].deviceReported);
    EXPECT_EQ(t.totalMissingSamples(), 150u);

    // sampleCount is the stored extent, not the extent minus the gap: the gap is
    // reported separately so a view can render it as a discontinuity.
    EXPECT_EQ(t.sampleCount(), 400u);
}

TEST(TimelineTest, IndexAtTimeClampsAtSegmentBoundary) {
    Timeline t(Timebase{}, SampleIndex(0), DeviceTick(0));
    ASSERT_TRUE(t.addSegment(SampleIndex(0), 1'000'000).ok());     // 0..500000 -> 0..0.5 s
    ASSERT_TRUE(t.addSegment(SampleIndex(500'000), 4'000'000).ok());
    t.extendTo(SampleIndex(1'000'000));

    // 0.4 s falls inside the first segment: 0.4 * 1e6 == 400000.
    auto const inside = t.indexAtTime(RationalTime{4, 10});
    ASSERT_TRUE(inside.ok());
    EXPECT_EQ(inside->value, 400'000u);

    // 0.6 s falls inside the second: 0.5 s + 0.1 s at 4 MHz == 500000 + 400000.
    auto const second = t.indexAtTime(RationalTime{6, 10});
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second->value, 900'000u);

    // Past the end resolves to the furthest known position rather than an error,
    // so a view scrolling off the right edge does not have to special-case it.
    auto const beyond = t.indexAtTime(RationalTime{100, 1});
    ASSERT_TRUE(beyond.ok());
    EXPECT_EQ(beyond->value, 1'000'000u);
}

TEST(TimelineTest, IndexAtTimeRejectsInvalidInput) {
    auto t = singleRate(1'000'000);
    EXPECT_FALSE(t.indexAtTime(RationalTime{1, 0}).ok());     // zero denominator
    EXPECT_FALSE(t.indexAtTime(RationalTime{-1, 1}).ok());    // before the origin
}

TEST(TimelineTest, SecondsAtIsExplicitlyLossy) {
    auto t = singleRate(3);
    // 999999 samples at 3 Hz is exactly 333333 s; the double convenience accessor
    // must agree to within a double's precision, and the exact value must still be
    // available from timeAt().
    EXPECT_NEAR(t.secondsAt(SampleIndex(999'999)), 333'333.0, 1e-6);
    auto const exact = t.timeAt(SampleIndex(999'999));
    ASSERT_TRUE(exact.ok());
    EXPECT_EQ(exact->numerator, 333'333u);
    EXPECT_EQ(exact->denominator, 1u);
}

TEST(TimelineTest, EmptyTimelineIsValidButUnusable) {
    // A capture that has not produced a block yet legitimately has no rate segment.
    // That state is valid; what must NOT happen is it silently answering as though
    // the rate were zero.
    Timeline t(Timebase{}, SampleIndex(0), DeviceTick(0));
    EXPECT_TRUE(t.validate().ok());
    EXPECT_EQ(t.sampleCount(), 0u);
    EXPECT_FALSE(t.timeAt(SampleIndex(0)).ok());
    EXPECT_FALSE(t.indexAtTime(RationalTime{1, 1000}).ok());
}

TEST(TimelineTest, ZeroRateSegmentIsRejectedAtConstruction) {
    Timeline t(Timebase{}, SampleIndex(0), DeviceTick(0));
    // A zero rate would make every later position a division by zero. Refusing it
    // here means validate() can never be handed a timeline it has to reject.
    EXPECT_FALSE(t.addSegment(SampleIndex(0), 0).ok());

    ASSERT_TRUE(t.addSegment(SampleIndex(0), 1'000'000).ok());
    t.extendTo(SampleIndex(10));
    EXPECT_TRUE(t.validate().ok());
}

}  // namespace
