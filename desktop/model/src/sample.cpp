// -----------------------------------------------------------------------------
// sample.cpp -- see sample.h.
// -----------------------------------------------------------------------------
#include "usn/model/sample.h"

#include <bit>

#include <fmt/format.h>

namespace usn {
namespace {

// The one place packed samples are read, so stride handling cannot drift.
std::uint32_t readWordLE(const std::byte* p) noexcept {
    // to_integer<uint32_t> rather than a cast of to_integer<unsigned>: on LP64
    // unsigned and uint32_t are the same type, so the cast is flagged as useless.
    return std::to_integer<std::uint32_t>(p[0]);
}

std::uint32_t readWordAt(const std::byte* base, std::size_t byteCount, std::size_t index,
                         std::uint8_t stride) noexcept {
    if (stride == 0 || base == nullptr) {
        return 0;
    }
    std::size_t const offset = index * stride;
    if (offset + stride > byteCount) {
        return 0;
    }
    const std::byte* p = base + offset;
    switch (stride) {
    case 1:
        return readWordLE(p);
    case 2:
        return readWordLE(p) | (readWordLE(p + 1) << 8);
    case 4:
        return readWordLE(p) | (readWordLE(p + 1) << 8) | (readWordLE(p + 2) << 16) |
               (readWordLE(p + 3) << 24);
    default:
        return 0;
    }
}

}  // namespace

std::uint32_t SampleBlockView::wordAt(std::size_t const index) const noexcept {
    return readWordAt(m_payload.data(), m_payload.size(), index, m_header.strideBytes);
}

Status SampleBlockView::validate() const {
    if (!isValidStride(m_header.strideBytes)) {
        return Status::error(ErrorCode::StrideUnsupported,
                             fmt::format("stride {} is not 1, 2 or 4", m_header.strideBytes))
            .withSampleIndex(m_header.firstSampleIndex.value);
    }
    if (m_header.sampleCount == 0) {
        return Status::error(ErrorCode::InvalidPacketLength, "sample count is zero")
            .withSampleIndex(m_header.firstSampleIndex.value);
    }
    // Implicit conversion is portable here: uint64_t and size_t are both 64-bit
    // unsigned on LP64 and LLP64, so an explicit cast would be flagged as useless
    // by GCC while being required-looking on MSVC.
    std::size_t const expected = m_header.payloadBytes();
    if (m_payload.size() != expected) {
        return Status::error(
                   ErrorCode::InvalidPacketLength,
                   fmt::format("payload size {} does not match sampleCount*stride = {}",
                               m_payload.size(), expected))
            .withSampleIndex(m_header.firstSampleIndex.value)
            .withContext("sampleCount", std::to_string(m_header.sampleCount))
            .withContext("stride", std::to_string(m_header.strideBytes));
    }
    auto const minStride = strideForChannelCount(m_header.channelCount);
    if (minStride == SampleStride::Invalid) {
        return Status::error(
            ErrorCode::ChannelCountUnsupported,
            fmt::format("channel count {} exceeds the 32 channels of wire version 1",
                        m_header.channelCount));
    }
    if (m_header.strideBytes < static_cast<std::uint8_t>(minStride)) {
        return Status::error(
                   ErrorCode::StrideUnsupported,
                   fmt::format("stride {} cannot hold {} channels (needs at least {})",
                               m_header.strideBytes, m_header.channelCount,
                               static_cast<int>(minStride)))
            .withContext("channelCount", std::to_string(m_header.channelCount));
    }
    if (m_header.channelMask != 0) {
        auto const maskBits = static_cast<std::uint16_t>(std::popcount(m_header.channelMask));
        if (m_header.channelCount > maskBits) {
            return Status::error(
                       ErrorCode::ChannelCountUnsupported,
                       fmt::format("channelCount {} exceeds the {} bits set in channelMask",
                                   m_header.channelCount, maskBits))
                .withContext("channelMask", fmt::format("0x{:016x}", m_header.channelMask));
        }
    }
    return Status::success();
}

std::vector<std::uint32_t> OwningSampleBlock::widen() const {
    auto v = view();
    std::vector<std::uint32_t> out;
    out.reserve(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) {
        out.push_back(v.wordAt(i));
    }
    return out;
}

std::uint32_t SampleWindow::wordAt(std::size_t const i) const noexcept {
    return readWordAt(m_packed.data(), m_packed.size(), i, m_stride);
}

Status SampleWindow::validate() const {
    if (!isValidStride(m_stride)) {
        return Status::error(ErrorCode::StrideUnsupported,
                             fmt::format("window stride {} is not 1, 2 or 4", m_stride));
    }
    if (m_packed.size() % m_stride != 0) {
        return Status::error(
            ErrorCode::InvalidPacketLength,
            fmt::format("window payload size {} is not a multiple of stride {}", m_packed.size(),
                        m_stride));
    }
    if (m_sampleRateHz == 0) {
        return Status::error(ErrorCode::SampleRateUnsupported, "window sample rate is zero");
    }
    return Status::success();
}

std::vector<std::uint32_t> SampleWindow::materializeU32() const {
    std::vector<std::uint32_t> out;
    out.reserve(size());
    for (std::size_t i = 0; i < size(); ++i) {
        out.push_back(wordAt(i));
    }
    return out;
}

}  // namespace usn
