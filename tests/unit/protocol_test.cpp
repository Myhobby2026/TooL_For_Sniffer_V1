// -----------------------------------------------------------------------------
// protocol_test.cpp -- tests for usn::protocol (Registry, DecoderAPI, Conformance).
// -----------------------------------------------------------------------------
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "usn/model/sample.h"
#include "usn/protocol/decoder_api.h"
#include "usn/protocol/decoder_registry.h"

namespace {

// TrivialEdgeDecoder: validates the decoder harness and interface contract.
// Watches for rising edges on its bound channel and produces an event per edge.
class TrivialEdgeDecoder final : public usn::protocol::IProtocolDecoder {
public:
    static constexpr std::uint32_t kId = 0x1001;

    TrivialEdgeDecoder() = default;

    usn::protocol::DecoderInfo info() const override {
        usn::protocol::DecoderInfo i;
        i.id = kId;
        i.name = "TrivialEdge";
        i.version = "1.0.0";
        i.apiVersion = usn::protocol::kDecoderApiVersion;
        i.description = "Test decoder detecting edge transitions";
        i.requiredRoles.push_back(usn::ChannelRole::Clock);
        return i;
    }

    usn::Status configure(const usn::protocol::DecoderConfiguration& config) override {
        m_config = config;
        const auto* binding = config.bindingFor(usn::ChannelRole::Clock);
        if (binding == nullptr || !binding->channel.isValid()) {
            return usn::Status::error(usn::ErrorCode::DecoderConfigurationInvalid, "Clock role not bound");
        }
        m_channel = binding->channel;
        reset();
        return usn::Status::success();
    }

    usn::protocol::DecoderConfiguration configuration() const override {
        return m_config;
    }

    void reset() override {
        m_lastLevel = false;
        m_samplesProcessed = 0;
        m_events.clear();
    }

    usn::Status process(const usn::SampleWindow& window) override {
        for (std::size_t i = 0; i < window.size(); ++i) {
            bool currentLevel = window.bitAt(static_cast<std::uint8_t>(m_channel.value), i);
            if (!m_lastLevel && currentLevel) {
                // Rising edge
                usn::DecodedEvent ev;
                ev.begin = window.sampleIndexOf(i);
                ev.end = window.sampleIndexOf(i + 1);
                ev.primaryChannel = m_channel;
                ev.type = usn::EventType::Edge;
                m_events.push_back(std::move(ev));
            }
            m_lastLevel = currentLevel;
            ++m_samplesProcessed;
        }
        return usn::Status::success();
    }

    std::vector<usn::DecodedEvent> takeEvents() override {
        std::vector<usn::DecodedEvent> out;
        out.swap(m_events);
        return out;
    }

    usn::protocol::DecoderCheckpoint saveCheckpoint() const override {
        usn::protocol::DecoderCheckpoint cp(sizeof(bool) + sizeof(std::uint64_t));
        std::memcpy(cp.data(), &m_lastLevel, sizeof(bool));
        std::memcpy(cp.data() + sizeof(bool), &m_samplesProcessed, sizeof(std::uint64_t));
        return cp;
    }

    usn::Status restoreCheckpoint(const usn::protocol::DecoderCheckpoint& cp) override {
        if (cp.size() < sizeof(bool) + sizeof(std::uint64_t)) {
            return usn::Status::error(usn::ErrorCode::DecoderInternalError, "Checkpoint corrupted");
        }
        std::memcpy(&m_lastLevel, cp.data(), sizeof(bool));
        std::memcpy(&m_samplesProcessed, cp.data() + sizeof(bool), sizeof(std::uint64_t));
        m_events.clear();
        return usn::Status::success();
    }

    std::uint64_t samplesProcessed() const noexcept override {
        return m_samplesProcessed;
    }

private:
    usn::protocol::DecoderConfiguration m_config{};
    usn::ChannelId m_channel{0};
    bool m_lastLevel{false};
    std::uint64_t m_samplesProcessed{0};
    std::vector<usn::DecodedEvent> m_events;
};

}  // namespace

TEST(ProtocolTest, RegistryRegisterCreateAndUnregister) {
    usn::protocol::DecoderRegistry reg;
    EXPECT_EQ(reg.size(), 0u);

    auto status = reg.registerDecoder(TrivialEdgeDecoder::kId, "TrivialEdge", []() {
        return std::make_unique<TrivialEdgeDecoder>();
    });
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(reg.size(), 1u);
    EXPECT_TRUE(reg.contains(TrivialEdgeDecoder::kId));

    auto byId = reg.create(TrivialEdgeDecoder::kId);
    ASSERT_TRUE(byId.ok());
    EXPECT_EQ((*byId)->info().name, "TrivialEdge");

    auto byName = reg.createByName("TrivialEdge");
    ASSERT_TRUE(byName.ok());
    EXPECT_EQ((*byName)->info().id, TrivialEdgeDecoder::kId);

    reg.unregister(TrivialEdgeDecoder::kId);
    EXPECT_EQ(reg.size(), 0u);
    EXPECT_FALSE(reg.contains(TrivialEdgeDecoder::kId));
}

TEST(ProtocolTest, DecoderProcessingAndEvents) {
    TrivialEdgeDecoder decoder;
    usn::protocol::DecoderConfiguration config;
    config.decoderId = TrivialEdgeDecoder::kId;
    config.bindings.push_back({usn::ChannelRole::Clock, usn::ChannelId{0}});

    EXPECT_TRUE(decoder.configure(config).ok());

    // Create a 8-sample block: 0, 1, 0, 1, 1, 0, 1, 0 (3 rising edges)
    usn::BlockHeader hdr{};
    hdr.firstSampleIndex = usn::SampleIndex{0};
    hdr.sampleRateHz = 1'000'000;
    hdr.sampleCount = 8;
    hdr.channelCount = 8;
    hdr.strideBytes = 1;

    std::vector<std::byte> payload = {
        std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x01},
        std::byte{0x01}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00}
    };

    usn::OwningSampleBlock block(hdr, std::move(payload));
    usn::SampleWindow window = usn::SampleWindow::fromBlock(block);

    EXPECT_TRUE(decoder.process(window).ok());
    EXPECT_EQ(decoder.samplesProcessed(), 8u);

    auto events = decoder.takeEvents();
    EXPECT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].begin, usn::SampleIndex{1});
    EXPECT_EQ(events[1].begin, usn::SampleIndex{3});
    EXPECT_EQ(events[2].begin, usn::SampleIndex{6});

    // Checkpoint test
    auto checkpoint = decoder.saveCheckpoint();
    decoder.reset();
    EXPECT_EQ(decoder.samplesProcessed(), 0u);

    EXPECT_TRUE(decoder.restoreCheckpoint(checkpoint).ok());
    EXPECT_EQ(decoder.samplesProcessed(), 8u);
}
