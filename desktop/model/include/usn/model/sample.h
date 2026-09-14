// -----------------------------------------------------------------------------
// sample.h -- the capture data model (docs/architecture_review.md section 10,
// master spec section 12).
//
// Ownership (master spec section 8):
//   OwningSampleBlock  move-only; allocated by the transport codec, moved through
//                      the pipeline queues, released by the last sink holding it.
//                      NEVER copied.
//   SampleBlockView    borrowed, non-owning, cheap to copy. Points into an
//                      OwningSampleBlock or into a memory-mapped file chunk.
//
// Samples stay packed at the device-native width (1, 2 or 4 bytes). Widening a
// 1-byte-per-sample capture to uint32 would quadruple RAM, disk and cache
// pressure and would defeat larger-than-RAM capture (deviation D2).
// -----------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "usn/common/status.h"
#include "usn/model/time.h"

namespace usn {

enum class SampleStride : std::uint8_t { Invalid = 0, One = 1, Two = 2, Four = 4 };

[[nodiscard]] constexpr SampleStride strideForChannelCount(std::uint16_t channelCount) noexcept {
    if (channelCount == 0) {
        return SampleStride::Invalid;
    }
    if (channelCount <= 8) {
        return SampleStride::One;
    }
    if (channelCount <= 16) {
        return SampleStride::Two;
    }
    if (channelCount <= 32) {
        return SampleStride::Four;
    }
    return SampleStride::Invalid;  // >32 channels is not representable in wire v1
}

[[nodiscard]] constexpr bool isValidStride(std::uint8_t stride) noexcept {
    return stride == 1 || stride == 2 || stride == 4;
}

enum class BlockFlag : std::uint8_t {
    None = 0,
    CompressedRle = 0x01,
    OverflowBefore = 0x08,
    DeviceReset = 0x20,
    TriggerFired = 0x40
};

[[nodiscard]] constexpr BlockFlag operator|(BlockFlag a, BlockFlag b) noexcept {
    return static_cast<BlockFlag>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr BlockFlag operator&(BlockFlag a, BlockFlag b) noexcept {
    return static_cast<BlockFlag>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr bool hasFlag(BlockFlag set, BlockFlag flag) noexcept {
    return (set & flag) == flag && flag != BlockFlag::None;
}

// Header for one block of packed samples. Mirrors UsnSampleBlockPrefix on the
// wire but is a normal C++ type with strong typing.
struct BlockHeader {
    SampleIndex firstSampleIndex{};
    DeviceTick firstTick{};
    std::uint32_t sequence{0};
    std::uint64_t streamId{0};
    std::uint32_t sampleCount{0};
    std::uint16_t channelCount{0};
    std::uint8_t strideBytes{0};
    BlockFlag flags{BlockFlag::None};
    std::uint64_t channelMask{0};
    std::uint64_t sampleRateHz{0};

    [[nodiscard]] SampleIndex lastSampleIndexExclusive() const noexcept {
        return SampleIndex(firstSampleIndex.value + sampleCount);
    }
    [[nodiscard]] std::uint64_t payloadBytes() const noexcept {
        return static_cast<std::uint64_t>(sampleCount) * strideBytes;
    }
    [[nodiscard]] bool contains(SampleIndex index) const noexcept {
        return index.value >= firstSampleIndex.value &&
               index.value < firstSampleIndex.value + sampleCount;
    }
};

// Borrowed view over a packed payload.
class SampleBlockView {
public:
    SampleBlockView() noexcept = default;
    SampleBlockView(const BlockHeader& header, std::span<const std::byte> payload) noexcept
        : m_header(header), m_payload(payload) {}

    [[nodiscard]] const BlockHeader& header() const noexcept { return m_header; }
    [[nodiscard]] std::span<const std::byte> payload() const noexcept { return m_payload; }
    [[nodiscard]] std::size_t size() const noexcept { return m_header.sampleCount; }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    // Stride-aware, little-endian, zero-copy. Out-of-range reads yield 0 rather
    // than invoking UB; callers on the hot path are expected to bound-check once.
    [[nodiscard]] std::uint32_t wordAt(std::size_t index) const noexcept;

    [[nodiscard]] bool bitAt(std::uint8_t bitPosition, std::size_t index) const noexcept {
        return ((wordAt(index) >> bitPosition) & 1U) != 0U;
    }

    [[nodiscard]] SampleIndex sampleIndexOf(std::size_t index) const noexcept {
        return SampleIndex(m_header.firstSampleIndex.value + index);
    }

    // Full consistency check: length, stride, count and mask agreement.
    [[nodiscard]] Status validate() const;

private:
    BlockHeader m_header{};
    std::span<const std::byte> m_payload{};
};

// Owning, move-only block. The unit that flows through pipeline queues.
class OwningSampleBlock {
public:
    OwningSampleBlock() = default;
    explicit OwningSampleBlock(BlockHeader header, std::vector<std::byte> payload = {})
        : m_header(header), m_payload(std::move(payload)) {}

    OwningSampleBlock(const OwningSampleBlock&) = delete;
    OwningSampleBlock& operator=(const OwningSampleBlock&) = delete;
    OwningSampleBlock(OwningSampleBlock&&) noexcept = default;
    OwningSampleBlock& operator=(OwningSampleBlock&&) noexcept = default;
    ~OwningSampleBlock() = default;

    [[nodiscard]] const BlockHeader& header() const noexcept { return m_header; }
    [[nodiscard]] BlockHeader& header() noexcept { return m_header; }
    [[nodiscard]] const std::vector<std::byte>& payload() const noexcept { return m_payload; }
    [[nodiscard]] std::vector<std::byte>& payload() noexcept { return m_payload; }

    [[nodiscard]] SampleBlockView view() const noexcept {
        return SampleBlockView(m_header, std::span<const std::byte>(m_payload));
    }

    [[nodiscard]] Status validate() const { return view().validate(); }

    // Widen to one uint32 per sample. Explicit and opt-in: it costs 4x memory for
    // a 1-byte-stride capture, which is exactly why it is not the default.
    [[nodiscard]] std::vector<std::uint32_t> widen() const;

private:
    BlockHeader m_header{};
    std::vector<std::byte> m_payload{};
};

// A contiguous range of samples that may span several blocks. This is what
// decoders and the measurement engine consume (deviation D2).
class SampleWindow {
public:
    SampleWindow() noexcept = default;
    SampleWindow(SampleIndex origin, std::uint64_t sampleRateHz, std::uint8_t stride,
                 std::span<const std::byte> packed) noexcept
        : m_origin(origin), m_sampleRateHz(sampleRateHz), m_stride(stride), m_packed(packed) {}

    [[nodiscard]] static SampleWindow fromBlock(const OwningSampleBlock& block) noexcept {
        const auto& h = block.header();
        return SampleWindow(h.firstSampleIndex, h.sampleRateHz, h.strideBytes,
                            std::span<const std::byte>(block.payload()));
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return m_stride == 0 ? 0 : m_packed.size() / m_stride;
    }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] SampleIndex origin() const noexcept { return m_origin; }
    [[nodiscard]] SampleIndex endExclusive() const noexcept {
        return SampleIndex(m_origin.value + size());
    }
    [[nodiscard]] std::uint64_t sampleRateHz() const noexcept { return m_sampleRateHz; }
    [[nodiscard]] std::uint8_t strideBytes() const noexcept { return m_stride; }
    [[nodiscard]] std::span<const std::byte> packed() const noexcept { return m_packed; }

    [[nodiscard]] SampleIndex sampleIndexOf(std::size_t i) const noexcept {
        return SampleIndex(m_origin.value + i);
    }
    [[nodiscard]] std::uint32_t wordAt(std::size_t i) const noexcept;
    [[nodiscard]] bool bitAt(std::uint8_t bitPosition, std::size_t i) const noexcept {
        return ((wordAt(i) >> bitPosition) & 1U) != 0U;
    }

    [[nodiscard]] Status validate() const;

    [[nodiscard]] std::vector<std::uint32_t> materializeU32() const;

private:
    SampleIndex m_origin{};
    std::uint64_t m_sampleRateHz{0};
    std::uint8_t m_stride{0};
    std::span<const std::byte> m_packed{};
};

}  // namespace usn
