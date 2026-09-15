// -----------------------------------------------------------------------------
// common_test.cpp -- tests for usn::common (Status, Log, CRC, SPSC, Perf).
// -----------------------------------------------------------------------------
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "usn/common/crc32c.h"
#include "usn/common/log.h"
#include "usn/common/perf.h"
#include "usn/common/spsc_queue.h"
#include "usn/common/status.h"

// --- Status & StatusOr -------------------------------------------------------

TEST(CommonTest, StatusSuccessAndError) {
    usn::Status okStatus = usn::Status::success();
    EXPECT_TRUE(okStatus.ok());
    EXPECT_TRUE(static_cast<bool>(okStatus));
    EXPECT_EQ(okStatus.code(), usn::ErrorCode::Ok);
    EXPECT_EQ(okStatus.severity(), usn::ErrorSeverity::Info);

    usn::Status err = usn::Status::error(usn::ErrorCode::DeviceBusy, "device is active")
                          .withContext("port", "COM3")
                          .withSampleIndex(1024u);

    EXPECT_FALSE(err.ok());
    EXPECT_FALSE(static_cast<bool>(err));
    EXPECT_EQ(err.code(), usn::ErrorCode::DeviceBusy);
    EXPECT_EQ(err.message(), "device is active");
    EXPECT_EQ(err.severity(), usn::ErrorSeverity::Error);

    EXPECT_EQ(err.context().size(), 1u);
    EXPECT_EQ(err.context()[0].key, "port");
    EXPECT_EQ(err.context()[0].value, "COM3");
    EXPECT_TRUE(err.sampleIndex().has_value());
    EXPECT_EQ(err.sampleIndex().value(), 1024u);

    std::string str = err.toString();
    EXPECT_NE(str.find("DeviceBusy"), std::string::npos);
    EXPECT_NE(str.find("device is active"), std::string::npos);
    EXPECT_NE(str.find("port=COM3"), std::string::npos);
}

TEST(CommonTest, StatusOrValueAndError) {
    usn::StatusOr<int> val = 42;
    EXPECT_TRUE(val.ok());
    EXPECT_EQ(*val, 42);
    EXPECT_EQ(val.value(), 42);
    EXPECT_TRUE(val.status().ok());

    usn::StatusOr<int> err = usn::Status::error(usn::ErrorCode::UsbTimeout, "read timed out");
    EXPECT_FALSE(err.ok());
    EXPECT_EQ(err.status().code(), usn::ErrorCode::UsbTimeout);

    // Test non-copyable type move
    struct MoveOnly {
        int x;
        explicit MoveOnly(int v) : x(v) {}
        MoveOnly(const MoveOnly&) = delete;
        MoveOnly& operator=(const MoveOnly&) = delete;
        MoveOnly(MoveOnly&&) noexcept = default;
        MoveOnly& operator=(MoveOnly&&) noexcept = default;
    };

    usn::StatusOr<MoveOnly> mo = MoveOnly(99);
    EXPECT_TRUE(mo.ok());
    MoveOnly extracted = std::move(mo).value();
    EXPECT_EQ(extracted.x, 99);
}

// --- CRC32C facade -----------------------------------------------------------

TEST(CommonTest, Crc32cClass) {
    std::string msg = "123456789";
    std::vector<std::byte> byteVec(msg.size());
    for (std::size_t i = 0; i < msg.size(); ++i) {
        byteVec[i] = static_cast<std::byte>(msg[i]);
    }
    std::span<const std::byte> span(byteVec.data(), byteVec.size());

    EXPECT_EQ(usn::Crc32c::compute(span), 0xE3069283u);
    EXPECT_TRUE(usn::Crc32c::verify(span, 0xE3069283u));
    EXPECT_FALSE(usn::Crc32c::verify(span, 0x12345678u));

    usn::Crc32c streaming;
    streaming.update(span.subspan(0, 4));
    streaming.update(span.subspan(4));
    EXPECT_EQ(streaming.value(), 0xE3069283u);

    streaming.reset();
    EXPECT_EQ(streaming.value(), 0x00000000u);  // final of INIT is 0
}

// --- Logger ------------------------------------------------------------------

class TestSink final : public usn::log::ISink {
public:
    void write(const usn::log::Record& rec) override {
        m_records.push_back(rec);
    }
    std::vector<usn::log::Record> m_records;
};

TEST(CommonTest, LoggerLevelAndFiltering) {
    auto sink = std::make_shared<TestSink>();
    auto& logger = usn::log::Logger::instance();
    logger.addSink(sink);

    logger.setLevel(usn::log::cats::kCapture, usn::log::Level::Warning);

    USN_LOG_DEBUG(usn::log::cats::kCapture, "Debug message should be filtered");
    USN_LOG_WARN(usn::log::cats::kCapture, "Warning message: count={}", 42);
    USN_LOG_ERROR(usn::log::cats::kCapture, "Error occurred");

    EXPECT_GE(sink->m_records.size(), 2u);
    bool foundWarning = false;
    bool foundError = false;
    for (const auto& r : sink->m_records) {
        if (r.level == usn::log::Level::Warning && r.message.find("42") != std::string::npos) {
            foundWarning = true;
        }
        if (r.level == usn::log::Level::Error) {
            foundError = true;
        }
    }
    EXPECT_TRUE(foundWarning);
    EXPECT_TRUE(foundError);
}

// --- SpscQueue ---------------------------------------------------------------

TEST(CommonTest, SpscQueueBasic) {
    usn::SpscQueue<int> queue(4);
    EXPECT_TRUE(queue.emptyApprox());
    EXPECT_GE(queue.usableCapacity(), 4u);

    EXPECT_TRUE(queue.tryPush(1));
    EXPECT_TRUE(queue.tryPush(2));
    EXPECT_FALSE(queue.emptyApprox());

    auto pop1 = queue.tryPop();
    ASSERT_TRUE(pop1.has_value());
    EXPECT_EQ(*pop1, 1);

    auto pop2 = queue.tryPop();
    ASSERT_TRUE(pop2.has_value());
    EXPECT_EQ(*pop2, 2);

    auto popEmpty = queue.tryPop();
    EXPECT_FALSE(popEmpty.has_value());
}

TEST(CommonTest, SpscQueueMultithreadedContention) {
    constexpr int kTotalItems = 10000;
    usn::SpscQueue<int> queue(64);

    std::thread producer([&]() {
        for (int i = 0; i < kTotalItems; ++i) {
            while (!queue.tryPush(i)) {
                std::this_thread::yield();
            }
        }
    });

    std::vector<int> received;
    received.reserve(kTotalItems);

    std::thread consumer([&]() {
        while (static_cast<int>(received.size()) < kTotalItems) {
            auto val = queue.tryPop();
            if (val.has_value()) {
                received.push_back(*val);
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(received.size(), static_cast<size_t>(kTotalItems));
    for (int i = 0; i < kTotalItems; ++i) {
        EXPECT_EQ(received[static_cast<size_t>(i)], i);
    }
}

// --- PerfRegistry & Probe ----------------------------------------------------

TEST(CommonTest, PerfRegistryAndProbe) {
    auto& perf = usn::perf::PerfRegistry::instance();
    perf.reset();
    perf.setEnabled(true);

    {
        usn::perf::Probe probe(usn::perf::probes::kCodecParse, 1024);
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }

    auto snap = perf.snapshot();
    ASSERT_FALSE(snap.empty());
    bool found = false;
    for (const auto& s : snap) {
        if (s.name == usn::perf::probes::kCodecParse) {
            found = true;
            EXPECT_EQ(s.count, 1u);
            EXPECT_EQ(s.bytesTotal, 1024u);
            EXPECT_GT(s.totalNs, 0u);
            EXPECT_GT(s.meanNs(), 0.0);
        }
    }
    EXPECT_TRUE(found);

    std::string json = perf.toJson();
    EXPECT_NE(json.find("codec.parse"), std::string::npos);

    perf.setEnabled(false);
}
