// -----------------------------------------------------------------------------
// wire_golden_test.cpp -- the C++ encoder must agree, byte for byte, with a packet
// built by an independent implementation of the same specification.
//
// expected.hex in this directory was produced by tests/golden/regenerate.py, which
// implements the field layout and the CRC32C polynomial from shared/wire/usn_wire.h
// in Python and never calls the C++ code. Comparing the two is what makes this a
// correctness test rather than a change detector: if the golden had been generated
// by saving buildPacket()'s own output, the test would pass no matter how wrong the
// encoder was, provided it was wrong the same way twice.
// -----------------------------------------------------------------------------
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "usn/transport/packet_codec.h"
#include "usn_wire.h"

using usn::BlockFlag;
using usn::BlockHeader;
using usn::DeviceTick;
using usn::SampleIndex;
using usn::transport::PacketCodec;

namespace {

#ifndef USN_GOLDEN_DIR
#error "USN_GOLDEN_DIR must be defined by CMake"
#endif

const std::filesystem::path kGoldenDir{USN_GOLDEN_DIR};

// Parses expected.hex: '#' comments, two hex digits per byte, any whitespace.
std::vector<std::byte> readHex(const std::filesystem::path& path) {
    std::ifstream in(path);
    EXPECT_TRUE(in.is_open()) << path.string();
    std::vector<std::byte> bytes;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::string token;
        for (char c : line) {
            if (std::isspace(static_cast<unsigned char>(c)) != 0) {
                if (!token.empty()) {
                    // EXPECT, not ASSERT: ASSERT_* expands to a `return` and this
                    // function returns a vector, which GoogleTest also forbids.
                    EXPECT_EQ(token.size(), 2u) << "byte '" << token << "' in " << path.string();
                    if (token.size() == 2) {
                        bytes.push_back(static_cast<std::byte>(std::stoul(token, nullptr, 16)));
                    }
                    token.clear();
                }
            } else {
                token += c;
            }
        }
        if (!token.empty()) {
            EXPECT_EQ(token.size(), 2u);
            bytes.push_back(static_cast<std::byte>(std::stoul(token, nullptr, 16)));
        }
    }
    return bytes;
}

std::vector<std::byte> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.is_open()) << path.string();
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

std::string toHex(const std::vector<std::byte>& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (auto const byte : bytes) {
        auto const value = static_cast<std::uint8_t>(byte);
        out += kDigits[value >> 4];
        out += kDigits[value & 0x0Fu];
    }
    return out;
}

struct GoldenCase {
    std::filesystem::path directory;
    nlohmann::json config;
    std::vector<std::byte> expected;
    std::vector<std::byte> input;
};

GoldenCase loadCase(const std::string& name) {
    GoldenCase testCase;
    testCase.directory = kGoldenDir / name;
    std::ifstream configStream(testCase.directory / "config.json");
    EXPECT_TRUE(configStream.is_open()) << (testCase.directory / "config.json").string();
    testCase.config = nlohmann::json::parse(configStream);
    testCase.expected = readHex(testCase.directory / "expected.hex");
    testCase.input = readFile(testCase.directory / "input.bin");
    return testCase;
}

TEST(WireGoldenTest, CppEncoderMatchesTheIndependentGolden) {
    auto const testCase = loadCase("wire_sample_block");
    const auto& config = testCase.config;
    const auto& block = config["block"];

    // Build the same packet from the same description the generator used.
    BlockHeader header;
    header.firstSampleIndex = SampleIndex(block["firstSampleIndex"].get<std::uint64_t>());
    header.firstTick = DeviceTick(block["firstDeviceTick"].get<std::uint64_t>());
    header.sequence = config["sequence"].get<std::uint32_t>();
    header.streamId = config["streamId"].get<std::uint64_t>();
    header.sampleCount = block["sampleCount"].get<std::uint32_t>();
    header.channelCount = block["channelCount"].get<std::uint16_t>();
    header.strideBytes = block["strideBytes"].get<std::uint8_t>();
    header.flags = BlockFlag::None;
    header.channelMask = block["channelMask"].get<std::uint64_t>();
    header.sampleRateHz = block["sampleRateHz"].get<std::uint64_t>();

    ASSERT_EQ(block["payloadPattern"].get<std::string>(), "counter16");
    ASSERT_EQ(header.strideBytes, 2);
    std::vector<std::byte> payload(static_cast<std::size_t>(header.sampleCount) * 2);
    for (std::uint32_t i = 0; i < header.sampleCount; ++i) {
        payload[static_cast<std::size_t>(i) * 2] = static_cast<std::byte>(i & 0xFFu);
        payload[static_cast<std::size_t>(i) * 2 + 1] = static_cast<std::byte>((i >> 8) & 0xFFu);
    }

    auto const body = PacketCodec::buildSampleBlockBody(header, payload);
    auto const packet = PacketCodec::buildPacket(
        static_cast<std::uint8_t>(config["packetType"].get<unsigned>()),
        static_cast<std::uint8_t>(config["flags"].get<unsigned>()), header.sequence,
        header.streamId, body);

    // Byte-for-byte, with the differing offset reported rather than a wall of hex.
    ASSERT_EQ(packet.size(), testCase.expected.size())
        << "expected " << testCase.expected.size() << " bytes, built " << packet.size();
    for (std::size_t i = 0; i < packet.size(); ++i) {
        if (packet[i] != testCase.expected[i]) {
            ADD_FAILURE() << "byte " << i << " differs: built 0x" << toHex({packet[i]})
                          << ", golden 0x" << toHex({testCase.expected[i]});
            break;   // one reported difference is enough to act on
        }
    }
    EXPECT_EQ(toHex(packet), toHex(testCase.expected));
}

TEST(WireGoldenTest, GoldenInputFileMatchesTheGoldenHex) {
    // input.bin and expected.hex are two renderings of the same case; if they ever
    // disagree the case directory itself is corrupt.
    auto const testCase = loadCase("wire_sample_block");
    EXPECT_EQ(toHex(testCase.input), toHex(testCase.expected));
}

TEST(WireGoldenTest, CodecDecodesTheGoldenPacketBackToItsFields) {
    auto const testCase = loadCase("wire_sample_block");
    const auto& block = testCase.config["block"];

    PacketCodec codec;
    codec.feed(testCase.input);
    auto packets = codec.takePackets();
    ASSERT_EQ(packets.size(), 1u);
    EXPECT_EQ(codec.stats().crcFailures, 0u);
    EXPECT_EQ(codec.stats().headerCrcFailures, 0u);
    EXPECT_EQ(codec.stats().resyncEvents, 0u);

    auto parsed = PacketCodec::parseSampleBlock(packets[0]);
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
    EXPECT_EQ(parsed->firstSampleIndex.value, block["firstSampleIndex"].get<std::uint64_t>());
    EXPECT_EQ(parsed->sampleCount, block["sampleCount"].get<std::uint32_t>());
    EXPECT_EQ(parsed->channelCount, block["channelCount"].get<std::uint16_t>());
    EXPECT_EQ(parsed->strideBytes, block["strideBytes"].get<std::uint8_t>());
    EXPECT_EQ(parsed->channelMask, block["channelMask"].get<std::uint64_t>());
    EXPECT_EQ(parsed->sampleRateHz, block["sampleRateHz"].get<std::uint64_t>());
    EXPECT_EQ(parsed->payload.size(),
              static_cast<std::size_t>(block["sampleCount"].get<std::uint32_t>()) *
                  block["strideBytes"].get<std::uint8_t>());
}

TEST(WireGoldenTest, OneFlippedBitInTheGoldenPacketIsRejected) {
    // The golden is the reference for "correct"; this is the reference for
    // "detectably incorrect". A CRC that accepts a single-bit change is not a CRC.
    auto const testCase = loadCase("wire_sample_block");
    for (std::size_t bit = 0; bit < testCase.input.size() * 8; bit += 7) {
        auto corrupted = testCase.input;
        auto const byteIndex = bit / 8;
        auto const bitIndex = bit % 8;
        corrupted[byteIndex] =
            corrupted[byteIndex] ^ static_cast<std::byte>(static_cast<std::uint8_t>(1u << bitIndex));

        PacketCodec codec;
        codec.feed(corrupted);
        auto packets = codec.takePackets();
        auto const stats = codec.stats();
        bool const accounted =
            packets.empty() || stats.crcFailures > 0 || stats.headerCrcFailures > 0 ||
            stats.resyncEvents > 0 || stats.lengthRejects > 0 || stats.reservedFlagRejects > 0 ||
            stats.headerVersionRejects > 0;
        EXPECT_TRUE(accounted) << "a single-bit change at bit " << bit << " was accepted silently";
        if (packets.size() == 1 && stats.crcFailures == 0 && stats.headerCrcFailures == 0) {
            // Acceptable only when the flipped bit landed in a field the CRC does not
            // cover and the packet is still structurally valid -- which cannot happen
            // here, since both CRCs between them cover every byte.
            ADD_FAILURE() << "bit " << bit << " changed a byte that no CRC covers";
        }
    }
}

}  // namespace
