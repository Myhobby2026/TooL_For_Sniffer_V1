// -----------------------------------------------------------------------------
// wire_format_test.cpp -- binary layout, CRC, and RLE codec validation.
//
// Verifies:
//   - Byte sizes and field offsets match the spec and shared C headers.
//   - CRC32C Castagnoli RFC 3720 test vectors and packet CRC calculations.
//   - RLE compression and decompression round-tripping for 1, 2, and 4 byte strides.
// -----------------------------------------------------------------------------
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "usn_wire.h"
#include "usn_wire_crc.h"
#include "usn_wire_rle.h"

// --- Wire structure sizes and offsets ----------------------------------------

TEST(WireFormatTest, HeaderSizeAndOffsets) {
    EXPECT_EQ(sizeof(struct UsnPacketHeader), 32u);
    EXPECT_EQ(offsetof(struct UsnPacketHeader, magic), 0u);
    EXPECT_EQ(offsetof(struct UsnPacketHeader, headerVersion), 4u);
    EXPECT_EQ(offsetof(struct UsnPacketHeader, packetType), 5u);
    EXPECT_EQ(offsetof(struct UsnPacketHeader, flags), 6u);
    EXPECT_EQ(offsetof(struct UsnPacketHeader, headerLength), 7u);
    EXPECT_EQ(offsetof(struct UsnPacketHeader, sequence), 8u);
    EXPECT_EQ(offsetof(struct UsnPacketHeader, bodyLength), 12u);
    EXPECT_EQ(offsetof(struct UsnPacketHeader, streamId), 16u);
    EXPECT_EQ(offsetof(struct UsnPacketHeader, bodyCrc32c), 24u);
    EXPECT_EQ(offsetof(struct UsnPacketHeader, headerCrc32c), 28u);
}

TEST(WireFormatTest, BodyPrefixSizes) {
    EXPECT_EQ(sizeof(struct UsnSampleBlockPrefix), 40u);
    EXPECT_EQ(sizeof(struct UsnRleBlockPrefix), 48u);
    EXPECT_EQ(sizeof(struct UsnRleRun), 8u);
    EXPECT_EQ(sizeof(struct UsnCommandBody), 4u);
    EXPECT_EQ(sizeof(struct UsnCommandAckBody), 6u);
    EXPECT_EQ(sizeof(struct UsnDeviceHelloBody), 24u);
    EXPECT_EQ(sizeof(struct UsnTriggerEventBody), 28u);
    EXPECT_EQ(sizeof(struct UsnHeartbeatBody), 32u);
}

// --- CRC32C RFC 3720 Appendix B Vectors --------------------------------------

TEST(WireFormatTest, Crc32cRfc3720Vectors) {
    // Vector 1: 32 bytes of 0x00 -> 0x8a9136aa
    std::vector<uint8_t> zeros(32, 0x00);
    EXPECT_EQ(usn_crc32c(zeros.data(), zeros.size()), 0x8A9136AAu);

    // Vector 2: 32 bytes of 0xff -> 0x62a8ab43
    std::vector<uint8_t> ones(32, 0xFF);
    EXPECT_EQ(usn_crc32c(ones.data(), ones.size()), 0x62A8AB43u);

    // Vector 3: 32 bytes ascending 00..1f -> 0x46dd794e
    std::vector<uint8_t> ascending(32);
    for (size_t i = 0; i < 32; ++i) {
        ascending[i] = static_cast<uint8_t>(i);
    }
    EXPECT_EQ(usn_crc32c(ascending.data(), ascending.size()), 0x46DD794Eu);

    // Vector 4: 32 bytes descending 1f..00 -> 0x113fdb5c
    std::vector<uint8_t> descending(32);
    for (size_t i = 0; i < 32; ++i) {
        descending[i] = static_cast<uint8_t>(31 - i);
    }
    EXPECT_EQ(usn_crc32c(descending.data(), descending.size()), 0x113FDB5Cu);

    // Vector 5: "123456789" -> 0xe3069283
    const char digits[] = "123456789";
    EXPECT_EQ(usn_crc32c(digits, 9), 0xE3069283u);
}

TEST(WireFormatTest, Crc32cIncrementalMatchesOneShot) {
    const char data[] = "Universal Sniffer Logic Analyzer Wire Protocol Castagnoli CRC-32C Verification";
    size_t len = strlen(data);

    uint32_t oneShot = usn_crc32c(data, len);

    uint32_t state = USN_CRC32C_INIT;
    state = usn_crc32c_update(state, data, 20);
    state = usn_crc32c_update(state, data + 20, len - 20);
    uint32_t streamed = usn_crc32c_final(state);

    EXPECT_EQ(oneShot, streamed);
    EXPECT_TRUE(usn_crc32c_verify(data, len, oneShot));
    EXPECT_FALSE(usn_crc32c_verify(data, len, oneShot ^ 0x01));
}

// --- RLE Codec Round-Trip Tests ---------------------------------------------

namespace {

void testRleRoundTrip(const std::vector<uint8_t>& rawSamples, uint32_t sampleCount, uint8_t strideBytes) {
    std::vector<struct UsnRleRun> runs(sampleCount);

    uint32_t runsWritten = 0;
    int encRes = usn_rle_encode(rawSamples.data(), sampleCount, strideBytes,
                                runs.data(), static_cast<uint32_t>(runs.size()),
                                &runsWritten);
    ASSERT_EQ(encRes, USN_RLE_OK) << "Encoding failed for stride " << static_cast<int>(strideBytes);
    ASSERT_GT(runsWritten, 0u);

    std::vector<uint8_t> decoded(rawSamples.size(), 0);
    uint32_t samplesDecoded = 0;
    int decRes = usn_rle_decode(runs.data(), runsWritten, strideBytes,
                                decoded.data(), sampleCount,
                                &samplesDecoded);
    ASSERT_EQ(decRes, USN_RLE_OK) << "Decoding failed for stride " << static_cast<int>(strideBytes);
    EXPECT_EQ(samplesDecoded, sampleCount);
    EXPECT_EQ(decoded, rawSamples);
}

}  // namespace

TEST(WireFormatTest, RleSingleRun1Byte) {
    uint32_t count = 500;
    std::vector<uint8_t> raw(count, 0xAA);
    testRleRoundTrip(raw, count, 1);
}

TEST(WireFormatTest, RleSingleRun2Bytes) {
    uint32_t count = 1000;
    std::vector<uint8_t> raw(count * 2);
    for (size_t i = 0; i < count; ++i) {
        raw[2 * i] = 0x34;
        raw[2 * i + 1] = 0x12;
    }
    testRleRoundTrip(raw, count, 2);
}

TEST(WireFormatTest, RleSingleRun4Bytes) {
    uint32_t count = 250;
    std::vector<uint8_t> raw(count * 4);
    for (size_t i = 0; i < count; ++i) {
        raw[4 * i] = 0x78;
        raw[4 * i + 1] = 0x56;
        raw[4 * i + 2] = 0x34;
        raw[4 * i + 3] = 0x12;
    }
    testRleRoundTrip(raw, count, 4);
}

TEST(WireFormatTest, RleAllUniqueSamples) {
    uint32_t count = 128;
    std::vector<uint8_t> raw(count * 2);
    for (uint32_t i = 0; i < count; ++i) {
        raw[2 * i] = static_cast<uint8_t>(i & 0xFF);
        raw[2 * i + 1] = static_cast<uint8_t>((i >> 8) & 0xFF);
    }
    testRleRoundTrip(raw, count, 2);
}

TEST(WireFormatTest, RleAlternatingPattern) {
    uint32_t count = 300;
    std::vector<uint8_t> raw(count);
    for (uint32_t i = 0; i < count; ++i) {
        raw[i] = (i % 2 == 0) ? 0x00 : 0xFF;
    }
    testRleRoundTrip(raw, count, 1);
}

TEST(WireFormatTest, RleErrorCases) {
    std::vector<uint8_t> raw(100, 0x55);
    std::vector<struct UsnRleRun> runs(1);  // capacity 1 is too small for 100 alternating
    uint32_t runsWritten = 0;

    std::vector<uint8_t> alternating(100);
    for (size_t i = 0; i < 100; ++i) alternating[i] = static_cast<uint8_t>(i);
    int res = usn_rle_encode(alternating.data(), 100, 1, runs.data(), 1, &runsWritten);
    EXPECT_EQ(res, USN_RLE_ERR_OUTPUT_FULL);

    // Null pointer checks
    EXPECT_EQ(usn_rle_encode(nullptr, 100, 1, runs.data(), 10, &runsWritten),
              USN_RLE_ERR_ARGUMENT);
    EXPECT_EQ(usn_rle_encode(raw.data(), 0, 1, runs.data(), 10, &runsWritten),
              USN_RLE_ERR_ARGUMENT);
    EXPECT_EQ(usn_rle_encode(raw.data(), 100, 3, runs.data(), 10, &runsWritten),
              USN_RLE_ERR_ARGUMENT);  // invalid stride 3
}
