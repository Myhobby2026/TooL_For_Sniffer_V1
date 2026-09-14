// -----------------------------------------------------------------------------
// end_to_end_test.cpp -- device, pipeline, session, storage and timeline wired
// together the way the application wires them.
//
// The device is FakeCaptureDevice, whose pump() is synchronous, so a whole capture
// is reproducible run to run. Everything above the device is the real code.
// -----------------------------------------------------------------------------
#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

#include "usn/core/capture_session.h"
#include "usn/test/test_fixtures.h"

using usn::ErrorCode;
using usn::SampleIndex;
using usn::core::AppConfig;
using usn::core::CaptureSession;
using usn::core::DiagnosticsLog;
using usn::core::SessionOptions;
using usn::core::SessionState;
using usn::hal::FakeCaptureDevice;
using usn::test::makeFakeDeviceOptions;
using usn::test::makeSupportedCaptureConfiguration;

namespace {

constexpr std::uint32_t kSamplesPerBlock = 128;

AppConfig sessionConfig() {
    AppConfig config;
    config.capture.channelCount = 16;
    config.capture.strideBytes = 2;
    config.capture.sampleRateHz = 1'000'000;
    config.threading.ingressCapacity = 1024;
    config.threading.storageLaneCapacity = 64;
    return config;
}

struct Rig {
    DiagnosticsLog log;
    AppConfig config{sessionConfig()};
    SessionOptions options;
    FakeCaptureDevice device{makeFakeDeviceOptions(kSamplesPerBlock)};
    CaptureSession session{log, config, options};
};

TEST(EndToEndTest, LongCaptureStaysContiguousAndComplete) {
    Rig rig;
    ASSERT_TRUE(rig.session.attach(rig.device).ok());
    ASSERT_TRUE(rig.session.prepare(makeSupportedCaptureConfiguration()).ok());
    ASSERT_TRUE(rig.session.start().ok());

    // 500 blocks x 128 samples = 64000 samples, 128000 bytes of packed payload.
    constexpr std::size_t kBlocks = 500;
    EXPECT_EQ(rig.device.pump(kBlocks), kBlocks);
    ASSERT_TRUE(rig.session.stop().ok());

    auto const stats = rig.session.statistics();
    EXPECT_EQ(stats.blocksStored, static_cast<std::uint64_t>(kBlocks));
    EXPECT_EQ(stats.samplesStored, static_cast<std::uint64_t>(kBlocks) * kSamplesPerBlock);
    EXPECT_EQ(stats.bytesStored, stats.samplesStored * 2);
    EXPECT_EQ(stats.gapsDetected, 0u);
    EXPECT_EQ(stats.blocksRejected, 0u);
    EXPECT_TRUE(stats.complete);
    EXPECT_TRUE(rig.log.isClean()) << "a clean capture recorded diagnostics";

    // The timeline must span exactly the stored samples and answer positions exactly.
    const auto& timeline = rig.session.timeline();
    EXPECT_EQ(timeline.endExclusive().value, stats.samplesStored);
    EXPECT_TRUE(timeline.isContiguous());
    auto const lastTime = timeline.timeAt(SampleIndex(stats.samplesStored));
    ASSERT_TRUE(lastTime.ok());
    // 64000 samples at 1 MHz is exactly 0.064 s. timeAt() normalizes the rational,
    // so 64000/1000000 is stored reduced to 8/125; the assertion is on the reduced
    // form plus the cross-product, so it checks the value and not one particular
    // spelling of it.
    EXPECT_EQ(lastTime->numerator, 8);
    EXPECT_EQ(lastTime->denominator, 125u);
    EXPECT_EQ(static_cast<std::uint64_t>(lastTime->numerator) * 1'000'000u,
              lastTime->denominator * stats.samplesStored);
    EXPECT_EQ(lastTime->toSeconds(), 0.064);

    // Spot-check the stored payload at several offsets: the counter pattern means a
    // wrong offset, a duplicated block or a byte-order error all show up at once.
    auto* storage = rig.session.storage();
    ASSERT_NE(storage, nullptr);
    for (std::uint64_t at : {0ull, 1ull, 12'345ull, 63'999ull}) {
        std::vector<std::byte> buffer(2);
        auto const read = storage->read(SampleIndex(at), 1, buffer);
        ASSERT_TRUE(read.ok()) << "read at " << at << ": " << read.status().message();
        auto const low = static_cast<std::uint8_t>(buffer[0]);
        auto const high = static_cast<std::uint8_t>(buffer[1]);
        auto const got = static_cast<std::uint16_t>(low | (static_cast<std::uint16_t>(high) << 8));
        EXPECT_EQ(got, static_cast<std::uint16_t>(at & 0xFFFFu)) << "sample " << at;
    }
}

TEST(EndToEndTest, LostPacketsLeaveAHoleTheWholeSystemAgreesOn) {
    Rig rig;
    ASSERT_TRUE(rig.session.attach(rig.device).ok());
    ASSERT_TRUE(rig.session.prepare(makeSupportedCaptureConfiguration()).ok());
    ASSERT_TRUE(rig.session.start().ok());

    ASSERT_EQ(rig.device.pump(10), 10u);
    // The device loses 5000 samples worth of packets, then carries on.
    rig.device.injectSampleIndexGap(5000);
    ASSERT_EQ(rig.device.pump(10), 10u);
    ASSERT_TRUE(rig.session.stop().ok());

    auto const stats = rig.session.statistics();
    EXPECT_EQ(stats.blocksStored, 20u);
    EXPECT_EQ(stats.gapsDetected, 1u);
    EXPECT_EQ(stats.missingSamples, 5000u);
    EXPECT_FALSE(stats.complete);

    // Timeline, storage and diagnostics must all describe the same hole. A view that
    // rendered the samples as adjacent would be showing the user data that does not
    // exist.
    const auto& timeline = rig.session.timeline();
    ASSERT_EQ(timeline.gaps().size(), 1u);
    auto const& gap = timeline.gaps()[0];
    EXPECT_EQ(gap.firstMissing.value, 10u * kSamplesPerBlock);
    EXPECT_EQ(gap.afterGap.value, 10u * kSamplesPerBlock + 5000u);

    auto* storage = rig.session.storage();
    ASSERT_NE(storage, nullptr);
    ASSERT_EQ(storage->holes().size(), 1u);
    EXPECT_EQ(storage->holes()[0].firstMissing.value, gap.firstMissing.value);
    EXPECT_EQ(storage->holes()[0].afterGap.value, gap.afterGap.value);
    auto const runs = storage->coveredRanges();
    ASSERT_EQ(runs.size(), 2u);
    EXPECT_EQ(runs[0].sampleCount(), 10u * kSamplesPerBlock);
    EXPECT_EQ(runs[1].sampleCount(), 10u * kSamplesPerBlock);

    // Both sides of the hole still read back correctly...
    std::vector<std::byte> buffer(2);
    EXPECT_TRUE(storage->read(SampleIndex(100), 1, buffer).ok());
    EXPECT_TRUE(storage->read(SampleIndex(gap.afterGap.value + 100), 1, buffer).ok());
    // ...and a read across the hole is refused rather than stitched together.
    std::vector<std::byte> wide(6000);
    EXPECT_FALSE(storage->read(SampleIndex(1000), 3000, wide).ok());

    EXPECT_GT(rig.log.size(), 0u);
}

TEST(EndToEndTest, StoppingMidCaptureFlushesWhatWasAlreadyStored) {
    Rig rig;
    ASSERT_TRUE(rig.session.attach(rig.device).ok());
    ASSERT_TRUE(rig.session.prepare(makeSupportedCaptureConfiguration()).ok());
    ASSERT_TRUE(rig.session.start().ok());
    ASSERT_EQ(rig.device.pump(7), 7u);

    // Flushing is a distinct state from Stopped: a capture cut short must still
    // account for the blocks it did store.
    ASSERT_TRUE(rig.session.stop().ok());
    EXPECT_EQ(rig.session.state(), SessionState::Stopped);

    auto const stats = rig.session.statistics();
    EXPECT_EQ(stats.blocksStored, 7u);
    EXPECT_EQ(stats.samplesStored, 7u * kSamplesPerBlock);
    EXPECT_TRUE(stats.complete);
    auto* storage = rig.session.storage();
    ASSERT_NE(storage, nullptr);
    EXPECT_EQ(storage->statistics().bytesDurable, storage->statistics().bytesWritten);
}

TEST(EndToEndTest, DeviceResetIsVisibleToTheSession) {
    Rig rig;
    ASSERT_TRUE(rig.session.attach(rig.device).ok());
    ASSERT_TRUE(rig.session.prepare(makeSupportedCaptureConfiguration()).ok());
    ASSERT_TRUE(rig.session.start().ok());
    ASSERT_EQ(rig.device.pump(3), 3u);
    rig.device.injectDeviceReset();
    rig.device.pump(3);
    ASSERT_TRUE(rig.session.stop().ok());
    // A device that reset mid-capture has told the host about it; the host must not
    // carry on as though the stream were continuous and uneventful.
    EXPECT_FALSE(rig.log.isClean());
}

TEST(EndToEndTest, PipelineCountersAgreeWithSessionStatistics) {
    Rig rig;
    ASSERT_TRUE(rig.session.attach(rig.device).ok());
    ASSERT_TRUE(rig.session.prepare(makeSupportedCaptureConfiguration()).ok());
    ASSERT_TRUE(rig.session.start().ok());
    ASSERT_EQ(rig.device.pump(50), 50u);
    ASSERT_TRUE(rig.session.stop().ok());

    auto const pipeline = rig.session.pipelineCounters();
    auto const stats = rig.session.statistics();
    EXPECT_EQ(pipeline.blocksIn, 50u);
    EXPECT_EQ(pipeline.samplesIn, 50u * kSamplesPerBlock);
    EXPECT_EQ(pipeline.ingressDropped, 0u);
    ASSERT_EQ(pipeline.lanes.size(), 1u);
    // Every block that entered the pipeline was delivered to the storage lane.
    EXPECT_EQ(pipeline.lanes[0].queued, 50u);
    EXPECT_EQ(pipeline.lanes[0].delivered, 50u);
    EXPECT_EQ(pipeline.lanes[0].dropped, 0u);
    EXPECT_EQ(stats.blocksReceived, pipeline.lanes[0].delivered);
}

}  // namespace
