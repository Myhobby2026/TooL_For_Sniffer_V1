// -----------------------------------------------------------------------------
// channel.h -- channel identity and configuration.
//
// A ChannelDescriptor carries a colour and a protocol hint, but core logic must
// never branch on them: they are presentation hints consumed by usn::app only.
// Keeping them here (rather than in the GUI layer) means a saved workspace can
// round-trip them without the core knowing what they mean.
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "usn/common/status.h"

namespace usn {

struct ChannelId {
    std::uint16_t value{0};

    constexpr ChannelId() noexcept = default;
    constexpr explicit ChannelId(std::uint16_t v) noexcept : value(v) {}
    friend constexpr auto operator<=>(ChannelId, ChannelId) noexcept = default;

    static constexpr ChannelId invalid() noexcept { return ChannelId(UINT16_MAX); }
    constexpr bool isValid() const noexcept { return value != UINT16_MAX; }
};

enum class ChannelRole : std::uint16_t {
    Unassigned = 0,
    Clock,
    Data0,
    Data1,
    Data2,
    Data3,
    ChipSelect,
    Tx,
    Rx,
    Sda,
    Scl,
    Frame,
    Error,
    Generic
};

[[nodiscard]] std::string_view nameOf(ChannelRole role) noexcept;

struct ChannelDescriptor {
    ChannelId id{};
    std::string name;
    std::uint32_t physicalPin{0};   // device-reported; drives the pin map diagnostics
    std::uint8_t bitPosition{0};    // bit within the packed sample word
    bool enabled{true};
    bool inverted{false};
    ChannelRole role{ChannelRole::Unassigned};
    std::uint32_t colorArgb{0xFF00CC66};  // presentation hint; ignored by core logic
};

using ChannelMap = std::vector<ChannelDescriptor>;

// Validates that a ChannelMap is usable: unique ids, unique bit positions,
// positions within the stride width. Called before a capture is configured so a
// bad map fails loudly here rather than producing garbage samples later.
[[nodiscard]] Status validateChannelMap(const ChannelMap& map, std::uint8_t strideBytes);

[[nodiscard]] const ChannelDescriptor* findById(const ChannelMap& map, ChannelId id) noexcept;
[[nodiscard]] const ChannelDescriptor* findByBit(const ChannelMap& map,
                                                 std::uint8_t bitPosition) noexcept;

// Builds the 64-bit channelMask from the enabled channels' bit positions.
[[nodiscard]] std::uint64_t channelMaskOf(const ChannelMap& map) noexcept;

}  // namespace usn
