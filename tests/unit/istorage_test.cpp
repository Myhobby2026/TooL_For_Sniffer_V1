// -----------------------------------------------------------------------------
// istorage_test.cpp -- the storage contract.
//
// The point of these tests is that the rules are enforced by the BACKEND, not by
// whoever calls it. When the chunked .usn writer lands in Phase 4 it has to pass
// this same suite unchanged; that is what makes IStorage worth having now.
// -----------------------------------------------------------------------------
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "usn/core/istorage.h"
#include "usn/test/test_fixtures.h"

using usn::ErrorCode;
using usn::SampleIndex;
using usn::core::InMemoryStorage;
using usn::core::IStorage;
using usn::core::StorageMetadata;
using usn::core::StorageTarget;
using usn::test::makeCounterBlock;

namespace {

StorageMetadata metadata(std::uint16_t const channels = 16, std::uint8_t const stride = 2,
                         std::uint64_t const rate = 1'000'000) {
    StorageMetadata meta;
    meta.formatName = "usn-inmemory";
    meta.formatVersion = 1;
    meta.strideBytes = stride;
    meta.channelCount = static_cast<std::uint8_t>(channels);
    meta.channelMask = (std::uint64_t{1} << channels) - 1;
    meta.sampleRateHz = rate;
    meta.deviceName = "FakeTeensy41";
    meta.firmwareVersion = "0.1.0-fake";
    return meta;
}

TEST(StorageFactoryTest, InMemoryIsAvailableAndUsnFileIsHonestAboutNotBeing) {
    auto memory = IStorage::create(StorageTarget::InMemory);
    ASSERT_NE(memory, nullptr);
    EXPECT_EQ(memory->target(), StorageTarget::InMemory);

    // Phase 4 delivers the .usn container. Returning a null pointer with a real
    // error at the call site is honest; silently substituting an in-memory backend
    // would let a user believe their capture is durable when it is not.
    EXPECT_EQ(IStorage::create(StorageTarget::UsnFile), nullptr);
}

TEST(StorageTest, OpenValidatesMetadata) {
    InMemoryStorage storage;
    EXPECT_FALSE(storage.open(metadata(16, 3)).ok());          // stride 3 is not a thing
    EXPECT_FALSE(storage.open(metadata(0)).ok());              // no channels
    EXPECT_FALSE(storage.open(metadata(33)).ok());             // above the supported maximum
    EXPECT_FALSE(storage.open(metadata(16, 2, 0)).ok());       // zero rate

    auto noName = metadata();
    noName.formatName.clear();
    EXPECT_FALSE(storage.open(noName).ok());

    // A mask whose popcount disagrees with channelCount would produce a file that
    // cannot be re-opened with the right channel mapping.
    auto badMask = metadata();
    badMask.channelMask = 0xFF;
    EXPECT_FALSE(storage.open(badMask).ok());

    EXPECT_TRUE(storage.open(metadata()).ok());
    EXPECT_TRUE(storage.isOpen());
    // Re-opening an open backend is an error, not a reset.
    EXPECT_FALSE(storage.open(metadata()).ok());
}

TEST(StorageTest, AppendRequiresOpen) {
    InMemoryStorage storage;
    auto block = makeCounterBlock(SampleIndex(0), 64);
    EXPECT_FALSE(storage.append(block.view()).ok());
}

TEST(StorageTest, AppendRejectsMismatchedGeometry) {
    InMemoryStorage storage;
    ASSERT_TRUE(storage.open(metadata(16, 2, 1'000'000)).ok());

    // A 1-byte stride cannot hold 16 enabled channels, so the model layer rejects
    // the block on its own terms before storage ever compares geometry. That is the
    // better answer, and the test asserts it rather than papering over it.
    auto narrowStride = makeCounterBlock(SampleIndex(0), 64, /*stride=*/1);
    EXPECT_EQ(storage.append(narrowStride.view()).code(), ErrorCode::StrideUnsupported);

    // A block that is internally consistent but does not match the storage geometry.
    auto wideStride = makeCounterBlock(SampleIndex(0), 64, /*stride=*/4);
    EXPECT_EQ(storage.append(wideStride.view()).code(), ErrorCode::ConfigurationError);

    auto wrongChannels = makeCounterBlock(SampleIndex(0), 64, 2, /*channelCount=*/8);
    EXPECT_EQ(storage.append(wrongChannels.view()).code(), ErrorCode::ConfigurationError);

    auto wrongRate = makeCounterBlock(SampleIndex(0), 64, 2, 16, /*rate=*/2'000'000);
    EXPECT_EQ(storage.append(wrongRate.view()).code(), ErrorCode::ConfigurationError);

    EXPECT_EQ(storage.statistics().blocksRejected, 4u);
    EXPECT_EQ(storage.statistics().blocksWritten, 0u);
}

TEST(StorageTest, AppendRejectsOverlapAndRewind) {
    InMemoryStorage storage;
    ASSERT_TRUE(storage.open(metadata()).ok());
    ASSERT_TRUE(storage.append(makeCounterBlock(SampleIndex(0), 100).view()).ok());

    // Rewriting history would corrupt every later index.
    EXPECT_EQ(storage.append(makeCounterBlock(SampleIndex(50), 100).view()).code(),
              ErrorCode::SampleIndexGap);
    EXPECT_EQ(storage.append(makeCounterBlock(SampleIndex(0), 100).view()).code(),
              ErrorCode::SampleIndexGap);
    EXPECT_EQ(storage.statistics().blocksWritten, 1u);
}

TEST(StorageTest, AHoleIsRecordedAndTheCaptureContinues) {
    InMemoryStorage storage;
    ASSERT_TRUE(storage.open(metadata()).ok());
    ASSERT_TRUE(storage.append(makeCounterBlock(SampleIndex(0), 100).view()).ok());

    // Losing packets mid-capture must not end the capture. The hole is recorded and
    // the data on both sides of it is kept, so a view can render the discontinuity
    // instead of pretending the samples were adjacent.
    auto const afterGap = storage.append(makeCounterBlock(SampleIndex(250), 100).view());
    ASSERT_TRUE(afterGap.ok()) << afterGap.message();

    auto const stats = storage.statistics();
    EXPECT_EQ(stats.blocksWritten, 2u);
    EXPECT_EQ(stats.holesRecorded, 1u);
    EXPECT_EQ(stats.missingSamples, 150u);

    ASSERT_EQ(storage.holes().size(), 1u);
    EXPECT_EQ(storage.holes()[0].firstMissing.value, 100u);
    EXPECT_EQ(storage.holes()[0].afterGap.value, 250u);
    EXPECT_EQ(storage.holes()[0].missingSamples(), 150u);

    // Two contiguous runs, and the overall extent spans both.
    auto const runs = storage.coveredRanges();
    ASSERT_EQ(runs.size(), 2u);
    EXPECT_EQ(runs[0].first.value, 0u);
    EXPECT_EQ(runs[0].endExclusive.value, 100u);
    EXPECT_EQ(runs[1].first.value, 250u);
    EXPECT_EQ(runs[1].endExclusive.value, 350u);
    EXPECT_EQ(storage.coveredRange().endExclusive.value, 350u);

    // Each side reads back correctly on its own...
    std::vector<std::byte> buffer(200);
    EXPECT_TRUE(storage.read(SampleIndex(0), 100, buffer).ok());
    EXPECT_TRUE(storage.read(SampleIndex(250), 100, buffer).ok());
    // ...and a read that spans the hole is refused rather than silently stitching
    // two non-adjacent runs together. The buffer is sized for the request so that
    // this asserts the hole check and not the length check.
    std::vector<std::byte> wide(250 * 2);
    auto const spanning = storage.read(SampleIndex(50), 250, wide);
    EXPECT_FALSE(spanning.ok());
    EXPECT_NE(std::string(spanning.status().message()).find("spans a hole"), std::string::npos);
}

TEST(StorageTest, ContiguousAppendsAccumulate) {
    InMemoryStorage storage;
    ASSERT_TRUE(storage.open(metadata()).ok());
    SampleIndex next(0);
    for (int i = 0; i < 10; ++i) {
        auto block = makeCounterBlock(next, 128);
        auto const appended = storage.append(block.view());
        ASSERT_TRUE(appended.ok());
        next = block.header().lastSampleIndexExclusive();
    }
    auto const stats = storage.statistics();
    EXPECT_EQ(stats.blocksWritten, 10u);
    EXPECT_EQ(stats.samplesWritten, 1280u);
    EXPECT_EQ(stats.bytesWritten, 2560u);
    EXPECT_EQ(stats.blocksRejected, 0u);
    auto const range = storage.coveredRange();
    EXPECT_EQ(range.first.value, 0u);
    EXPECT_EQ(range.endExclusive.value, 1280u);
    EXPECT_EQ(range.sampleCount(), 1280u);
}

TEST(StorageTest, ReadRoundTripsExactly) {
    InMemoryStorage storage;
    ASSERT_TRUE(storage.open(metadata(16, 2, 1'000'000)).ok());
    ASSERT_TRUE(storage.append(makeCounterBlock(SampleIndex(0), 256, 2).view()).ok());
    ASSERT_TRUE(storage.append(makeCounterBlock(SampleIndex(256), 256, 2).view()).ok());

    std::vector<std::byte> buffer(64 * 2);
    auto const read = storage.read(SampleIndex(200), 64, buffer);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(*read, 64u);

    // The counter pattern makes a wrong offset immediately visible.
    for (std::uint32_t i = 0; i < 64; ++i) {
        auto const expected = static_cast<std::uint16_t>(200 + i);
        auto const low = static_cast<std::uint8_t>(buffer[i * 2]);
        auto const high = static_cast<std::uint8_t>(buffer[i * 2 + 1]);
        auto const got = static_cast<std::uint16_t>(low | (static_cast<std::uint16_t>(high) << 8));
        EXPECT_EQ(got, expected) << "sample " << i;
    }
}

TEST(StorageTest, ReadRejectsOutOfRangeAndShortBuffers) {
    InMemoryStorage storage;
    ASSERT_TRUE(storage.open(metadata(16, 2)).ok());
    ASSERT_TRUE(storage.append(makeCounterBlock(SampleIndex(1000), 100, 2).view()).ok());

    std::vector<std::byte> buffer(200);
    EXPECT_FALSE(storage.read(SampleIndex(999), 1, buffer).ok());    // before the range
    EXPECT_FALSE(storage.read(SampleIndex(1050), 100, buffer).ok()); // past the end
    std::vector<std::byte> tiny(2);
    EXPECT_EQ(storage.read(SampleIndex(1000), 10, tiny).status().code(),
              ErrorCode::InvalidPacketLength);

    auto const zero = storage.read(SampleIndex(1000), 0, buffer);
    ASSERT_TRUE(zero.ok());
    EXPECT_EQ(*zero, 0u);
}

TEST(StorageTest, FlushAccountsForDurableBytes) {
    InMemoryStorage storage;
    ASSERT_TRUE(storage.open(metadata()).ok());
    ASSERT_TRUE(storage.append(makeCounterBlock(SampleIndex(0), 64).view()).ok());
    EXPECT_EQ(storage.statistics().bytesDurable, 0u);   // not yet acknowledged
    ASSERT_TRUE(storage.flush().ok());
    EXPECT_EQ(storage.statistics().bytesDurable, 128u);
    EXPECT_EQ(storage.statistics().flushes, 1u);
}

TEST(StorageTest, CloseFlushesAndIsIdempotent) {
    InMemoryStorage storage;
    ASSERT_TRUE(storage.open(metadata()).ok());
    ASSERT_TRUE(storage.append(makeCounterBlock(SampleIndex(0), 64).view()).ok());
    ASSERT_TRUE(storage.close().ok());
    EXPECT_FALSE(storage.isOpen());
    EXPECT_EQ(storage.statistics().bytesDurable, 128u);
    EXPECT_TRUE(storage.close().ok());     // closing twice is not an error
    EXPECT_FALSE(storage.append(makeCounterBlock(SampleIndex(64), 64).view()).ok());
}

TEST(StorageTest, InvalidBlockIsRejectedBeforeItIsStored) {
    InMemoryStorage storage;
    ASSERT_TRUE(storage.open(metadata()).ok());
    // A header claiming more samples than the payload holds must not be accepted:
    // storing it would make coveredRange() and every later index wrong.
    auto block = makeCounterBlock(SampleIndex(0), 64);
    block.header().sampleCount = 128;
    auto const status = storage.append(block.view());
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(storage.statistics().blocksWritten, 0u);
    EXPECT_EQ(storage.statistics().blocksRejected, 1u);
}

TEST(StorageTest, MetadataIsRetrievableUnchanged) {
    InMemoryStorage storage;
    auto const meta = metadata(8, 1, 5'000'000);
    ASSERT_TRUE(storage.open(meta).ok());
    const auto& stored = storage.metadata();
    EXPECT_EQ(stored.channelCount, 8);
    EXPECT_EQ(stored.strideBytes, 1);
    EXPECT_EQ(stored.sampleRateHz, 5'000'000u);
    EXPECT_EQ(stored.deviceName, "FakeTeensy41");
}

}  // namespace
