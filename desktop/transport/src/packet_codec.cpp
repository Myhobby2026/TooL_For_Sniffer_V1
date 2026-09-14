// -----------------------------------------------------------------------------
// packet_codec.cpp -- see packet_codec.h.
// -----------------------------------------------------------------------------
#include "usn/transport/packet_codec.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

#include "usn/common/crc32c.h"
#include "usn/common/log.h"
#include "usn/model/diagnostics.h"
#include "usn_wire_crc.h"
#include "usn_wire_rle.h"

namespace usn::transport {
namespace {

constexpr std::byte kMagic0{USN_WIRE_MAGIC_BYTE_0};
constexpr std::byte kMagic1{USN_WIRE_MAGIC_BYTE_1};
constexpr std::byte kMagic2{USN_WIRE_MAGIC_BYTE_2};
constexpr std::byte kMagic3{USN_WIRE_MAGIC_BYTE_3};

std::uint8_t rdU8(const std::byte* p) noexcept { return std::to_integer<std::uint8_t>(*p); }

std::uint16_t rdU16(const std::byte* p) noexcept {
    return static_cast<std::uint16_t>(rdU8(p)) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(rdU8(p + 1)) << 8);
}

// Each shifted term is already uint32_t (unsigned int) after promotion, so the
// outer casts the first version had were redundant -- GCC's -Wuseless-cast said
// so, and it was right.
std::uint32_t rdU32(const std::byte* p) noexcept {
    return static_cast<std::uint32_t>(rdU8(p)) |
           (static_cast<std::uint32_t>(rdU8(p + 1)) << 8) |
           (static_cast<std::uint32_t>(rdU8(p + 2)) << 16) |
           (static_cast<std::uint32_t>(rdU8(p + 3)) << 24);
}

std::uint64_t rdU64(const std::byte* p) noexcept {
    return static_cast<std::uint64_t>(rdU32(p)) |
           (static_cast<std::uint64_t>(rdU32(p + 4)) << 32);
}

void wrU16(std::vector<std::byte>& out, std::uint16_t v) {
    out.push_back(static_cast<std::byte>(v & 0xFFU));
    out.push_back(static_cast<std::byte>((v >> 8) & 0xFFU));
}

void wrU32(std::vector<std::byte>& out, std::uint32_t v) {
    out.push_back(static_cast<std::byte>(v & 0xFFU));
    out.push_back(static_cast<std::byte>((v >> 8) & 0xFFU));
    out.push_back(static_cast<std::byte>((v >> 16) & 0xFFU));
    out.push_back(static_cast<std::byte>((v >> 24) & 0xFFU));
}

void wrU64(std::vector<std::byte>& out, std::uint64_t v) {
    wrU32(out, static_cast<std::uint32_t>(v & 0xFFFFFFFFULL));
    wrU32(out, static_cast<std::uint32_t>(v >> 32));
}

bool magicAt(const std::vector<std::byte>& buf, std::size_t pos) noexcept {
    return pos + 4 <= buf.size() && buf[pos] == kMagic0 && buf[pos + 1] == kMagic1 &&
           buf[pos + 2] == kMagic2 && buf[pos + 3] == kMagic3;
}

bool isKnownPacketType(std::uint8_t type) noexcept {
    return type >= USN_PKT_DEVICE_HELLO && type <= USN_PKT_STREAM_END;
}

// Distance from `expected` to `got` in a wrapping uint32 sequence space.
std::uint32_t sequenceDistance(std::uint32_t expected, std::uint32_t got) noexcept {
    return got - expected;  // unsigned wraparound is exactly what we want here
}

}  // namespace

// --- PacketCodec -------------------------------------------------------------

PacketCodec::PacketCodec(CodecConfig config) : m_config(config) {
    m_expectedSequence = config.initialExpectedSequence;
    m_buffer.reserve(USN_WIRE_HEADER_SIZE + std::min<std::size_t>(config.maxBodyBytes, 1U << 16));
}

void PacketCodec::reset() noexcept {
    m_state = State::HuntMagic;
    m_buffer.clear();
    m_packets.clear();
    m_diagnostics.clear();
    m_stats = FramingStats{};
    m_expectedSequence = m_config.initialExpectedSequence;
    m_sequenceSeeded = false;
    m_nextSampleIndex.reset();
    m_currentStreamId.reset();
    m_garbageReported = false;
}

std::size_t PacketCodec::bufferedBytes() const noexcept { return m_buffer.size(); }

void PacketCodec::feed(std::span<const std::byte> bytes) {
    m_stats.bytesReceived += bytes.size();
    m_buffer.insert(m_buffer.end(), bytes.begin(), bytes.end());
    processBuffered();
}

std::vector<ParsedPacket> PacketCodec::takePackets() {
    std::vector<ParsedPacket> out;
    out.swap(m_packets);
    return out;
}

std::vector<usn::DiagnosticEvent> PacketCodec::takeDiagnostics() {
    std::vector<usn::DiagnosticEvent> out;
    out.swap(m_diagnostics);
    return out;
}

void PacketCodec::emitDiagnostic(ErrorCode const code, std::string_view const message) {
    auto event = diagnosticFromStatus(Status::error(code, std::string(message)));
    if (m_currentStreamId.has_value()) {
        event.streamId = *m_currentStreamId;
    }
    m_diagnostics.push_back(std::move(event));
}

void PacketCodec::resync(std::size_t const advanceBy, ErrorCode const reason,
                         std::string message) {
    // Advance past the false start and re-hunt for the magic. Counting the episode
    // once (rather than per byte) keeps the counter meaningful.
    std::size_t const skip = std::max<std::size_t>(advanceBy, 1);
    std::size_t const actual = std::min(skip, m_buffer.size());
    m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(actual));
    m_stats.bytesSkippedDuringResync += actual;
    m_stats.resyncEvents += 1;
    m_state = State::HuntMagic;
    emitDiagnostic(reason, std::move(message));
    USN_LOG_DEBUG(log::cats::kUsb, "codec resync: {}", message);
}

bool PacketCodec::tryValidateHeader() {
    // Caller guarantees m_buffer.size() >= USN_WIRE_HEADER_SIZE and magic matches.
    const std::byte* p = m_buffer.data();
    PacketHeader h;
    h.magic = rdU32(p);
    h.headerVersion = rdU8(p + 4);
    h.packetType = rdU8(p + 5);
    h.flags = rdU8(p + 6);
    h.headerLength = rdU8(p + 7);
    h.sequence = rdU32(p + 8);
    h.bodyLength = rdU32(p + 12);
    h.streamId = rdU64(p + 16);
    h.bodyCrc32c = rdU32(p + 24);
    h.headerCrc32c = rdU32(p + 28);

    if (h.headerVersion < USN_WIRE_MIN_SUPPORTED_VERSION) {
        m_stats.headerVersionRejects += 1;
        resync(1, ErrorCode::HeaderVersionUnsupported,
               fmt::format("header version {} is below the minimum supported {}", h.headerVersion,
                           USN_WIRE_MIN_SUPPORTED_VERSION));
        return false;
    }
    // A reserved flag bit set means we are out of sync or the firmware is newer
    // than this host. Either way: reject, do not guess.
    if ((h.flags & USN_FLAG_RESERVED) != 0) {
        m_stats.reservedFlagRejects += 1;
        resync(1, ErrorCode::ReservedFlagSet,
               fmt::format("reserved flag bit set in flags 0x{:02x}", h.flags));
        return false;
    }
    if (h.headerLength < USN_WIRE_HEADER_SIZE) {
        m_stats.lengthRejects += 1;
        resync(1, ErrorCode::InvalidPacketLength,
               fmt::format("headerLength {} is below the fixed size {}", h.headerLength,
                           USN_WIRE_HEADER_SIZE));
        return false;
    }
    // The hard cap. Without it a single corrupt length byte could make the host
    // allocate unboundedly or stall forever waiting for bytes that never come.
    if (h.bodyLength > m_config.maxBodyBytes) {
        m_stats.lengthRejects += 1;
        resync(1, ErrorCode::InvalidPacketLength,
               fmt::format("bodyLength {} exceeds the configured maximum {}", h.bodyLength,
                           m_config.maxBodyBytes));
        return false;
    }
    // Header CRC covers bytes [0..27] so a corrupt header is caught BEFORE
    // bodyLength is trusted.
    std::uint32_t const expectedHeaderCrc = usn_crc32c(p, 28);
    if (expectedHeaderCrc != h.headerCrc32c) {
        m_stats.headerCrcFailures += 1;
        resync(1, ErrorCode::CrcMismatch,
               fmt::format("header CRC mismatch: computed 0x{:08x}, packet says 0x{:08x}",
                           expectedHeaderCrc, h.headerCrc32c));
        return false;
    }
    if (h.packetType == USN_PKT_INVALID) {
        m_stats.lengthRejects += 1;
        resync(1, ErrorCode::InvalidPacketType, "packet type INVALID (0x00) is not a valid packet");
        return false;
    }
    return true;
}

void PacketCodec::processBuffered() {
    // Bounded work per feed() so a huge input cannot monopolise the RX thread.
    for (;;) {
        if (m_state == State::HuntMagic) {
            std::size_t pos = 0;
            bool found = false;
            // Leave the last 3 bytes in place: they may be the start of a magic
            // split across two feed() calls.
            std::size_t const limit = m_buffer.size() >= 3 ? m_buffer.size() - 3 : 0;
            while (pos + 4 <= m_buffer.size()) {
                if (magicAt(m_buffer, pos)) {
                    found = true;
                    break;
                }
                ++pos;
            }
            if (!found) {
                if (limit > 0) {
                    m_stats.bytesSkippedDuringResync += limit;
                    m_buffer.erase(m_buffer.begin(),
                                   m_buffer.begin() + static_cast<std::ptrdiff_t>(limit));
                    if (!m_garbageReported) {
                        m_garbageReported = true;
                        m_stats.resyncEvents += 1;
                        emitDiagnostic(
                            ErrorCode::FramingResync,
                            "discarding bytes while hunting for the packet magic; the link is "
                            "carrying data that is not part of a packet");
                    }
                }
                return;  // need more bytes
            }
            if (pos > 0) {
                // Garbage before a valid magic. Reported, never silent.
                m_stats.bytesSkippedDuringResync += pos;
                m_stats.resyncEvents += 1;
                m_garbageReported = false;
                m_buffer.erase(m_buffer.begin(),
                               m_buffer.begin() + static_cast<std::ptrdiff_t>(pos));
                emitDiagnostic(ErrorCode::FramingResync,
                               fmt::format("skipped {} bytes of garbage before packet magic", pos));
            }
            m_state = State::ReadHeader;
        }

        if (m_state == State::ReadHeader) {
            if (m_buffer.size() < USN_WIRE_HEADER_SIZE) {
                return;  // partial header; wait for more bytes
            }
            if (!magicAt(m_buffer, 0)) {
                // Cannot normally happen (we only enter here on a magic match), but
                // stay total rather than assume.
                resync(1, ErrorCode::InvalidMagic, "magic lost while reading header");
                continue;
            }
            if (!tryValidateHeader()) {
                continue;  // resync() already advanced
            }
            PacketHeader h;
            h.magic = rdU32(m_buffer.data());
            h.headerVersion = rdU8(m_buffer.data() + 4);
            h.packetType = rdU8(m_buffer.data() + 5);
            h.flags = rdU8(m_buffer.data() + 6);
            h.headerLength = rdU8(m_buffer.data() + 7);
            h.sequence = rdU32(m_buffer.data() + 8);
            h.bodyLength = rdU32(m_buffer.data() + 12);
            h.streamId = rdU64(m_buffer.data() + 16);
            h.bodyCrc32c = rdU32(m_buffer.data() + 24);
            h.headerCrc32c = rdU32(m_buffer.data() + 28);

            std::size_t const total =
                static_cast<std::size_t>(h.headerLength) + h.bodyLength;
            if (m_buffer.size() < total) {
                return;  // partial body; wait for more bytes
            }
            const std::byte* bodyPtr = m_buffer.data() + h.headerLength;
            std::span<const std::byte> body(bodyPtr, h.bodyLength);

            if (h.crcPresent()) {
                std::uint32_t const computed = usn_crc32c(body.data(), body.size());
                if (computed != h.bodyCrc32c) {
                    m_stats.crcFailures += 1;
                    m_buffer.erase(m_buffer.begin(),
                                   m_buffer.begin() + static_cast<std::ptrdiff_t>(total));
                    emitDiagnostic(ErrorCode::CrcMismatch,
                                   fmt::format("body CRC mismatch on sequence {}: computed "
                                               "0x{:08x}, packet says 0x{:08x}",
                                               h.sequence, computed, h.bodyCrc32c));
                    m_state = State::HuntMagic;
                    continue;
                }
            } else if (m_config.requireCrc) {
                m_buffer.erase(m_buffer.begin(),
                               m_buffer.begin() + static_cast<std::ptrdiff_t>(total));
                emitDiagnostic(ErrorCode::CrcMismatch,
                               fmt::format("packet sequence {} has no CRC but the codec "
                                           "requires one",
                                           h.sequence));
                m_state = State::HuntMagic;
                continue;
            }

            // --- continuity checks (master spec section 14) --------------------
            if (m_config.trackContinuity) {
                if (!m_currentStreamId.has_value()) {
                    m_currentStreamId = h.streamId;
                } else if (*m_currentStreamId != h.streamId) {
                    m_stats.streamIdChanges += 1;
                    emitDiagnostic(ErrorCode::StreamIdMismatch,
                                   fmt::format("streamId changed from {} to {} -- the device "
                                               "restarted or re-armed mid-capture",
                                               *m_currentStreamId, h.streamId));
                    m_currentStreamId = h.streamId;
                    // A new stream restarts sample-index continuity tracking.
                    m_nextSampleIndex.reset();
                }
                // Seed once. With haveExpectedSequence set, the caller has pinned the
                // sequence the stream must start at; otherwise the first packet seen
                // defines it.
                if (!m_sequenceSeeded) {
                    m_expectedSequence = m_config.haveExpectedSequence
                                             ? m_config.initialExpectedSequence
                                             : h.sequence;
                    m_sequenceSeeded = true;
                }
                if (h.sequence != m_expectedSequence) {
                    std::uint32_t const gap = sequenceDistance(m_expectedSequence, h.sequence);
                    m_stats.sequenceGaps += 1;
                    m_stats.maxGapSize = std::max(m_stats.maxGapSize, gap);
                    emitDiagnostic(
                        ErrorCode::SequenceGap,
                        fmt::format("sequence gap: expected {}, got {} ({} packet(s) missing)",
                                    m_expectedSequence, h.sequence, gap));
                }
                m_expectedSequence = h.sequence + 1;
            }

            // --- unknown types: skip and report, never abort -------------------
            if (!isKnownPacketType(h.packetType)) {
                m_stats.unknownTypeSkips += 1;
                m_buffer.erase(m_buffer.begin(),
                               m_buffer.begin() + static_cast<std::ptrdiff_t>(total));
                emitDiagnostic(ErrorCode::InvalidPacketType,
                               fmt::format("unknown packet type 0x{:02x} skipped ({} body bytes); "
                                           "the firmware may be newer than this host",
                                           h.packetType, h.bodyLength));
                m_state = State::HuntMagic;
                continue;
            }

            ParsedPacket packet;
            packet.header = h;
            packet.body.assign(body.begin(), body.end());

            // Sample-index continuity is verified independently of packet sequence,
            // so a lost packet and a device-side skip are distinguishable.
            if (h.packetType == USN_PKT_SAMPLE_BLOCK ||
                h.packetType == USN_PKT_SAMPLE_BLOCK_RLE) {
                if (h.packetType == USN_PKT_SAMPLE_BLOCK) {
                    auto parsed = parseSampleBlock(packet);
                    if (!parsed.ok()) {
                        m_stats.bodyParseFailures += 1;
                        emitDiagnostic(parsed.status().code(), parsed.status().message());
                    } else if (m_config.trackContinuity) {
                        checkSampleContinuity(parsed->firstSampleIndex, parsed->sampleCount);
                    }
                } else {
                    auto parsed = parseRleBlock(packet);
                    if (!parsed.ok()) {
                        m_stats.bodyParseFailures += 1;
                        emitDiagnostic(parsed.status().code(), parsed.status().message());
                    } else if (m_config.trackContinuity) {
                        checkSampleContinuity(parsed->block.firstSampleIndex,
                                              parsed->decodedSampleCount);
                    }
                }
            }

            m_stats.packetsOk += 1;
            m_garbageReported = false;
            m_packets.push_back(std::move(packet));

            m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(total));
            m_state = m_buffer.empty() ? State::HuntMagic : State::ReadHeader;
        }
    }
}

void PacketCodec::checkSampleContinuity(SampleIndex const first, std::uint32_t const count) {
    if (m_nextSampleIndex.has_value() && first.value != m_nextSampleIndex->value) {
        m_stats.sampleIndexGaps += 1;
        std::uint64_t const missing = first.value > m_nextSampleIndex->value
                                          ? first.value - m_nextSampleIndex->value
                                          : m_nextSampleIndex->value - first.value;
        emitDiagnostic(
            ErrorCode::SampleIndexGap,
            fmt::format("sample index discontinuity: expected {} but block starts at {} "
                        "({} sample(s) {})",
                        m_nextSampleIndex->value, first.value, missing,
                        first.value > m_nextSampleIndex->value ? "missing" : "rewound"));
    }
    m_nextSampleIndex = SampleIndex(first.value + count);
}

// --- body parsing ------------------------------------------------------------

StatusOr<SampleBlockPayload> PacketCodec::parseSampleBlock(const ParsedPacket& packet) {
    if (packet.body.size() < USN_WIRE_SIZE_PREFIX) {
        return Status::error(
            ErrorCode::InvalidPacketLength,
            fmt::format("sample block body is {} bytes, needs at least {}", packet.body.size(),
                        USN_WIRE_SIZE_PREFIX));
    }
    const std::byte* p = packet.body.data();
    SampleBlockPayload out;
    out.firstSampleIndex = SampleIndex(rdU64(p));
    out.firstTick = DeviceTick(rdU64(p + 8));
    out.sampleCount = rdU32(p + 16);
    out.channelCount = rdU16(p + 20);
    out.strideBytes = rdU8(p + 22);
    std::uint8_t const reserved = rdU8(p + 23);
    out.channelMask = rdU64(p + 24);
    out.sampleRateHz = rdU64(p + 32);

    if (reserved != 0) {
        return Status::error(ErrorCode::InvalidPacketLength,
                             fmt::format("sample block reserved byte is 0x{:02x}, expected 0",
                                         reserved));
    }
    if (!isValidStride(out.strideBytes)) {
        return Status::error(ErrorCode::StrideUnsupported,
                             fmt::format("sample block stride {} is not 1, 2 or 4",
                                         out.strideBytes));
    }
    if (out.sampleCount == 0) {
        return Status::error(ErrorCode::InvalidPacketLength, "sample block sampleCount is zero");
    }
    std::uint64_t const expected =
        static_cast<std::uint64_t>(out.sampleCount) * out.strideBytes;
    if (packet.body.size() != USN_WIRE_SIZE_PREFIX + expected) {
        return Status::error(
                   ErrorCode::InvalidPacketLength,
                   fmt::format("sample block body size {} does not match prefix + sampleCount * "
                               "stride = {}",
                               packet.body.size(), USN_WIRE_SIZE_PREFIX + expected))
            .withSampleIndex(out.firstSampleIndex.value);
    }
    auto const minStride = strideForChannelCount(out.channelCount);
    if (minStride == SampleStride::Invalid) {
        return Status::error(ErrorCode::ChannelCountUnsupported,
                             fmt::format("channel count {} exceeds wire version 1's 32 channels",
                                         out.channelCount));
    }
    if (out.strideBytes < static_cast<std::uint8_t>(minStride)) {
        return Status::error(ErrorCode::StrideUnsupported,
                             fmt::format("stride {} cannot hold {} channels", out.strideBytes,
                                         out.channelCount));
    }
    out.payload = std::span<const std::byte>(packet.body.data() + USN_WIRE_SIZE_PREFIX,
                                             static_cast<std::size_t>(expected));
    return out;
}

StatusOr<RleBlockPayload> PacketCodec::parseRleBlock(const ParsedPacket& packet) {
    if (packet.body.size() < USN_WIRE_RLE_PREFIX_SIZE) {
        return Status::error(ErrorCode::InvalidPacketLength,
                             fmt::format("RLE block body is {} bytes, needs at least {}",
                                         packet.body.size(), USN_WIRE_RLE_PREFIX_SIZE));
    }
    RleBlockPayload out;
    const std::byte* p = packet.body.data();
    out.block.firstSampleIndex = SampleIndex(rdU64(p));
    out.block.firstTick = DeviceTick(rdU64(p + 8));
    out.block.sampleCount = rdU32(p + 16);
    out.block.channelCount = rdU16(p + 20);
    out.block.strideBytes = rdU8(p + 22);
    out.block.channelMask = rdU64(p + 24);
    out.block.sampleRateHz = rdU64(p + 32);
    out.decodedSampleCount = rdU32(p + 40);
    std::uint32_t const runCount = rdU32(p + 44);

    if (!isValidStride(out.block.strideBytes)) {
        return Status::error(ErrorCode::StrideUnsupported,
                             fmt::format("RLE block stride {} is not 1, 2 or 4",
                                         out.block.strideBytes));
    }
    std::uint64_t const expectedBody =
        USN_WIRE_RLE_PREFIX_SIZE + static_cast<std::uint64_t>(runCount) * sizeof(UsnRleRun);
    if (packet.body.size() != expectedBody) {
        return Status::error(
            ErrorCode::InvalidPacketLength,
            fmt::format("RLE body size {} does not match prefix + {} runs = {}", packet.body.size(),
                        runCount, expectedBody));
    }
    out.runs.resize(runCount);
    for (std::uint32_t i = 0; i < runCount; ++i) {
        const std::byte* rp = p + USN_WIRE_RLE_PREFIX_SIZE + i * sizeof(UsnRleRun);
        out.runs[i].word = rdU32(rp);
        out.runs[i].count = rdU32(rp + 4);
    }
    // Validate the expansion BEFORE any allocation: a corrupt count must not be
    // able to make the host allocate gigabytes.
    std::uint64_t const total = usn_rle_total_samples(out.runs.data(), runCount);
    if (total != out.decodedSampleCount) {
        return Status::error(
                   ErrorCode::InvalidPacketLength,
                   fmt::format("RLE decodedSampleCount {} does not match the sum of run counts {}",
                               out.decodedSampleCount, total))
            .withSampleIndex(out.block.firstSampleIndex.value);
    }
    out.block.sampleCount = out.decodedSampleCount;
    return out;
}

StatusOr<std::vector<std::byte>> PacketCodec::expandRle(const RleBlockPayload& rle,
                                                       std::uint32_t const maxSamples) {
    if (rle.decodedSampleCount == 0) {
        return Status::error(ErrorCode::InvalidPacketLength, "RLE block decodes to zero samples");
    }
    if (rle.decodedSampleCount > maxSamples) {
        return Status::error(
                   ErrorCode::OutOfMemory,
                   fmt::format("RLE expansion to {} samples exceeds the configured limit {}",
                               rle.decodedSampleCount, maxSamples))
            .withSampleIndex(rle.block.firstSampleIndex.value);
    }
    std::vector<std::byte> out(static_cast<std::size_t>(rle.decodedSampleCount) *
                               rle.block.strideBytes);
    std::uint32_t written = 0;
    int const rc = usn_rle_decode(rle.runs.data(), static_cast<std::uint32_t>(rle.runs.size()),
                                  rle.block.strideBytes, out.data(), rle.decodedSampleCount,
                                  &written);
    if (rc != USN_RLE_OK) {
        return Status::error(ErrorCode::FileCorrupt,
                             fmt::format("RLE decode failed with status {}", rc));
    }
    if (written != rle.decodedSampleCount) {
        return Status::error(ErrorCode::FileCorrupt,
                             fmt::format("RLE decode produced {} samples, expected {}", written,
                                         rle.decodedSampleCount));
    }
    return out;
}

// --- building ----------------------------------------------------------------

std::vector<std::byte> PacketCodec::buildPacket(std::uint8_t const packetType,
                                                std::uint8_t const flags,
                                                std::uint32_t const sequence,
                                                std::uint64_t const streamId,
                                                std::span<const std::byte> body) {
    std::vector<std::byte> out;
    out.reserve(USN_WIRE_HEADER_SIZE + body.size());

    std::vector<std::byte> header;
    header.reserve(USN_WIRE_HEADER_SIZE);
    wrU32(header, USN_WIRE_MAGIC_VALUE);
    header.push_back(static_cast<std::byte>(USN_WIRE_VERSION));
    header.push_back(static_cast<std::byte>(packetType));
    header.push_back(static_cast<std::byte>(flags));
    header.push_back(static_cast<std::byte>(USN_WIRE_HEADER_SIZE));
    wrU32(header, sequence);
    wrU32(header, static_cast<std::uint32_t>(body.size()));
    wrU64(header, streamId);
    wrU32(header, usn_crc32c(body.data(), body.size()));
    // Header CRC covers bytes [0..27]; the field itself sits at [28..31].
    wrU32(header, usn_crc32c(header.data(), 28));

    out.insert(out.end(), header.begin(), header.end());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::vector<std::byte> PacketCodec::buildSampleBlockBody(const BlockHeader& header,
                                                        std::span<const std::byte> payload) {
    std::vector<std::byte> body;
    body.reserve(USN_WIRE_SIZE_PREFIX + payload.size());
    wrU64(body, header.firstSampleIndex.value);
    wrU64(body, header.firstTick.value);
    wrU32(body, header.sampleCount);
    wrU16(body, header.channelCount);
    body.push_back(static_cast<std::byte>(header.strideBytes));
    body.push_back(static_cast<std::byte>(0));  // reserved
    wrU64(body, header.channelMask);
    wrU64(body, header.sampleRateHz);
    body.insert(body.end(), payload.begin(), payload.end());
    return body;
}

std::vector<std::byte> PacketCodec::buildCommandBody(std::uint16_t const commandId,
                                                    std::span<const std::byte> args) {
    std::vector<std::byte> body;
    body.reserve(4 + args.size());
    wrU16(body, commandId);
    wrU16(body, static_cast<std::uint16_t>(args.size()));
    body.insert(body.end(), args.begin(), args.end());
    return body;
}

}  // namespace usn::transport
