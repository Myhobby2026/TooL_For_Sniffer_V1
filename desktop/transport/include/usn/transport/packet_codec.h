// -----------------------------------------------------------------------------
// packet_codec.h -- framing, integrity and resynchronisation
// (docs/architecture_review.md section 8).
//
// A pure function over byte streams: no I/O, no threads, no device. Every
// corruption mode in master spec section 14 can therefore be injected as a byte
// vector in a unit test, with no hardware and no timing.
//
// The parser is TOTAL: for any input byte sequence -- random, truncated,
// adversarial -- it terminates, bounds its memory to 65536 + 32 bytes, and never
// invokes undefined behaviour. That property is fuzz-tested, not assumed.
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <string>
#include <vector>

#include "usn/common/status.h"
#include "usn/model/diagnostics.h"
#include "usn/model/sample.h"
#include "usn/model/time.h"
#include "usn_wire.h"

namespace usn::transport {

// Host-order view of the 32-byte wire header.
struct PacketHeader {
    std::uint32_t magic{0};
    std::uint8_t headerVersion{0};
    std::uint8_t packetType{0};
    std::uint8_t flags{0};
    std::uint8_t headerLength{0};
    std::uint32_t sequence{0};
    std::uint32_t bodyLength{0};
    std::uint64_t streamId{0};
    std::uint32_t bodyCrc32c{0};
    std::uint32_t headerCrc32c{0};

    [[nodiscard]] bool hasFlag(std::uint8_t flag) const noexcept { return (flags & flag) != 0; }
    [[nodiscard]] bool isCompressed() const noexcept { return hasFlag(USN_FLAG_COMPRESSED_RLE); }
    [[nodiscard]] bool crcPresent() const noexcept { return hasFlag(USN_FLAG_CRC_PRESENT); }
};

struct ParsedPacket {
    PacketHeader header;
    std::vector<std::byte> body;
};

// Parsed sample-block body, host order.
struct SampleBlockPayload {
    SampleIndex firstSampleIndex{};
    DeviceTick firstTick{};
    std::uint32_t sampleCount{0};
    std::uint16_t channelCount{0};
    std::uint8_t strideBytes{0};
    std::uint64_t channelMask{0};
    std::uint64_t sampleRateHz{0};
    std::span<const std::byte> payload;
};

struct RleBlockPayload {
    SampleBlockPayload block;
    std::uint32_t decodedSampleCount{0};
    std::vector<UsnRleRun> runs;
};

struct FramingStats {
    std::uint64_t bytesReceived{0};
    std::uint64_t packetsOk{0};
    std::uint64_t crcFailures{0};
    std::uint64_t headerCrcFailures{0};
    std::uint64_t headerVersionRejects{0};
    std::uint64_t lengthRejects{0};
    std::uint64_t reservedFlagRejects{0};
    std::uint64_t unknownTypeSkips{0};
    std::uint64_t resyncEvents{0};
    std::uint64_t bytesSkippedDuringResync{0};
    std::uint64_t sequenceGaps{0};
    std::uint32_t maxGapSize{0};
    std::uint64_t sampleIndexGaps{0};
    std::uint64_t streamIdChanges{0};
    std::uint64_t bodyParseFailures{0};
};

struct CodecConfig {
    std::uint32_t maxBodyBytes{USN_WIRE_MAX_BODY_LENGTH};
    bool requireCrc{false};           // reject packets with CRC_PRESENT clear
    bool strictMagic{true};
    bool trackContinuity{true};       // sequence + sample-index gap detection
    std::uint32_t initialExpectedSequence{0};
    bool haveExpectedSequence{false};
};

class PacketCodec {
public:
    explicit PacketCodec(CodecConfig config = {});

    PacketCodec(const PacketCodec&) = delete;
    PacketCodec& operator=(const PacketCodec&) = delete;

    // Feed an arbitrary byte range. Split points do not matter: partial headers and
    // partial bodies are buffered across calls.
    void feed(std::span<const std::byte> bytes);

    // Moves out every complete validated packet produced so far.
    std::vector<ParsedPacket> takePackets();

    [[nodiscard]] const FramingStats& stats() const noexcept { return m_stats; }
    [[nodiscard]] const CodecConfig& config() const noexcept { return m_config; }

    // Continuity tracking state.
    [[nodiscard]] std::uint32_t expectedSequence() const noexcept { return m_expectedSequence; }
    [[nodiscard]] std::optional<SampleIndex> nextExpectedSampleIndex() const noexcept {
        return m_nextSampleIndex;
    }

    // Diagnostics accumulated since the last call. Drained by the pipeline so that
    // every rejection surfaces somewhere visible (master spec section 14).
    std::vector<usn::DiagnosticEvent> takeDiagnostics();

    void reset() noexcept;

    // --- body parsing (separate so it can be unit-tested on a known-good body) --
    [[nodiscard]] static StatusOr<SampleBlockPayload> parseSampleBlock(
        const ParsedPacket& packet);
    [[nodiscard]] static StatusOr<RleBlockPayload> parseRleBlock(const ParsedPacket& packet);

    // Expands an RLE body into packed samples, refusing to allocate more than
    // maxSamples. A corrupt decodedSampleCount cannot cause a huge allocation.
    [[nodiscard]] static StatusOr<std::vector<std::byte>> expandRle(const RleBlockPayload& rle,
                                                                    std::uint32_t maxSamples);

    // --- building (host -> device, and used by test fixtures) ------------------
    // Serialises a complete packet including both CRCs.
    [[nodiscard]] static std::vector<std::byte> buildPacket(std::uint8_t packetType,
                                                            std::uint8_t flags,
                                                            std::uint32_t sequence,
                                                            std::uint64_t streamId,
                                                            std::span<const std::byte> body);

    // Serialises a sample block body (prefix + packed payload).
    [[nodiscard]] static std::vector<std::byte> buildSampleBlockBody(
        const BlockHeader& header, std::span<const std::byte> payload);

    // Serialises a host->device COMMAND packet body.
    [[nodiscard]] static std::vector<std::byte> buildCommandBody(std::uint16_t commandId,
                                                                std::span<const std::byte> args);

    // Current peak buffered bytes, exposed so the memory bound is testable.
    [[nodiscard]] std::size_t bufferedBytes() const noexcept;
    [[nodiscard]] static constexpr std::size_t maxBufferedBytes(const CodecConfig& c) noexcept {
        return static_cast<std::size_t>(c.maxBodyBytes) + USN_WIRE_HEADER_SIZE + 4;
    }

private:
    // Only two states. A third "ReadBody" state was removed because every
    // rejection path would have had to remember to reset it, and one that forgot
    // would spin the feed() loop forever doing nothing.
    enum class State : std::uint8_t { HuntMagic, ReadHeader };

    void processBuffered();
    bool tryValidateHeader();
    void resync(std::size_t advanceBy, ErrorCode reason, std::string message);
    // string_view, not string: Status::message() returns a view, and taking a
    // string would force a conversion at every call site.
    void emitDiagnostic(ErrorCode code, std::string_view message);
    void checkSampleContinuity(SampleIndex first, std::uint32_t count);

    CodecConfig m_config;
    State m_state{State::HuntMagic};
    std::vector<std::byte> m_buffer;
    std::vector<ParsedPacket> m_packets;
    std::vector<usn::DiagnosticEvent> m_diagnostics;
    FramingStats m_stats;

    std::uint32_t m_expectedSequence{0};
    // Whether the expected sequence has been seeded yet. Without this the seeding
    // below would run on every packet (haveExpectedSequence is a CONFIG flag, not a
    // per-packet one), which made the gap check that follows compare a value against
    // itself and therefore never fire: dropped packets would go unreported unless
    // they also produced a sample-index gap.
    bool m_sequenceSeeded{false};
    std::optional<SampleIndex> m_nextSampleIndex;
    std::optional<std::uint64_t> m_currentStreamId;
    // One diagnostic per garbage episode rather than one per discarded byte, so a
    // noisy link does not flood the Diagnostics panel while still never being
    // silent about the fact that data was thrown away.
    bool m_garbageReported{false};
};

}  // namespace usn::transport
