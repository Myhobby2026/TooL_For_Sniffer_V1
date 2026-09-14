// -----------------------------------------------------------------------------
// wire_dumper_test.cpp -- the command-line tool, run as a separate process.
//
// Exercising the tool through its real argv and its real exit status is the only way
// to test the part a field engineer actually touches: that a clean stream exits 0,
// that a corrupt stream exits non-zero, and that the summary names the problem.
// -----------------------------------------------------------------------------
#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "usn/transport/packet_codec.h"
#include "usn_wire.h"

using usn::BlockFlag;
using usn::BlockHeader;
using usn::DeviceTick;
using usn::SampleIndex;
using usn::transport::PacketCodec;

namespace {

#ifndef USN_WIRE_DUMPER_PATH
#error "USN_WIRE_DUMPER_PATH must be defined by CMake"
#endif

const char* const kTool = USN_WIRE_DUMPER_PATH;

class TempFile {
public:
    explicit TempFile(std::string name)
        : m_path(std::filesystem::temp_directory_path() / std::move(name)) {}
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }
    void write(const std::vector<std::byte>& bytes) const {
        std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open()) << m_path.string();
        out.write(reinterpret_cast<const char*>(bytes.data()),   // NOLINT
                  static_cast<std::streamsize>(bytes.size()));
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};

std::vector<std::byte> packetAt(std::uint64_t const firstSample, std::uint32_t const sequence) {
    BlockHeader header;
    header.firstSampleIndex = SampleIndex(firstSample);
    header.firstTick = DeviceTick(firstSample);
    header.sequence = sequence;
    header.streamId = 7;
    header.sampleCount = 16;
    header.channelCount = 8;
    header.strideBytes = 2;
    header.flags = BlockFlag::None;
    header.channelMask = 0xFFu;
    header.sampleRateHz = 1'000'000;
    std::vector<std::byte> payload(32);
    for (std::uint32_t i = 0; i < 16; ++i) {
        payload[i * 2] = static_cast<std::byte>(i & 0xFFu);
        payload[i * 2 + 1] = std::byte{0};
    }
    auto body = PacketCodec::buildSampleBlockBody(header, payload);
    return PacketCodec::buildPacket(USN_PKT_SAMPLE_BLOCK, USN_FLAG_CRC_PRESENT, sequence, 7, body);
}

int run(const std::string& command) { return std::system(command.c_str()); }

#ifdef _WIN32
int exitCodeOf(int const status) { return status; }
#else
#include <sys/wait.h>
int exitCodeOf(int const status) { return WIFEXITED(status) ? WEXITSTATUS(status) : -1; }
#endif

TEST(WireDumperTest, CleanStreamExitsZeroAndReportsClean) {
    TempFile file("usn_dumper_clean.bin");
    std::vector<std::byte> stream;
    for (std::uint32_t i = 0; i < 8; ++i) {
        auto packet = packetAt(i * 16, i);
        stream.insert(stream.end(), packet.begin(), packet.end());
    }
    file.write(stream);

    auto const outPath = file.path().string() + ".out";
    auto const command = std::string("\"") + kTool + "\" \"" + file.path().string() + "\" > \"" +
                         outPath + "\" 2>&1";
    auto const status = exitCodeOf(run(command));
    std::ifstream report(outPath);
    std::string text((std::istreambuf_iterator<char>(report)), std::istreambuf_iterator<char>());
    std::error_code ec;
    std::filesystem::remove(outPath, ec);

    EXPECT_EQ(status, 0) << "wire_dumper rejected a clean stream:\n" << text;
    EXPECT_NE(text.find("result: CLEAN"), std::string::npos) << text;
    EXPECT_NE(text.find("packets accepted        : 8"), std::string::npos) << text;
    EXPECT_NE(text.find("SAMPLE_BLOCK"), std::string::npos) << text;
}

TEST(WireDumperTest, CorruptStreamExitsNonZeroAndNamesTheProblem) {
    TempFile file("usn_dumper_corrupt.bin");
    std::vector<std::byte> stream;
    for (std::uint32_t i = 0; i < 4; ++i) {
        auto packet = packetAt(i * 16, i);
        stream.insert(stream.end(), packet.begin(), packet.end());
    }
    // Corrupt the body of the second packet: the CRC must catch it.
    auto const victim = packetAt(0, 0).size() + USN_WIRE_HEADER_SIZE + 8;
    stream[victim] = stream[victim] ^ std::byte{0xFF};
    file.write(stream);

    auto const outPath = file.path().string() + ".out";
    auto const command = std::string("\"") + kTool + "\" \"" + file.path().string() + "\" > \"" +
                         outPath + "\" 2>&1";
    auto const status = exitCodeOf(run(command));
    std::ifstream report(outPath);
    std::string text((std::istreambuf_iterator<char>(report)), std::istreambuf_iterator<char>());
    std::error_code ec;
    std::filesystem::remove(outPath, ec);

    EXPECT_NE(status, 0) << "wire_dumper accepted a corrupt stream:\n" << text;
    EXPECT_NE(text.find("INTEGRITY EVENTS PRESENT"), std::string::npos) << text;
}

TEST(WireDumperTest, MissingInputIsAnError) {
    auto const command = std::string("\"") + kTool + "\" /definitely/not/here.bin > /dev/null 2>&1";
    EXPECT_NE(exitCodeOf(run(command)), 0);
}

TEST(WireDumperTest, AllowDirtyOverridesTheExitStatus) {
    TempFile file("usn_dumper_dirty.bin");
    std::vector<std::byte> stream(120, std::byte{0x42});   // no magic anywhere
    file.write(stream);

    auto const strict = std::string("\"") + kTool + "\" \"" + file.path().string() +
                        "\" --summary-only > /dev/null 2>&1";
    EXPECT_NE(exitCodeOf(run(strict)), 0);

    auto const lenient = std::string("\"") + kTool + "\" \"" + file.path().string() +
                         "\" --summary-only --allow-dirty > /dev/null 2>&1";
    EXPECT_EQ(exitCodeOf(run(lenient)), 0);
}

}  // namespace
