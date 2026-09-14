// -----------------------------------------------------------------------------
// wire_dumper -- decode a raw Universal Sniffer byte stream and report everything
// the framing layer noticed.
//
// Why this exists as a tool and not only as a test: when a real Teensy produces a
// stream the GUI renders oddly, the first question is always "is the wire data
// itself intact?". Answering that must not require starting the application,
// attaching a display, or trusting the same code path that is under suspicion.
//
// Exit status is meaningful: 0 means the stream decoded with no integrity events,
// 1 means it did not (or the input could not be read). That makes it usable in a
// pipeline and in CI.
// -----------------------------------------------------------------------------
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/core.h>

#include "usn/model/diagnostics.h"
#include "usn/transport/packet_codec.h"
#include "usn_wire.h"

namespace {

struct Options {
    std::filesystem::path input;
    std::size_t chunkBytes = 65536;
    std::uint64_t maxPackets = 0;          // 0 = unlimited
    bool requireCrc = false;
    bool trackContinuity = true;
    bool summaryOnly = false;
    bool allowDirty = false;
    bool printDiagnostics = true;
};

void printUsage(std::string_view program) {
    fmt::print(
        "Usage: {} [options] <input.bin | ->\n"
        "\n"
        "Decodes a raw Universal Sniffer wire stream and reports framing integrity.\n"
        "\n"
        "Options:\n"
        "  --chunk <bytes>       feed the codec in chunks of this size (default 65536).\n"
        "                        Small values exercise the split-packet buffering path.\n"
        "  --max-packets <n>     stop after n packets (default unlimited)\n"
        "  --require-crc         reject packets that do not set CRC_PRESENT\n"
        "  --no-continuity       disable sequence / sample-index gap detection\n"
        "  --summary-only        print framing statistics, not per-packet lines\n"
        "  --no-diagnostics      do not print the diagnostic events the codec raised\n"
        "  --allow-dirty         exit 0 even when integrity events were recorded\n"
        "  -h, --help            this text\n"
        "\n"
        "Exit status:\n"
        "  0  stream decoded with no CRC failures, resyncs or gaps\n"
        "  1  integrity events were recorded, or the input could not be read\n",
        program);
}

bool parseArgs(int argc, char** argv, Options& out) {
    if (argc < 2) {
        return false;
    }
    for (int i = 1; i < argc; ++i) {
        std::string_view const arg = argv[i];
        auto const nextValue = [&](std::string_view& dest) -> bool {
            if (i + 1 >= argc) {
                fmt::print(stderr, "error: {} requires a value\n", arg);
                return false;
            }
            dest = argv[++i];
            return true;
        };
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            std::exit(0);
        } else if (arg == "--require-crc") {
            out.requireCrc = true;
        } else if (arg == "--no-continuity") {
            out.trackContinuity = false;
        } else if (arg == "--summary-only") {
            out.summaryOnly = true;
        } else if (arg == "--no-diagnostics") {
            out.printDiagnostics = false;
        } else if (arg == "--allow-dirty") {
            out.allowDirty = true;
        } else if (arg == "--chunk") {
            std::string_view value;
            if (!nextValue(value)) {
                return false;
            }
            out.chunkBytes = static_cast<std::size_t>(std::stoull(std::string(value)));
            if (out.chunkBytes == 0) {
                fmt::print(stderr, "error: --chunk must be greater than zero\n");
                return false;
            }
        } else if (arg == "--max-packets") {
            std::string_view value;
            if (!nextValue(value)) {
                return false;
            }
            out.maxPackets = std::stoull(std::string(value));
        } else if (arg.starts_with("--")) {
            fmt::print(stderr, "error: unknown option '{}'\n", arg);
            return false;
        } else {
            out.input = arg;
        }
    }
    if (out.input.empty()) {
        fmt::print(stderr, "error: no input file given ('-' reads stdin)\n");
        return false;
    }
    return true;
}

std::string_view packetTypeName(std::uint8_t const type) {
    switch (type) {
    case USN_PKT_INVALID:           return "INVALID";
    case USN_PKT_DEVICE_HELLO:      return "DEVICE_HELLO";
    case USN_PKT_CAPABILITIES:      return "CAPABILITIES";
    case USN_PKT_COMMAND:           return "COMMAND";
    case USN_PKT_COMMAND_ACK:       return "COMMAND_ACK";
    case USN_PKT_CAPTURE_START_ACK: return "CAPTURE_START_ACK";
    case USN_PKT_CAPTURE_STOP_ACK:  return "CAPTURE_STOP_ACK";
    case USN_PKT_SAMPLE_BLOCK:      return "SAMPLE_BLOCK";
    case USN_PKT_SAMPLE_BLOCK_RLE:  return "SAMPLE_BLOCK_RLE";
    case USN_PKT_TRIGGER_EVENT:     return "TRIGGER_EVENT";
    case USN_PKT_DIAGNOSTIC_EVENT:  return "DIAGNOSTIC_EVENT";
    case USN_PKT_HEARTBEAT:         return "HEARTBEAT";
    case USN_PKT_TIME_SYNC:         return "TIME_SYNC";
    case USN_PKT_PING:              return "PING";
    case USN_PKT_PONG:              return "PONG";
    case USN_PKT_ERROR:             return "ERROR";
    case USN_PKT_STREAM_END:        return "STREAM_END";
    default:                        return "UNKNOWN";
    }
}

std::string flagNames(std::uint8_t const flags) {
    struct NamedFlag { std::uint8_t bit; const char* name; };
    static constexpr NamedFlag kNames[] = {
        {USN_FLAG_COMPRESSED_RLE,  "RLE"},
        {USN_FLAG_FIRST_OF_STREAM, "FIRST"},
        {USN_FLAG_LAST_OF_STREAM,  "LAST"},
        {USN_FLAG_OVERFLOW_BEFORE, "OVERFLOW"},
        {USN_FLAG_CRC_PRESENT,     "CRC"},
        {USN_FLAG_DEVICE_RESET,    "RESET"},
        {USN_FLAG_TRIGGER_FIRED,   "TRIGGERED"},
        {USN_FLAG_RESERVED,        "RESERVED"},
    };
    std::string out;
    for (const auto& named : kNames) {
        if ((flags & named.bit) != 0) {
            if (!out.empty()) {
                out += '|';
            }
            out += named.name;
        }
    }
    return out.empty() ? std::string("-") : out;
}

bool readChunk(std::istream& in, std::vector<std::byte>& buffer, std::size_t want) {
    buffer.resize(want);
    in.read(reinterpret_cast<char*>(buffer.data()),   // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
            static_cast<std::streamsize>(want));
    auto const got = in.gcount();
    buffer.resize(got < 0 ? 0 : static_cast<std::size_t>(got));
    return !buffer.empty();
}

void printSampleBlock(const usn::transport::ParsedPacket& packet) {
    auto parsed = usn::transport::PacketCodec::parseSampleBlock(packet);
    if (!parsed.ok()) {
        fmt::print("      body: UNPARSEABLE ({})\n", parsed.status().message());
        return;
    }
    const auto& block = *parsed;
    fmt::print(
        "      block: firstSample={} count={} channels={} stride={}B rate={}Hz mask=0x{:x}\n",
        block.firstSampleIndex.value, block.sampleCount, block.channelCount, block.strideBytes,
        block.sampleRateHz, block.channelMask);
}

void printRleBlock(const usn::transport::ParsedPacket& packet) {
    auto parsed = usn::transport::PacketCodec::parseRleBlock(packet);
    if (!parsed.ok()) {
        fmt::print("      body: UNPARSEABLE ({})\n", parsed.status().message());
        return;
    }
    const auto& rle = *parsed;
    fmt::print("      rle: firstSample={} decodedCount={} runs={} channels={} stride={}B\n",
               rle.block.firstSampleIndex.value, rle.decodedSampleCount, rle.runs.size(),
               rle.block.channelCount, rle.block.strideBytes);
}

void printStats(const usn::transport::FramingStats& s) {
    fmt::print("\n--- framing statistics ---\n");
    fmt::print("bytes received          : {}\n", s.bytesReceived);
    fmt::print("packets accepted        : {}\n", s.packetsOk);
    fmt::print("body CRC failures       : {}\n", s.crcFailures);
    fmt::print("header CRC failures     : {}\n", s.headerCrcFailures);
    fmt::print("header version rejects  : {}\n", s.headerVersionRejects);
    fmt::print("length rejects          : {}\n", s.lengthRejects);
    fmt::print("reserved flag rejects   : {}\n", s.reservedFlagRejects);
    fmt::print("unknown type skips      : {}\n", s.unknownTypeSkips);
    fmt::print("body parse failures     : {}\n", s.bodyParseFailures);
    fmt::print("resync events           : {}\n", s.resyncEvents);
    fmt::print("bytes skipped in resync : {}\n", s.bytesSkippedDuringResync);
    fmt::print("sequence gaps           : {} (largest {})\n", s.sequenceGaps, s.maxGapSize);
    fmt::print("sample index gaps       : {}\n", s.sampleIndexGaps);
    fmt::print("stream id changes       : {}\n", s.streamIdChanges);
}

bool isClean(const usn::transport::FramingStats& s) {
    return s.crcFailures == 0 && s.headerCrcFailures == 0 && s.headerVersionRejects == 0 &&
           s.lengthRejects == 0 && s.reservedFlagRejects == 0 && s.bodyParseFailures == 0 &&
           s.resyncEvents == 0 && s.sequenceGaps == 0 && s.sampleIndexGaps == 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseArgs(argc, argv, options)) {
        printUsage(argv[0]);
        return 1;
    }

    std::ifstream fileStream;
    std::istream* in = &std::cin;
    if (options.input != "-") {
        std::error_code ec;
        if (!std::filesystem::exists(options.input, ec)) {
            fmt::print(stderr, "error: '{}' does not exist\n", options.input.string());
            return 1;
        }
        fileStream.open(options.input, std::ios::binary);
        if (!fileStream) {
            fmt::print(stderr, "error: could not open '{}'\n", options.input.string());
            return 1;
        }
        in = &fileStream;
    }

    usn::transport::CodecConfig codecConfig;
    codecConfig.requireCrc = options.requireCrc;
    codecConfig.trackContinuity = options.trackContinuity;
    usn::transport::PacketCodec codec(codecConfig);

    fmt::print("wire_dumper: input='{}' chunk={} requireCrc={} continuity={}\n\n",
               options.input.string(), options.chunkBytes, options.requireCrc,
               options.trackContinuity);
    if (!options.summaryOnly) {
        fmt::print("{:>6}  {:<18} {:>8} {:>10} {:>8}  {:<28}\n", "packet", "type", "seq",
                   "streamId", "bodyLen", "flags");
    }

    std::vector<std::byte> buffer;
    std::uint64_t printed = 0;
    bool limitReached = false;
    while (!limitReached && readChunk(*in, buffer, options.chunkBytes)) {
        codec.feed(buffer);
        for (auto& packet : codec.takePackets()) {
            if (!options.summaryOnly) {
                fmt::print("{:>6}  {:<18} {:>8} {:>10} {:>8}  {:<28}\n", printed + 1,
                           packetTypeName(packet.header.packetType), packet.header.sequence,
                           packet.header.streamId, packet.header.bodyLength,
                           flagNames(packet.header.flags));
                if (packet.header.packetType == USN_PKT_SAMPLE_BLOCK) {
                    printSampleBlock(packet);
                } else if (packet.header.packetType == USN_PKT_SAMPLE_BLOCK_RLE) {
                    printRleBlock(packet);
                }
            }
            ++printed;
            if (options.maxPackets != 0 && printed >= options.maxPackets) {
                limitReached = true;
                break;
            }
        }
        if (options.printDiagnostics) {
            for (const auto& diagnostic : codec.takeDiagnostics()) {
                fmt::print(stderr, "diagnostic: {}\n", diagnostic.toString());
            }
        } else {
            (void)codec.takeDiagnostics();
        }
    }
    // Drain whatever the codec still holds; takeDiagnostics() must be called even
    // when printing is off so the counters reflect the whole stream.
    if (options.printDiagnostics) {
        for (const auto& diagnostic : codec.takeDiagnostics()) {
            fmt::print(stderr, "diagnostic: {}\n", diagnostic.toString());
        }
    }

    printStats(codec.stats());
    auto const buffered = codec.bufferedBytes();
    if (buffered != 0) {
        fmt::print("\nwarning: {} byte(s) left in the codec buffer at end of stream "
                   "(a truncated packet)\n",
                   buffered);
    }

    bool clean = isClean(codec.stats()) && buffered == 0;
    fmt::print("\nresult: {}\n", clean ? "CLEAN" : "INTEGRITY EVENTS PRESENT");
    if (printed == 0) {
        fmt::print(stderr, "warning: no complete packet was decoded from this input\n");
    }
    if (!clean && !options.allowDirty) {
        return 1;
    }
    return 0;
}
