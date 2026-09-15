// -----------------------------------------------------------------------------
// transport_test.cpp -- tests for usn::transport (Loopback, File, Manager).
// -----------------------------------------------------------------------------
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "usn/transport/file_transport.h"
#include "usn/transport/itransport.h"
#include "usn/transport/loopback_transport.h"

// --- LoopbackTransport Tests -------------------------------------------------

TEST(TransportTest, LoopbackOpenCloseWriteRead) {
    usn::transport::LoopbackTransport transport;
    EXPECT_FALSE(transport.isOpen());

    usn::transport::TransportConfig config{};
    config.kind = usn::transport::TransportKind::Loopback;
    EXPECT_TRUE(transport.open(config).ok());
    EXPECT_TRUE(transport.isOpen());

    // Write bytes to host input (device output)
    std::vector<std::byte> data = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
    transport.pushToDeviceOutput(data);
    EXPECT_EQ(transport.pendingBytes(), 3u);

    // Read bytes
    std::vector<std::byte> readBuf(10);
    auto readRes = transport.read(readBuf, std::chrono::milliseconds(100));
    ASSERT_TRUE(readRes.ok());
    EXPECT_EQ(*readRes, 3u);
    EXPECT_EQ(readBuf[0], std::byte{0x01});
    EXPECT_EQ(readBuf[1], std::byte{0x02});
    EXPECT_EQ(readBuf[2], std::byte{0x03});

    // Write from host to device
    std::vector<std::byte> hostData = {std::byte{0xAA}, std::byte{0xBB}};
    EXPECT_TRUE(transport.write(hostData).ok());
    auto written = transport.takeWritten();
    EXPECT_EQ(written.size(), 2u);
    EXPECT_EQ(written[0], std::byte{0xAA});
    EXPECT_EQ(written[1], std::byte{0xBB});

    transport.close();
    EXPECT_FALSE(transport.isOpen());
}

TEST(TransportTest, LoopbackFaultInjection) {
    usn::transport::LoopbackTransport transport;
    usn::transport::TransportConfig config{};
    EXPECT_TRUE(transport.open(config).ok());

    // Corrupt next bytes
    transport.corruptNextBytes(1, 0xFF);
    std::vector<std::byte> data = {std::byte{0x00}, std::byte{0x12}};
    transport.pushToDeviceOutput(data);

    std::vector<std::byte> readBuf(2);
    auto res = transport.read(readBuf, std::chrono::milliseconds(50));
    ASSERT_TRUE(res.ok());
    EXPECT_EQ(*res, 2u);
    EXPECT_EQ(readBuf[0], std::byte{0xFF});  // flipped
    EXPECT_EQ(readBuf[1], std::byte{0x12});  // unaffected

    // Write blocking test
    EXPECT_FALSE(transport.writeWouldBlock());
    transport.setWriteBlocks(true);
    EXPECT_TRUE(transport.writeWouldBlock());
}

// --- FileTransport Tests -----------------------------------------------------

TEST(TransportTest, FileTransportReadAndWriteRejection) {
    auto tempFile = std::filesystem::temp_directory_path() / "usn_transport_test.bin";
    {
        std::ofstream out(tempFile, std::ios::binary);
        const char content[] = "USN_TEST_STREAM_DATA";
        out.write(content, sizeof(content) - 1);
    }

    usn::transport::FileTransport ft;
    EXPECT_FALSE(ft.isOpen());

    usn::transport::TransportConfig config{};
    config.kind = usn::transport::TransportKind::File;
    config.endpoint = tempFile.string();

    EXPECT_TRUE(ft.open(config).ok());
    EXPECT_TRUE(ft.isOpen());
    EXPECT_GT(ft.fileSize(), 0u);

    std::vector<std::byte> buffer(ft.fileSize());
    auto readRes = ft.read(buffer, std::chrono::milliseconds(50));
    ASSERT_TRUE(readRes.ok());
    EXPECT_EQ(*readRes, buffer.size());

    // FileTransport is read-only; writing must be rejected
    std::vector<std::byte> writeAttempt = {std::byte{0x01}};
    auto writeRes = ft.write(writeAttempt);
    EXPECT_FALSE(writeRes.ok());

    ft.close();
    EXPECT_FALSE(ft.isOpen());

    std::filesystem::remove(tempFile);
}

// --- TransportManager Tests --------------------------------------------------

TEST(TransportTest, TransportManagerCreationAndReconnectPolicy) {
    usn::transport::TransportManager mgr;

    mgr.registerFactory(usn::transport::TransportKind::Loopback,
                        [](const usn::transport::TransportConfig&) {
                            return std::make_unique<usn::transport::LoopbackTransport>();
                        });

    usn::transport::TransportConfig config{};
    config.kind = usn::transport::TransportKind::Loopback;
    auto transportOr = mgr.create(config);
    EXPECT_TRUE(transportOr.ok());
    EXPECT_NE(*transportOr, nullptr);

    // Reconnect policy calculation
    usn::transport::ReconnectPolicy policy{};
    policy.initialBackoff = std::chrono::milliseconds(100);
    policy.backoffMultiplier = 2.0;
    policy.maxBackoff = std::chrono::milliseconds(1000);
    policy.jitter = false;

    mgr.setReconnectPolicy(policy);
    EXPECT_EQ(mgr.backoffForAttempt(0), std::chrono::milliseconds(100));
    EXPECT_EQ(mgr.backoffForAttempt(1), std::chrono::milliseconds(200));
    EXPECT_EQ(mgr.backoffForAttempt(2), std::chrono::milliseconds(400));
    EXPECT_EQ(mgr.backoffForAttempt(4), std::chrono::milliseconds(1000));  // clamped to max
}
