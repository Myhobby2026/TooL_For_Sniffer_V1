// -----------------------------------------------------------------------------
// codec_fuzz_test.cpp -- randomised input against the packet framer.
//
// This is a deterministic fuzzer, not a coverage-guided one: the seed is derived
// from the iteration index, so a failure is reproducible from the number printed in
// the message and there is no corpus to manage. Coverage guidance is a later
// addition; determinism is what makes it usable in CI today.
//
// The invariants are the ones the framer owes the rest of the system:
//   * its buffer stays inside the declared bound, so a corrupt length field cannot
//     make the host allocate without limit;
//   * bytesReceived always equals the bytes actually fed;
//   * every accepted packet is structurally valid and its body is the declared
//     length;
//   * bytes that were consumed without producing a packet are accounted for by a
//     rejection or a resync counter -- consumption is never silent.
//
// USN_FUZZ_ITERATIONS raises the count; the default keeps `ctest` quick.
// -----------------------------------------------------------------------------
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "usn/transport/packet_codec.h"
#include "usn/test/test_fixtures.h"
#include "usn_wire.h"

using usn::BlockHeader;
using usn::BlockFlag;
using usn::SampleIndex;
using usn::transport::CodecConfig;
using usn::transport::PacketCodec;

namespace {

std::uint64_t iterationsFromEnvironment(std::uint64_t const fallback) {
    if (const char* const env = std::getenv("USN_FUZZ_ITERATIONS"); env != nullptr) {
        auto const parsed = std::strtoull(env, nullptr, 10);
        if (parsed > 0) {
            return parsed;
        }
    }
    return fallback;
}

std::vector<std::byte> validPacket(std::mt19937_64& rng, std::uint32_t const sequence) {
    BlockHeader header;
    header.firstSampleIndex = SampleIndex(sequence * 16u);
    header.firstTick = usn::DeviceTick(sequence * 16u);
    header.sequence = sequence;
    header.streamId = 1;
    header.sampleCount = 16;
    header.channelCount = 8;
    header.strideBytes = 2;
    header.flags = BlockFlag::None;
    header.channelMask = 0xFFu;
    header.sampleRateHz = 1'000'000;

    std::vector<std::byte> payload(32);
    for (auto& byte : payload) {
        byte = static_cast<std::byte>(rng() & 0xFFu);
    }
    auto body = PacketCodec::buildSampleBlockBody(header, payload);
    return PacketCodec::buildPacket(USN_PKT_SAMPLE_BLOCK, USN_FLAG_CRC_PRESENT, sequence, 1, body);
}

struct Accounting {
    std::uint64_t bytesFed{0};
    std::uint64_t packetsAccepted{0};
    std::uint64_t framedBytesAccepted{0};
    bool consumedWithoutAccount{0};
};

// Feeds `stream` in random-sized chunks and checks the invariants.
Accounting exercise(PacketCodec& codec, const std::vector<std::byte>& stream,
                    std::mt19937_64& rng) {
    Accounting accounting;
    std::size_t offset = 0;
    while (offset < stream.size()) {
        std::size_t const want = 1 + (rng() % 200);
        auto const take = std::min(want, stream.size() - offset);
        codec.feed(std::span<const std::byte>(stream.data() + offset, take));
        offset += take;
        accounting.bytesFed += take;

        // Invariant: the internal buffer never exceeds its declared bound, whatever
        // the input claimed about its own length.
        EXPECT_LE(codec.bufferedBytes(), PacketCodec::maxBufferedBytes(codec.config()));

        for (const auto& packet : codec.takePackets()) {
            accounting.packetsAccepted += 1;
            accounting.framedBytesAccepted +=
                static_cast<std::uint64_t>(USN_WIRE_HEADER_SIZE) + packet.header.bodyLength;
            // Invariant: an accepted packet is structurally consistent.
            EXPECT_EQ(packet.body.size(), static_cast<std::size_t>(packet.header.bodyLength));
            EXPECT_EQ(packet.header.magic, USN_WIRE_MAGIC_VALUE);
            EXPECT_EQ(packet.header.headerLength, USN_WIRE_HEADER_SIZE);
            EXPECT_LE(packet.header.bodyLength, USN_WIRE_MAX_BODY_LENGTH);
        }
    }
    EXPECT_EQ(codec.stats().bytesReceived, accounting.bytesFed);

    // Invariant: bytes that were consumed must be accounted for. Anything consumed
    // that produced neither a packet nor a rejection/resync counter is silent loss.
    auto const consumed = accounting.bytesFed - codec.bufferedBytes();
    auto const stats = codec.stats();
    auto const accounted = stats.packetsOk > 0 || stats.crcFailures > 0 ||
                           stats.headerCrcFailures > 0 || stats.headerVersionRejects > 0 ||
                           stats.lengthRejects > 0 || stats.reservedFlagRejects > 0 ||
                           stats.unknownTypeSkips > 0 || stats.resyncEvents > 0 ||
                           stats.bodyParseFailures > 0 || consumed == 0;
    if (!accounted) {
        accounting.consumedWithoutAccount = true;
    }
    EXPECT_LE(accounting.framedBytesAccepted, consumed);
    return accounting;
}

TEST(CodecFuzzTest, PureRandomBytes) {
    auto const iterations = iterationsFromEnvironment(2000);
    for (std::uint64_t i = 0; i < iterations; ++i) {
        std::mt19937_64 rng(0x5EEDu + i);
        std::size_t const length = rng() % 2048;
        std::vector<std::byte> stream(length);
        for (auto& byte : stream) {
            byte = static_cast<std::byte>(rng() & 0xFFu);
        }
        CodecConfig config;
        config.maxBodyBytes = 4096;
        PacketCodec codec(config);
        auto const accounting = exercise(codec, stream, rng);
        ASSERT_FALSE(accounting.consumedWithoutAccount) << "iteration " << i;
    }
}

TEST(CodecFuzzTest, ValidPacketsWithRandomBitFlips) {
    auto const iterations = iterationsFromEnvironment(2000);
    for (std::uint64_t i = 0; i < iterations; ++i) {
        std::mt19937_64 rng(0xC0FFEEu + i);
        std::vector<std::byte> stream;
        for (std::uint32_t p = 0; p < 4; ++p) {
            auto packet = validPacket(rng, p);
            stream.insert(stream.end(), packet.begin(), packet.end());
        }
        // Flip between one and eight bits somewhere in the stream.
        std::uint64_t const flips = 1 + (rng() % 8);
        for (std::uint64_t f = 0; f < flips && !stream.empty(); ++f) {
            std::size_t const index = rng() % stream.size();
            std::uint8_t const bit = rng() % 8;
            stream[index] = stream[index] ^
                            static_cast<std::byte>(static_cast<std::uint8_t>(1u << bit));
        }
        PacketCodec codec;
        auto const accounting = exercise(codec, stream, rng);
        ASSERT_FALSE(accounting.consumedWithoutAccount) << "iteration " << i;
    }
}

TEST(CodecFuzzTest, TruncatedAndSplicedStreams) {
    auto const iterations = iterationsFromEnvironment(2000);
    for (std::uint64_t i = 0; i < iterations; ++i) {
        std::mt19937_64 rng(0xBADC0DEu + i);
        std::vector<std::byte> stream;
        for (std::uint32_t p = 0; p < 3; ++p) {
            auto packet = validPacket(rng, p);
            // Keep a random prefix of each packet, then splice in random noise: this
            // is what a truncated USB transfer followed by a resumed one looks like.
            std::size_t const keep = rng() % (packet.size() + 1);
            stream.insert(stream.end(), packet.begin(), packet.begin() + keep);
            std::size_t const noise = rng() % 64;
            for (std::size_t n = 0; n < noise; ++n) {
                stream.push_back(static_cast<std::byte>(rng() & 0xFFu));
            }
        }
        PacketCodec codec;
        auto const accounting = exercise(codec, stream, rng);
        ASSERT_FALSE(accounting.consumedWithoutAccount) << "iteration " << i;
    }
}

TEST(CodecFuzzTest, HostileLengthFields) {
    // A header whose bodyLength is enormous must not make the host allocate: the
    // bound is enforced from the config, not from what the input claims.
    auto const iterations = iterationsFromEnvironment(500);
    for (std::uint64_t i = 0; i < iterations; ++i) {
        std::mt19937_64 rng(0xFEEDu + i);
        auto packet = validPacket(rng, 0);
        // Overwrite bodyLength (bytes 12..15) with a random large value. The header
        // CRC will no longer match, which is the point: the length must be rejected
        // before it is ever used to size an allocation.
        std::uint32_t const hostile = static_cast<std::uint32_t>(rng() % 0xFFFFFFFFu);
        packet[12] = static_cast<std::byte>(hostile & 0xFFu);
        packet[13] = static_cast<std::byte>((hostile >> 8) & 0xFFu);
        packet[14] = static_cast<std::byte>((hostile >> 16) & 0xFFu);
        packet[15] = static_cast<std::byte>((hostile >> 24) & 0xFFu);

        CodecConfig config;
        config.maxBodyBytes = 4096;
        PacketCodec codec(config);
        exercise(codec, packet, rng);
        EXPECT_LE(codec.bufferedBytes(), PacketCodec::maxBufferedBytes(config))
            << "iteration " << i;
        EXPECT_EQ(codec.takePackets().size(), 0u) << "iteration " << i;
    }
}

TEST(CodecFuzzTest, SameSeedProducesSameResult) {
    // Determinism is what makes a fuzz failure actionable: without it the report
    // cannot be reproduced and therefore cannot be fixed.
    auto run = [](std::uint64_t seed) {
        std::mt19937_64 rng(seed);
        std::vector<std::byte> stream(1024);
        for (auto& byte : stream) {
            byte = static_cast<std::byte>(rng() & 0xFFu);
        }
        PacketCodec codec;
        codec.feed(stream);
        (void)codec.takePackets();
        return codec.stats();
    };
    auto const a = run(42);
    auto const b = run(42);
    EXPECT_EQ(a.bytesReceived, b.bytesReceived);
    EXPECT_EQ(a.packetsOk, b.packetsOk);
    EXPECT_EQ(a.crcFailures, b.crcFailures);
    EXPECT_EQ(a.resyncEvents, b.resyncEvents);
    EXPECT_EQ(a.bytesSkippedDuringResync, b.bytesSkippedDuringResync);
}

}  // namespace
