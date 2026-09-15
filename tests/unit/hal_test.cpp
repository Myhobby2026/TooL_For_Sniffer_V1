// -----------------------------------------------------------------------------
// hal_test.cpp -- tests for usn::hal (FakeCaptureDevice, DeviceManager, Probes).
// -----------------------------------------------------------------------------
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "usn/hal/device_manager.h"
#include "usn/hal/fake_capture_device.h"
#include "usn/test/test_fixtures.h"

namespace {

class TestSink final : public usn::hal::ISampleSink {
public:
    usn::hal::Backpressure onBlock(usn::OwningSampleBlock block) override {
        m_blocks.push_back(std::move(block));
        return usn::hal::Backpressure::Accepted;
    }

    void onDiagnostic(usn::DiagnosticEvent diag) override {
        m_diagnostics.push_back(diag);
    }

    std::vector<usn::OwningSampleBlock> m_blocks;
    std::vector<usn::DiagnosticEvent> m_diagnostics;
};

}  // namespace

TEST(HalTest, FakeCaptureDeviceLifecycle) {
    auto opts = usn::test::makeFakeDeviceOptions(256);
    usn::hal::FakeCaptureDevice device(opts);

    EXPECT_EQ(device.state(), usn::hal::DeviceState::Disconnected);

    EXPECT_TRUE(device.connect().ok());
    EXPECT_EQ(device.state(), usn::hal::DeviceState::Connected);

    auto caps = device.queryCapabilities();
    ASSERT_TRUE(caps.ok());
    EXPECT_EQ(caps->channelCountMax, 16u);

    auto validCfg = usn::test::makeSupportedCaptureConfiguration();
    EXPECT_TRUE(device.configure(validCfg).ok());
    EXPECT_EQ(device.state(), usn::hal::DeviceState::Connected);

    TestSink sink;
    device.setSampleSink(&sink);

    EXPECT_TRUE(device.arm().ok());
    EXPECT_EQ(device.state(), usn::hal::DeviceState::Armed);

    EXPECT_TRUE(device.start().ok());
    EXPECT_EQ(device.state(), usn::hal::DeviceState::Capturing);

    // Deterministic block production
    size_t pumped = device.pump(4);
    EXPECT_EQ(pumped, 4u);
    EXPECT_EQ(sink.m_blocks.size(), 4u);

    EXPECT_TRUE(device.stop().ok());
    EXPECT_EQ(device.state(), usn::hal::DeviceState::Connected);

    device.disconnect();
    EXPECT_EQ(device.state(), usn::hal::DeviceState::Disconnected);
}

TEST(HalTest, FakeCaptureDeviceEnvelopeValidation) {
    auto opts = usn::test::makeFakeDeviceOptions(256);
    usn::hal::FakeCaptureDevice device(opts);

    EXPECT_TRUE(device.connect().ok());

    auto invalidCfg = usn::test::makeSupportedCaptureConfiguration();
    // Out of envelope: 100 MHz is beyond the 20 MHz max
    invalidCfg.sampleRateHz = 100'000'000;

    auto confRes = device.configure(invalidCfg);
    EXPECT_FALSE(confRes.ok());
    EXPECT_EQ(confRes.code(), usn::ErrorCode::SampleRateUnsupported);
}

TEST(HalTest, FakeCaptureDeviceFaultInjection) {
    auto opts = usn::test::makeFakeDeviceOptions(128);
    usn::hal::FakeCaptureDevice device(opts);

    EXPECT_TRUE(device.connect().ok());
    EXPECT_TRUE(device.configure(usn::test::makeSupportedCaptureConfiguration()).ok());

    TestSink sink;
    device.setSampleSink(&sink);

    EXPECT_TRUE(device.arm().ok());
    EXPECT_TRUE(device.start().ok());

    // Inject overflow on next block
    device.injectDeviceOverflow(1);
    device.pump(1);
    ASSERT_EQ(sink.m_blocks.size(), 1u);
    EXPECT_TRUE(hasFlag(sink.m_blocks[0].header().flags, usn::BlockFlag::OverflowBefore));

    // Now inject sequence gap
    device.injectSequenceGap(5);
    device.pump(1);
    ASSERT_EQ(sink.m_blocks.size(), 2u);

    EXPECT_GT(sink.m_blocks[1].header().sequence, sink.m_blocks[0].header().sequence + 1);

    EXPECT_TRUE(device.stop().ok());
}

TEST(HalTest, DeviceManagerManualProbe) {
    usn::hal::DeviceManager mgr;
    auto probe = std::make_unique<usn::hal::ManualProbe>();
    probe->addEndpoint("COM5", "Teensy 4.1 USB Serial");
    mgr.addProbe(std::move(probe));

    mgr.setDeviceFactory([](const usn::transport::DiscoveredDevice& dev) {
        auto opts = usn::test::makeFakeDeviceOptions();
        opts.identity.endpoint = dev.endpoint;
        return std::make_unique<usn::hal::FakeCaptureDevice>(opts);
    });

    EXPECT_TRUE(mgr.rescan().ok());
    EXPECT_EQ(mgr.discovered().size(), 1u);
    EXPECT_EQ(mgr.discovered()[0].endpoint, "COM5");

    auto devOr = mgr.acquire("COM5");
    ASSERT_TRUE(devOr.ok());
    EXPECT_NE(*devOr, nullptr);
    EXPECT_EQ((*devOr)->identity().endpoint, "COM5");

    mgr.release(*devOr);
}
