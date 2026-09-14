// -----------------------------------------------------------------------------
// wire_codec_test.cpp -- the packet framing contract shared with the firmware.
//
// shared/wire/usn_wire.h is compiled into both the Teensy image and the desktop, so
// these tests are the host side of that contract. The properties that matter:
//
//   * a stream split at ANY byte offset decodes identically (partial headers and
//     partial bodies must be buffered across feed() calls);
//   * corruption is detected and counted, never passed through;
//   * garbage on the link resynchronises instead of desynchronising forever;
//   * the codec's internal buffer is bounded, so a corrupt length field cannot make
//     the host allocate arbitrarily;
//   * sequence and sample-index discontinuities are reported.
// -----------------------------------------------------------------------------
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <span>
#include <random>
#include <vector>

#include "usn/transport/packet_codec.h"
#include "usn/test/test_fixtures.h"
#include "usn_wire.h"

using usn::BlockHeader;
using usn::BlockFlag;
using usn::ErrorCode;
using usn::SampleIndex;
using usn::transport::CodecConfig;
using usn::transport::PacketCodec;
using usn::transport::ParsedPacket;

namespace {

constexpr std::uint32_t kSamples = 32;
constexpr std::uint64_t kStream = 0xABCDu;

std::vector<std::byte> counterPayload(std::uint32_t const samples, std::uint8_t const stride) {
    std::vector<std::byte> payload(static_cast<std::size_t>(samples) * stride);
    for (std::uint32_t i = 0; i < samples; ++i) {
        for (std::uint8_t b = 0; b < stride; ++b) {
            payload[static_cast<std::size_t>(i) * stride + b] =
                static_cast<std::byte>((i >> (8 * b)) & 0xFFu);
        }
    }
    return payload;
}

BlockHeader headerAt(SampleIndex const first, std::uint32_t const sequence) {
    BlockHeader header;
    header.firstSampleIndex = first;
    header.firstTick = usn::DeviceTick(first.value);
    header.sequence = sequence;
    header.streamId = kStream;
    header.sampleCount = kSamples;
    header.channelCount = 8;
    header.strideBytes = 2;
    header.flags = BlockFlag::None;
    header.channelMask = 0xFFu;
    header.sampleRateHz = 1'000'000;
    return header;
}

std::vector<std::byte> sampleBlockPacket(SampleIndex const first, std::uint32_t const sequence) {
    auto const header = headerAt(first, sequence);
    auto payload = counterPayload(kSamples, 2);
    auto body = PacketCodec::buildSampleBlockBody(header, payload);
    return PacketCodec::buildPacket(USN_PKT_SAMPLE_BLOCK, USN_FLAG_CRC_PRESENT, sequence, kStream,
                                    body);
}

std::uint64_t totalDiagnosticCount(const std::vector<usn::DiagnosticEvent>& events,
                                   ErrorCode const code) {
    std::uint64_t total = 0;
    for (const auto& event : events) {
        if (event.code == code) {
            total += event.count;
        }
    }
    return total;
}

TEST(WireLayoutTest, HeaderAndBodySizesMatchTheSharedHeader) {
    // These are the numbers the firmware is compiled against. If they ever change,
    // both sides change together or neither does.
    EXPECT_EQ(sizeof(UsnPacketHeader), static_cast<std::size_t>(USN_WIRE_HEADER_SIZE));
    EXPECT_EQ(USN_WIRE_HEADER_SIZE, 32u);
    EXPECT_EQ(USN_WIRE_MAX_BODY_LENGTH, 65536u);
    EXPECT_EQ(USN_WIRE_MAGIC_VALUE, 0x314E5355u);
}

TEST(PacketCodecTest, RoundTrip) {
    PacketCodec codec;
    auto const packet = sampleBlockPacket(SampleIndex(0), 1);
    codec.feed(packet);
    auto packets = codec.takePackets();
    ASSERT_EQ(packets.size(), 1u);
    EXPECT_EQ(packets[0].header.packetType, USN_PKT_SAMPLE_BLOCK);
    EXPECT_EQ(packets[0].header.sequence, 1u);
    EXPECT_EQ(packets[0].header.streamId, kStream);
    EXPECT_TRUE(packets[0].header.crcPresent());

    auto parsed = PacketCodec::parseSampleBlock(packets[0]);
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
    EXPECT_EQ(parsed->firstSampleIndex.value, 0u);
    EXPECT_EQ(parsed->sampleCount, kSamples);
    EXPECT_EQ(parsed->channelCount, 8u);
    EXPECT_EQ(parsed->strideBytes, 2u);
    EXPECT_EQ(parsed->sampleRateHz, 1'000'000u);
    EXPECT_EQ(parsed->payload.size(), static_cast<std::size_t>(kSamples) * 2);

    EXPECT_EQ(codec.stats().packetsOk, 1u);
    EXPECT_EQ(codec.stats().crcFailures, 0u);
    EXPECT_EQ(codec.stats().resyncEvents, 0u);
    EXPECT_EQ(codec.bufferedBytes(), 0u);
}

TEST(PacketCodecTest, DecodesIdenticallyWhenSplitAtEveryByteOffset) {
    auto const stream = [] {
        std::vector<std::byte> all;
        for (std::uint32_t i = 0; i < 4; ++i) {
            auto packet = sampleBlockPacket(SampleIndex(i * kSamples), i);
            all.insert(all.end(), packet.begin(), packet.end());
        }
        return all;
    }();

    // Feeding one byte at a time is the harshest split: every partial header and
    // every partial body has to be buffered across calls.
    for (std::size_t chunk : {std::size_t{1}, std::size_t{3}, std::size_t{31}, std::size_t{32},
                              std::size_t{33}, std::size_t{100}}) {
        PacketCodec codec;
        for (std::size_t offset = 0; offset < stream.size(); offset += chunk) {
            auto const take = std::min(chunk, stream.size() - offset);
            codec.feed(std::span<const std::byte>(stream.data() + offset, take));
        }
        auto packets = codec.takePackets();
        ASSERT_EQ(packets.size(), 4u) << "chunk=" << chunk;
        for (std::uint32_t i = 0; i < 4; ++i) {
            EXPECT_EQ(packets[i].header.sequence, i) << "chunk=" << chunk;
            auto parsed = PacketCodec::parseSampleBlock(packets[i]);
            ASSERT_TRUE(parsed.ok()) << "chunk=" << chunk;
            EXPECT_EQ(parsed->firstSampleIndex.value, static_cast<std::uint64_t>(i) * kSamples);
        }
        EXPECT_EQ(codec.stats().resyncEvents, 0u) << "chunk=" << chunk;
        EXPECT_EQ(codec.stats().crcFailures, 0u) << "chunk=" << chunk;
    }
}

TEST(PacketCodecTest, CorruptBodyIsDetectedAndNotDelivered) {
    auto packet = sampleBlockPacket(SampleIndex(0), 1);
    // Flip a bit in the body (past the 32-byte header).
    auto const bodyStart = USN_WIRE_HEADER_SIZE + 8;
    packet[bodyStart] = packet[bodyStart] ^ std::byte{0x01};

    PacketCodec codec;
    codec.feed(packet);
    auto packets = codec.takePackets();
    EXPECT_EQ(packets.size(), 0u) << "a packet with a corrupt body was delivered";
    EXPECT_EQ(codec.stats().crcFailures, 1u);
    EXPECT_GT(totalDiagnosticCount(codec.takeDiagnostics(), ErrorCode::CrcMismatch), 0u);
}

TEST(PacketCodecTest, CorruptHeaderIsDetected) {
    auto packet = sampleBlockPacket(SampleIndex(0), 1);
    packet[6] = packet[6] ^ std::byte{0x80};   // inside the header, past magic+version

    PacketCodec codec;
    codec.feed(packet);
    EXPECT_EQ(codec.takePackets().size(), 0u);
    auto const stats = codec.stats();
    EXPECT_TRUE(stats.headerCrcFailures > 0 || stats.resyncEvents > 0)
        << "a corrupt header was accepted silently";
}

TEST(PacketCodecTest, GarbageBeforeAPacketResynchronises) {
    std::mt19937 rng(12345);   // fixed seed: a failing resync test must be reproducible
    std::vector<std::byte> stream(200);
    for (auto& byte : stream) {
        byte = static_cast<std::byte>(rng() & 0xFFu);
    }
    // Make sure the noise does not accidentally contain the magic.
    for (std::size_t i = 0; i + 4 <= stream.size(); ++i) {
        if (stream[i] == std::byte{USN_WIRE_MAGIC_BYTE_0} &&
            stream[i + 1] == std::byte{USN_WIRE_MAGIC_BYTE_1} &&
            stream[i + 2] == std::byte{USN_WIRE_MAGIC_BYTE_2} &&
            stream[i + 3] == std::byte{USN_WIRE_MAGIC_BYTE_3}) {
            stream[i] = std::byte{0x00};
        }
    }
    auto const packet = sampleBlockPacket(SampleIndex(0), 1);
    stream.insert(stream.end(), packet.begin(), packet.end());

    PacketCodec codec;
    codec.feed(stream);
    auto packets = codec.takePackets();
    ASSERT_EQ(packets.size(), 1u) << "the codec did not recover the packet after garbage";
    EXPECT_EQ(packets[0].header.sequence, 1u);
    EXPECT_GE(codec.stats().resyncEvents, 1u);
    EXPECT_GT(codec.stats().bytesSkippedDuringResync, 0u);
    // Discarding 200 bytes is not silent: it is reported.
    EXPECT_GT(totalDiagnosticCount(codec.takeDiagnostics(), ErrorCode::FramingResync), 0u);
}

TEST(PacketCodecTest, SequenceGapsAreReported) {
    PacketCodec codec;
    auto first = sampleBlockPacket(SampleIndex(0), 1);
    auto third = sampleBlockPacket(SampleIndex(2 * kSamples), 3);   // sequence 2 never arrived
    codec.feed(first);
    codec.feed(third);
    auto packets = codec.takePackets();
    ASSERT_EQ(packets.size(), 2u);   // both are individually valid
    EXPECT_EQ(codec.stats().sequenceGaps, 1u);
    EXPECT_GT(totalDiagnosticCount(codec.takeDiagnostics(), ErrorCode::SequenceGap), 0u);
}

TEST(PacketCodecTest, APinnedStartingSequenceDetectsAGapInTheFirstPacket) {
    // haveExpectedSequence lets the host say "the stream must start at N". Without
    // it the first packet seen defines the expectation, so a stream that has already
    // lost packets before the host attached would look clean.
    CodecConfig config;
    config.haveExpectedSequence = true;
    config.initialExpectedSequence = 1;
    PacketCodec codec(config);
    codec.feed(sampleBlockPacket(SampleIndex(0), 4));   // 1, 2 and 3 never arrived
    EXPECT_EQ(codec.takePackets().size(), 1u);          // the packet itself is valid
    EXPECT_EQ(codec.stats().sequenceGaps, 1u);
    EXPECT_EQ(codec.stats().maxGapSize, 3u);
    EXPECT_GT(totalDiagnosticCount(codec.takeDiagnostics(), ErrorCode::SequenceGap), 0u);
}

TEST(PacketCodecTest, ContiguousSequencesProduceNoGap) {
    PacketCodec codec;
    for (std::uint32_t i = 0; i < 20; ++i) {
        codec.feed(sampleBlockPacket(SampleIndex(i * kSamples), i));
    }
    EXPECT_EQ(codec.takePackets().size(), 20u);
    EXPECT_EQ(codec.stats().sequenceGaps, 0u);
    EXPECT_EQ(codec.stats().sampleIndexGaps, 0u);
}

TEST(PacketCodecTest, SampleIndexGapsAreReported) {
    PacketCodec codec;
    codec.feed(sampleBlockPacket(SampleIndex(0), 1));
    // Sequence numbers are contiguous, but the sample indices are not: the device
    // dropped samples without losing a packet.
    codec.feed(sampleBlockPacket(SampleIndex(5000), 2));
    EXPECT_EQ(codec.takePackets().size(), 2u);
    EXPECT_EQ(codec.stats().sampleIndexGaps, 1u);
    EXPECT_GT(totalDiagnosticCount(codec.takeDiagnostics(), ErrorCode::SampleIndexGap), 0u);
}

TEST(PacketCodecTest, BufferIsBoundedByTheDeclaredBodyLimit) {
    // A length field claiming the maximum body must not make the host allocate more
    // than the declared bound, and a truncated stream must leave the rest buffered
    // rather than consumed.
    CodecConfig config;
    config.maxBodyBytes = 1024;
    PacketCodec codec(config);

    std::vector<std::byte> body(1024, std::byte{0x5A});
    auto packet = PacketCodec::buildPacket(USN_PKT_HEARTBEAT, USN_FLAG_CRC_PRESENT, 1, kStream,
                                           body);
    // Feed only the header plus a few body bytes.
    codec.feed(std::span<const std::byte>(packet.data(), USN_WIRE_HEADER_SIZE + 16));
    EXPECT_LE(codec.bufferedBytes(), PacketCodec::maxBufferedBytes(config));
    EXPECT_EQ(codec.takePackets().size(), 0u);

    // A body length above the configured maximum must be rejected outright.
    std::vector<std::byte> oversized(2048, std::byte{0x5A});
    auto big = PacketCodec::buildPacket(USN_PKT_HEARTBEAT, USN_FLAG_CRC_PRESENT, 2, kStream,
                                        oversized);
    PacketCodec strict(config);
    strict.feed(big);
    EXPECT_EQ(strict.takePackets().size(), 0u);
    EXPECT_GE(strict.stats().lengthRejects, 1u);
    EXPECT_LE(strict.bufferedBytes(), PacketCodec::maxBufferedBytes(config));
}

TEST(PacketCodecTest, ReservedFlagBitIsRejected) {
    std::vector<std::byte> body(8, std::byte{0x00});
    auto packet = PacketCodec::buildPacket(USN_PKT_HEARTBEAT,
                                           USN_FLAG_CRC_PRESENT | USN_FLAG_RESERVED, 1, kStream,
                                           body);
    PacketCodec codec;
    codec.feed(packet);
    EXPECT_EQ(codec.takePackets().size(), 0u);
    EXPECT_GE(codec.stats().reservedFlagRejects, 1u);
    EXPECT_GT(totalDiagnosticCount(codec.takeDiagnostics(), ErrorCode::ReservedFlagSet), 0u);
}

TEST(PacketCodecTest, UnknownPacketTypeIsSkippedNotFatal) {
    std::vector<std::byte> body(8, std::byte{0x00});
    auto unknown = PacketCodec::buildPacket(0x7F, USN_FLAG_CRC_PRESENT, 1, kStream, body);
    auto known = sampleBlockPacket(SampleIndex(0), 2);

    PacketCodec codec;
    codec.feed(unknown);
    codec.feed(known);
    auto packets = codec.takePackets();
    // A firmware newer than the host may add packet types. Skipping the unknown one
    // and still decoding the rest is what keeps the two upgradable independently.
    ASSERT_EQ(packets.size(), 1u);
    EXPECT_EQ(packets[0].header.packetType, USN_PKT_SAMPLE_BLOCK);
    EXPECT_GE(codec.stats().unknownTypeSkips, 1u);
}

TEST(PacketCodecTest, RequireCrcRejectsUncrcedPackets) {
    std::vector<std::byte> body(8, std::byte{0x00});
    auto packet = PacketCodec::buildPacket(USN_PKT_HEARTBEAT, /*flags=*/0, 1, kStream, body);

    CodecConfig permissive;
    permissive.requireCrc = false;
    PacketCodec lenient(permissive);
    lenient.feed(packet);
    EXPECT_EQ(lenient.takePackets().size(), 1u);

    CodecConfig strict;
    strict.requireCrc = true;
    PacketCodec guarded(strict);
    guarded.feed(packet);
    EXPECT_EQ(guarded.takePackets().size(), 0u);
}

TEST(PacketCodecTest, MultiplePacketsInOneFeed) {
    std::vector<std::byte> stream;
    for (std::uint32_t i = 0; i < 10; ++i) {
        auto packet = sampleBlockPacket(SampleIndex(i * kSamples), i);
        stream.insert(stream.end(), packet.begin(), packet.end());
    }
    PacketCodec codec;
    codec.feed(stream);
    auto packets = codec.takePackets();
    ASSERT_EQ(packets.size(), 10u);
    EXPECT_EQ(codec.stats().bytesReceived, stream.size());
    EXPECT_EQ(codec.stats().sequenceGaps, 0u);
    EXPECT_EQ(codec.stats().sampleIndexGaps, 0u);
    EXPECT_TRUE(codec.takeDiagnostics().empty());
}

TEST(PacketCodecTest, ResetClearsState) {
    PacketCodec codec;
    auto packet = sampleBlockPacket(SampleIndex(0), 1);
    codec.feed(std::span<const std::byte>(packet.data(), 10));   // partial
    EXPECT_GT(codec.bufferedBytes(), 0u);
    codec.reset();
    EXPECT_EQ(codec.bufferedBytes(), 0u);
    EXPECT_EQ(codec.stats().bytesReceived, 0u);
    // After a reset the next packet is treated as the start of a stream.
    codec.feed(packet);
    EXPECT_EQ(codec.takePackets().size(), 1u);
}

TEST(PacketCodecTest, SampleBlockBodyRejectsTruncation) {
    auto packet = sampleBlockPacket(SampleIndex(0), 1);
    ParsedPacket truncated;
    truncated.header.packetType = USN_PKT_SAMPLE_BLOCK;
    truncated.body.assign(packet.begin() + USN_WIRE_HEADER_SIZE,
                          packet.begin() + USN_WIRE_HEADER_SIZE + 8);   // prefix is 40 bytes
    EXPECT_FALSE(PacketCodec::parseSampleBlock(truncated).ok());
}

}  // namespace
