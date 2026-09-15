// -----------------------------------------------------------------------------
// model_test.cpp -- tests for usn::model (Time, Sample, Channel, Capabilities).
// -----------------------------------------------------------------------------
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include "usn/model/capabilities.h"
#include "usn/model/channel.h"
#include "usn/model/sample.h"
#include "usn/model/time.h"

// --- Strong Integer Types ----------------------------------------------------

TEST(ModelTest, StrongTypeNonConvertibility) {
    static_assert(!std::is_convertible_v<uint64_t, usn::SampleIndex>);
    static_assert(!std::is_convertible_v<usn::SampleIndex, uint64_t>);
    static_assert(!std::is_convertible_v<usn::SampleIndex, usn::DeviceTick>);
    static_assert(!std::is_convertible_v<usn::ChannelId, uint16_t>);

    usn::SampleIndex s1{100};
    usn::SampleIndex s2{200};
    EXPECT_LT(s1, s2);
    EXPECT_NE(s1, s2);

    usn::ChannelId ch1{3};
    usn::ChannelId ch2{3};
    EXPECT_EQ(ch1, ch2);
    EXPECT_TRUE(ch1.isValid());
    EXPECT_FALSE(usn::ChannelId::invalid().isValid());
}

// --- RationalTime & Conversions ----------------------------------------------

TEST(ModelTest, RationalTimeOperationsAndComparison) {
    usn::RationalTime t1{1, 3};
    usn::RationalTime t2{2, 6};
    EXPECT_EQ(t1, t2);

    usn::RationalTime half{1, 2};
    usn::RationalTime third{1, 3};
    EXPECT_LT(third, half);
    EXPECT_GT(half, third);

    usn::RationalTime negative{-1, 2};
    EXPECT_LT(negative, third);

    // Exact conversion without floating-point drift
    auto timeFromIdx = usn::sampleIndexToTime(usn::SampleIndex{1000},
                                             usn::SampleIndex{5000},
                                             1'000'000);
    ASSERT_TRUE(timeFromIdx.ok());
    // (5000 - 1000) / 1'000'000 = 4000 / 1000000 = 1 / 250 s = 4 ms
    EXPECT_DOUBLE_EQ(timeFromIdx->toSeconds(), 0.004);

    usn::Timebase tb{1, 1'000'000'000};  // nanosecond ticks
    auto timeFromTick = usn::tickToTime(usn::DeviceTick{100},
                                        usn::DeviceTick{600},
                                        tb);
    ASSERT_TRUE(timeFromTick.ok());
    // 500 ns = 5e-7 s
    EXPECT_DOUBLE_EQ(timeFromTick->toSeconds(), 500e-9);
}

// --- SampleBlockView & OwningSampleBlock --------------------------------------

TEST(ModelTest, SampleBlockViewStride1) {
    usn::BlockHeader hdr{};
    hdr.firstSampleIndex = usn::SampleIndex{100};
    hdr.sampleCount = 4;
    hdr.channelCount = 8;
    hdr.strideBytes = 1;

    std::vector<std::byte> payload = {
        std::byte{0x01}, std::byte{0x02}, std::byte{0x04}, std::byte{0x08}
    };

    usn::SampleBlockView view(hdr, payload);
    EXPECT_TRUE(view.validate().ok());
    EXPECT_EQ(view.size(), 4u);
    EXPECT_EQ(view.wordAt(0), 0x01u);
    EXPECT_EQ(view.wordAt(1), 0x02u);
    EXPECT_EQ(view.wordAt(2), 0x04u);
    EXPECT_EQ(view.wordAt(3), 0x08u);

    EXPECT_TRUE(view.bitAt(0, 0));
    EXPECT_FALSE(view.bitAt(1, 0));
    EXPECT_TRUE(view.bitAt(1, 1));
    EXPECT_TRUE(view.bitAt(2, 2));
    EXPECT_TRUE(view.bitAt(3, 3));

    EXPECT_EQ(view.sampleIndexOf(2), usn::SampleIndex{102});
}

TEST(ModelTest, SampleBlockViewStride2And4) {
    // Stride 2
    usn::BlockHeader hdr2{};
    hdr2.firstSampleIndex = usn::SampleIndex{0};
    hdr2.sampleCount = 2;
    hdr2.channelCount = 16;
    hdr2.strideBytes = 2;
    std::vector<std::byte> payload2 = {
        std::byte{0x34}, std::byte{0x12},  // 0x1234
        std::byte{0xCD}, std::byte{0xAB}   // 0xABCD
    };
    usn::SampleBlockView view2(hdr2, payload2);
    EXPECT_TRUE(view2.validate().ok());
    EXPECT_EQ(view2.wordAt(0), 0x1234u);
    EXPECT_EQ(view2.wordAt(1), 0xABCDu);

    // Stride 4
    usn::BlockHeader hdr4{};
    hdr4.firstSampleIndex = usn::SampleIndex{0};
    hdr4.sampleCount = 1;
    hdr4.channelCount = 32;
    hdr4.strideBytes = 4;
    std::vector<std::byte> payload4 = {
        std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12}
    };
    usn::SampleBlockView view4(hdr4, payload4);
    EXPECT_TRUE(view4.validate().ok());
    EXPECT_EQ(view4.wordAt(0), 0x12345678u);
}

TEST(ModelTest, OwningSampleBlockMove) {
    usn::BlockHeader hdr{};
    hdr.firstSampleIndex = usn::SampleIndex{50};
    hdr.sampleCount = 2;
    hdr.channelCount = 8;
    hdr.strideBytes = 1;

    std::vector<std::byte> payload = {std::byte{0xAA}, std::byte{0x55}};
    usn::OwningSampleBlock block(hdr, std::move(payload));

    usn::OwningSampleBlock moved = std::move(block);
    EXPECT_EQ(moved.header().firstSampleIndex, usn::SampleIndex{50});
    EXPECT_EQ(moved.payload().size(), 2u);
    EXPECT_EQ(moved.view().wordAt(0), 0xAAu);
}

// --- DeviceCapabilities ------------------------------------------------------

TEST(ModelTest, DeviceCapabilitiesEnvelope) {
    usn::DeviceCapabilities caps{};
    caps.channelCountMax = 16;
    caps.rateEnvelope = {
        {8, 20'000'000, true},
        {16, 10'000'000, true}
    };

    EXPECT_TRUE(caps.supports(8, 20'000'000));
    EXPECT_TRUE(caps.supports(8, 10'000'000));
    EXPECT_FALSE(caps.supports(8, 25'000'000));

    EXPECT_TRUE(caps.supports(16, 10'000'000));
    EXPECT_FALSE(caps.supports(16, 20'000'000));
}
