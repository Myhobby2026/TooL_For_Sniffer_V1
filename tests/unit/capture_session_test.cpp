// -----------------------------------------------------------------------------
// capture_session_test.cpp -- the capture path end to end.
//
// FakeCaptureDevice.pump() is synchronous, so these tests drive a whole capture
// deterministically: no timers, no sleeps to tune, nothing that flakes on a loaded
// machine. The device is fake; the session, pipeline, storage and timeline under
// test are the real ones.
// -----------------------------------------------------------------------------
#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

#include "usn/core/capture_session.h"
#include "usn/test/test_fixtures.h"
#include "usn/trigger/trigger_node.h"

using usn::CaptureConfiguration;
using usn::DeviceCapabilities;
using usn::ErrorCode;
using usn::SampleIndex;
using usn::core::AppConfig;
using usn::core::CaptureSession;
using usn::core::DiagnosticsLog;
using usn::core::SessionOptions;
using usn::core::SessionState;
using usn::core::StorageTarget;
using usn::hal::FakeCaptureDevice;
using usn::test::makeFakeDeviceOptions;
using usn::test::makeSupportedCaptureConfiguration;
using usn::test::makeTeensyLikeCapabilities;
using usn::test::makeUncharacterisedCapabilities;
using usn::trigger::TriggerNode;

namespace {

constexpr std::uint32_t kSamplesPerBlock = 256;

std::uint64_t totalDiagnosticCount(const DiagnosticsLog& log, ErrorCode const code) {
    std::uint64_t total = 0;
    for (const auto& event : log.snapshot()) {
        if (event.code == code) {
            total += event.count;
        }
    }
    return total;
}

// Waits for a condition that another thread establishes, with a bounded wait. Used
// only where the pipeline legitimately finishes work asynchronously.
template <typename Predicate>
bool waitFor(Predicate predicate, int const timeoutMs = 5000) {
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

AppConfig defaultSessionConfig() {
    AppConfig config;
    config.capture.channelCount = 16;
    config.capture.strideBytes = 2;
    config.capture.sampleRateHz = 1'000'000;
    return config;
}

// CaptureSession copies its AppConfig at construction, so a test that wants a
// different configuration has to supply it here rather than poke at a member
// afterwards -- poking afterwards would silently test the defaults instead.
struct Fixture {
    DiagnosticsLog log;
    AppConfig config;
    SessionOptions options;
    FakeCaptureDevice device{makeFakeDeviceOptions(kSamplesPerBlock)};
    CaptureSession session;

    explicit Fixture(AppConfig cfg = defaultSessionConfig())
        : config(std::move(cfg)), session(log, config, options) {}

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
};

TEST(CaptureSessionTest, LifecycleIsEnforcedInOrder) {
    Fixture f;
    EXPECT_EQ(f.session.state(), SessionState::Idle);
    EXPECT_FALSE(f.session.start().ok());                       // nothing attached
    EXPECT_FALSE(f.session.prepare(makeSupportedCaptureConfiguration()).ok());

    ASSERT_TRUE(f.session.attach(f.device).ok());
    EXPECT_EQ(f.session.state(), SessionState::Attached);
    EXPECT_FALSE(f.session.start().ok());                       // not prepared
    EXPECT_FALSE(f.session.arm(TriggerNode::always()).ok());    // not prepared

    ASSERT_TRUE(f.session.prepare(makeSupportedCaptureConfiguration()).ok());
    EXPECT_EQ(f.session.state(), SessionState::Prepared);
    EXPECT_TRUE(f.session.hasCapabilities());

    ASSERT_TRUE(f.session.start().ok());
    EXPECT_EQ(f.session.state(), SessionState::Running);
    EXPECT_FALSE(f.session.prepare(makeSupportedCaptureConfiguration()).ok());  // already running

    ASSERT_TRUE(f.session.stop().ok());
    EXPECT_EQ(f.session.state(), SessionState::Stopped);
    // Stopping twice is not an error; the second call has nothing to do.
    EXPECT_TRUE(f.session.stop().ok());
}

TEST(CaptureSessionTest, CapabilitiesComeFromTheDeviceNotFromATable) {
    Fixture f;
    ASSERT_TRUE(f.session.attach(f.device).ok());
    ASSERT_TRUE(f.session.prepare(makeSupportedCaptureConfiguration()).ok());
    const auto& caps = f.session.capabilities();
    EXPECT_EQ(caps.deviceName, "FakeTeensy41");
    EXPECT_EQ(caps.channelCountMax, 16u);
    EXPECT_FALSE(caps.rateEnvelope.empty());
    // The link has not been measured, and the session must not invent a number.
    EXPECT_EQ(caps.measuredLinkBytesPerSec, 0u);
}

TEST(CaptureSessionTest, RequestsOutsideTheReportedEnvelopeAreRefusedOnTheHost) {
    Fixture f;
    ASSERT_TRUE(f.session.attach(f.device).ok());

    auto tooFast = makeSupportedCaptureConfiguration();
    tooFast.sampleRateHz = 500'000'000;    // far beyond anything reported
    auto const rateStatus = f.session.prepare(tooFast);
    EXPECT_FALSE(rateStatus.ok());
    EXPECT_EQ(rateStatus.code(), ErrorCode::SampleRateUnsupported);
    EXPECT_EQ(f.session.state(), SessionState::Attached);

    auto tooMany = makeSupportedCaptureConfiguration();
    tooMany.channelCount = 32;
    tooMany.channelMask = 0xFFFFFFFFu;
    EXPECT_EQ(f.session.prepare(tooMany).code(), ErrorCode::ChannelCountUnsupported);

    auto rle = makeSupportedCaptureConfiguration();
    rle.enableRle = true;                  // the fake device reports rleSupported = false
    EXPECT_EQ(f.session.prepare(rle).code(), ErrorCode::DeviceUnsupported);
}

TEST(CaptureSessionTest, AnUncharacterisedDeviceIsRefusedRatherThanGuessed) {
    Fixture f;
    f.device.setCapabilitiesOverride(makeUncharacterisedCapabilities());
    ASSERT_TRUE(f.session.attach(f.device).ok());
    auto const status = f.session.prepare(makeSupportedCaptureConfiguration());
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), ErrorCode::SampleRateUnsupported);
    // The message has to say "we do not know", not "you asked for too much".
    EXPECT_NE(std::string(status.message()).find("not been characterised"), std::string::npos);
}

TEST(CaptureSessionTest, StrideTooNarrowForTheEnabledChannelsIsRefused) {
    auto config = defaultSessionConfig();
    config.capture.strideBytes = 1;        // cannot hold 16 channels
    Fixture f{config};
    ASSERT_TRUE(f.session.attach(f.device).ok());
    EXPECT_EQ(f.session.prepare(makeSupportedCaptureConfiguration()).code(),
              ErrorCode::StrideUnsupported);
}

TEST(CaptureSessionTest, EndToEndCaptureStoresEverySample) {
    Fixture f;
    ASSERT_TRUE(f.session.attach(f.device).ok());
    ASSERT_TRUE(f.session.prepare(makeSupportedCaptureConfiguration()).ok());
    ASSERT_TRUE(f.session.start().ok());

    constexpr std::size_t kBlocks = 20;
    EXPECT_EQ(f.device.pump(kBlocks), kBlocks);
    ASSERT_TRUE(f.session.stop().ok());

    auto const stats = f.session.statistics();
    EXPECT_EQ(stats.blocksReceived, static_cast<std::uint64_t>(kBlocks));
    EXPECT_EQ(stats.blocksStored, static_cast<std::uint64_t>(kBlocks));
    EXPECT_EQ(stats.blocksRejected, 0u);
    EXPECT_EQ(stats.samplesStored, static_cast<std::uint64_t>(kBlocks) * kSamplesPerBlock);
    EXPECT_EQ(stats.bytesStored, stats.samplesStored * 2);
    EXPECT_EQ(stats.gapsDetected, 0u);
    EXPECT_EQ(stats.missingSamples, 0u);
    EXPECT_TRUE(stats.complete);

    // Storage and the timeline must agree; they are updated from the same call.
    auto* storage = f.session.storage();
    ASSERT_NE(storage, nullptr);
    auto const range = storage->coveredRange();
    EXPECT_EQ(range.sampleCount(), stats.samplesStored);
    EXPECT_EQ(f.session.timeline().endExclusive().value, range.endExclusive.value);
    EXPECT_TRUE(f.session.timeline().isContiguous());
    EXPECT_EQ(storage->statistics().bytesDurable, storage->statistics().bytesWritten);
    EXPECT_FALSE(storage->isOpen());       // stop() closed it

    // The diagnostics log is the integrity record: a clean capture leaves it clean.
    EXPECT_TRUE(f.log.isClean());
    EXPECT_EQ(f.session.pipelineCounters().ingressDropped, 0u);
}

TEST(CaptureSessionTest, StoredSamplesReadBackExactly) {
    Fixture f;
    ASSERT_TRUE(f.session.attach(f.device).ok());
    ASSERT_TRUE(f.session.prepare(makeSupportedCaptureConfiguration()).ok());
    ASSERT_TRUE(f.session.start().ok());
    ASSERT_EQ(f.device.pump(4), 4u);
    ASSERT_TRUE(f.session.stop().ok());

    auto* storage = f.session.storage();
    ASSERT_NE(storage, nullptr);
    std::vector<std::byte> buffer(64 * 2);
    auto const read = storage->read(SampleIndex(300), 64, buffer);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(*read, 64u);

    // FakeDeviceOptions::Pattern::Counter is documented as making lost or duplicated
    // samples obvious: the word at sample index i is i. At a 2-byte stride the stored
    // value is therefore i truncated to 16 bits, little-endian. A wrong byte order, a
    // wrong offset, a duplicated block or a shifted timeline all break this at once.
    for (std::uint32_t i = 0; i < 64; ++i) {
        auto const low = static_cast<std::uint8_t>(buffer[i * 2]);
        auto const high = static_cast<std::uint8_t>(buffer[i * 2 + 1]);
        auto const got = static_cast<std::uint16_t>(low | (static_cast<std::uint16_t>(high) << 8));
        auto const expected = static_cast<std::uint16_t>((300u + i) & 0xFFFFu);
        EXPECT_EQ(got, expected) << "sample " << i;
    }

    // Reading the same range twice must give identical bytes: the read path holds no
    // state that a second call could disturb.
    std::vector<std::byte> again(64 * 2);
    ASSERT_TRUE(storage->read(SampleIndex(300), 64, again).ok());
    EXPECT_EQ(buffer, again);
}

TEST(CaptureSessionTest, ASampleIndexGapStaysAGap) {
    Fixture f;
    ASSERT_TRUE(f.session.attach(f.device).ok());
    ASSERT_TRUE(f.session.prepare(makeSupportedCaptureConfiguration()).ok());
    ASSERT_TRUE(f.session.start().ok());

    ASSERT_EQ(f.device.pump(3), 3u);
    f.device.injectSampleIndexGap(1000);
    ASSERT_EQ(f.device.pump(3), 3u);
    ASSERT_TRUE(f.session.stop().ok());

    auto const stats = f.session.statistics();
    EXPECT_EQ(stats.blocksStored, 6u);
    EXPECT_EQ(stats.gapsDetected, 1u);
    EXPECT_EQ(stats.missingSamples, 1000u);
    // A capture with a hole in it is not complete, and must not claim to be.
    EXPECT_FALSE(stats.complete);

    const auto& timeline = f.session.timeline();
    EXPECT_FALSE(timeline.isContiguous());
    ASSERT_EQ(timeline.gaps().size(), 1u);
    EXPECT_EQ(timeline.gaps()[0].missingSamples, 1000u);
    EXPECT_EQ(timeline.gaps()[0].firstMissing.value, 3u * kSamplesPerBlock);
    EXPECT_EQ(timeline.gaps()[0].afterGap.value, 3u * kSamplesPerBlock + 1000u);

    // The gap is in the integrity record too, not only in the timeline.
    EXPECT_GT(totalDiagnosticCount(f.log, ErrorCode::SampleIndexGap), 0u);
}

TEST(CaptureSessionTest, DeviceReportedOverflowReachesTheDiagnostics) {
    Fixture f;
    ASSERT_TRUE(f.session.attach(f.device).ok());
    ASSERT_TRUE(f.session.prepare(makeSupportedCaptureConfiguration()).ok());
    ASSERT_TRUE(f.session.start().ok());
    f.device.injectDeviceOverflow(2);
    ASSERT_EQ(f.device.pump(5), 5u);
    ASSERT_TRUE(f.session.stop().ok());

    // The device flagged an overflow and said so itself. Whether that also produced
    // an index gap depends on the device, but either way it must not pass unnoticed.
    EXPECT_FALSE(f.log.isClean());
    EXPECT_GT(totalDiagnosticCount(f.log, ErrorCode::DmaOverflow), 0u);
}

TEST(CaptureSessionTest, SequenceRenumberingAloneDoesNotCorruptTheCapture) {
    // Sequence-number continuity is checked by the wire codec, not here: the session
    // consumes decoded blocks, so by the time it sees one the sequence has already
    // been validated. A renumbering with contiguous SAMPLE indices therefore means
    // no sample data was lost, and the session says so rather than inventing a gap.
    // Pinned because the opposite behaviour -- reporting a sample gap that did not
    // happen -- would make the timeline lie about continuity.
    Fixture f;
    ASSERT_TRUE(f.session.attach(f.device).ok());
    ASSERT_TRUE(f.session.prepare(makeSupportedCaptureConfiguration()).ok());
    ASSERT_TRUE(f.session.start().ok());
    f.device.injectSequenceGap(5);
    ASSERT_EQ(f.device.pump(4), 4u);
    ASSERT_TRUE(f.session.stop().ok());

    auto const stats = f.session.statistics();
    EXPECT_EQ(stats.blocksStored, 4u);
    EXPECT_EQ(stats.gapsDetected, 0u);
    EXPECT_EQ(stats.missingSamples, 0u);
    EXPECT_TRUE(f.session.timeline().isContiguous());
}

TEST(CaptureSessionTest, AbortMarksTheCaptureIncomplete) {
    Fixture f;
    ASSERT_TRUE(f.session.attach(f.device).ok());
    ASSERT_TRUE(f.session.prepare(makeSupportedCaptureConfiguration()).ok());
    ASSERT_TRUE(f.session.start().ok());
    f.device.pump(5);
    ASSERT_TRUE(f.session.abort().ok());
    EXPECT_EQ(f.session.state(), SessionState::Aborted);
    EXPECT_FALSE(f.session.statistics().complete);
}

TEST(CaptureSessionTest, SampleLimitEndsTheCaptureAndSaysWhy) {
    auto config = defaultSessionConfig();
    config.capture.maxSamples = 3 * kSamplesPerBlock;
    Fixture f{config};
    ASSERT_TRUE(f.session.attach(f.device).ok());
    ASSERT_TRUE(f.session.prepare(makeSupportedCaptureConfiguration()).ok());
    ASSERT_TRUE(f.session.start().ok());

    ASSERT_EQ(f.device.pump(3), 3u);
    // The limit is noticed on the storage lane's worker thread, so this is the one
    // place a bounded wait is genuinely required.
    EXPECT_TRUE(waitFor([&f] { return f.session.stopRequested(); }))
        << "the configured sample limit did not end the capture";
    EXPECT_NE(f.session.stopReason().find("maxSamples"), std::string::npos);

    ASSERT_TRUE(f.session.stop().ok());
    auto const stats = f.session.statistics();
    EXPECT_EQ(stats.samplesStored, 3u * kSamplesPerBlock);
    EXPECT_EQ(stats.gapsDetected, 0u);
    EXPECT_EQ(stats.blocksRejected, 0u);
    // Ending because a configured limit was reached is a planned stop: the capture
    // is complete, and the reason is recorded as information rather than as a fault.
    EXPECT_TRUE(stats.complete);
    EXPECT_GT(totalDiagnosticCount(f.log, ErrorCode::Ok), 0u);
}

TEST(CaptureSessionTest, TriggerIsCompiledAndArmedThroughTheDevice) {
    Fixture f;
    ASSERT_TRUE(f.session.attach(f.device).ok());
    ASSERT_TRUE(f.session.prepare(makeSupportedCaptureConfiguration()).ok());

    auto ast = TriggerNode::rising(usn::ChannelId{3});
    ASSERT_TRUE(f.session.arm(ast).ok());
    EXPECT_EQ(f.session.state(), SessionState::Armed);
    auto* engine = f.device.triggerEngine();
    ASSERT_NE(engine, nullptr);
    EXPECT_TRUE(engine->isArmed());

    // Arming twice is idempotent rather than an error: the GUI offers it as a toggle.
    EXPECT_TRUE(f.session.arm(ast).ok());

    ASSERT_TRUE(f.session.start().ok());
    ASSERT_EQ(f.device.pump(2), 2u);
    ASSERT_TRUE(f.session.stop().ok());

    ASSERT_TRUE(f.session.disarm().ok());
    EXPECT_FALSE(engine->isArmed());
}

TEST(CaptureSessionTest, DetachReleasesEverything) {
    Fixture f;
    ASSERT_TRUE(f.session.attach(f.device).ok());
    ASSERT_TRUE(f.session.prepare(makeSupportedCaptureConfiguration()).ok());
    f.session.detach();
    EXPECT_EQ(f.session.state(), SessionState::Idle);
    EXPECT_EQ(f.session.device(), nullptr);
    EXPECT_EQ(f.session.storage(), nullptr);
    EXPECT_EQ(f.session.pipeline(), nullptr);
    EXPECT_FALSE(f.session.hasCapabilities());
    EXPECT_EQ(f.session.statistics().blocksStored, 0u);
}

TEST(CaptureSessionTest, ForwardedDiagnosticsLandInTheLog) {
    Fixture f;
    usn::DiagnosticEvent event;
    event.severity = usn::DiagnosticSeverity::Warning;
    event.code = ErrorCode::UsbTimeout;
    event.message = "forwarded from the transport";
    f.session.onDiagnostic(event);
    EXPECT_GT(totalDiagnosticCount(f.log, ErrorCode::UsbTimeout), 0u);
}

TEST(CaptureSessionTest, SessionStateNamesAreStable) {
    // The GUI shows these strings; a rename is a user-visible change, so it is pinned.
    EXPECT_EQ(usn::core::nameOf(SessionState::Idle), "idle");
    EXPECT_EQ(usn::core::nameOf(SessionState::Running), "running");
    EXPECT_EQ(usn::core::nameOf(SessionState::Flushing), "flushing");
    EXPECT_EQ(usn::core::nameOf(SessionState::Stopped), "stopped");
}

}  // namespace
