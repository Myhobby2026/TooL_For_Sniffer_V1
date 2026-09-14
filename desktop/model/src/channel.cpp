// -----------------------------------------------------------------------------
// channel.cpp -- see channel.h.
// -----------------------------------------------------------------------------
#include "usn/model/channel.h"

#include <algorithm>
#include <set>

#include <fmt/format.h>

#include "usn/model/sample.h"

namespace usn {

std::string_view nameOf(ChannelRole const role) noexcept {
    switch (role) {
    case ChannelRole::Unassigned: return "unassigned";
    case ChannelRole::Clock:      return "clock";
    case ChannelRole::Data0:      return "data0";
    case ChannelRole::Data1:      return "data1";
    case ChannelRole::Data2:      return "data2";
    case ChannelRole::Data3:      return "data3";
    case ChannelRole::ChipSelect: return "cs";
    case ChannelRole::Tx:         return "tx";
    case ChannelRole::Rx:         return "rx";
    case ChannelRole::Sda:        return "sda";
    case ChannelRole::Scl:        return "scl";
    case ChannelRole::Frame:      return "frame";
    case ChannelRole::Error:      return "error";
    case ChannelRole::Generic:    return "generic";
    }
    return "unknown";
}

Status validateChannelMap(const ChannelMap& map, std::uint8_t const strideBytes) {
    if (map.empty()) {
        return Status::error(ErrorCode::ConfigurationError, "channel map is empty");
    }
    if (!isValidStride(strideBytes)) {
        return Status::error(ErrorCode::StrideUnsupported,
                             fmt::format("stride {} is not 1, 2 or 4", strideBytes));
    }
    std::uint16_t enabledCount = 0;
    std::set<std::uint16_t> ids;
    std::set<std::uint8_t> bits;
    auto const maxBit = static_cast<std::uint16_t>(strideBytes) * 8U;

    for (const auto& ch : map) {
        if (!ids.insert(ch.id.value).second) {
            return Status::error(ErrorCode::ConfigurationError,
                                 fmt::format("duplicate channel id {}", ch.id.value))
                .withContext("name", ch.name);
        }
        if (!bits.insert(ch.bitPosition).second) {
            return Status::error(
                       ErrorCode::ConfigurationError,
                       fmt::format("duplicate bit position {}", static_cast<int>(ch.bitPosition)))
                .withContext("name", ch.name)
                .withContext("id", std::to_string(ch.id.value));
        }
        if (ch.bitPosition >= maxBit) {
            return Status::error(
                       ErrorCode::ConfigurationError,
                       fmt::format("bit position {} does not fit in a {}-byte sample word",
                                   static_cast<int>(ch.bitPosition), strideBytes))
                .withContext("name", ch.name);
        }
        if (ch.name.empty()) {
            return Status::error(ErrorCode::ConfigurationError,
                                 fmt::format("channel {} has no name", ch.id.value));
        }
        if (ch.enabled) {
            ++enabledCount;
        }
    }
    if (enabledCount == 0) {
        return Status::error(ErrorCode::ConfigurationError, "no channels are enabled");
    }
    if (enabledCount > maxBit) {
        return Status::error(
            ErrorCode::ChannelCountUnsupported,
            fmt::format("{} enabled channels cannot fit in a {}-byte sample word", enabledCount,
                        strideBytes));
    }
    return Status::success();
}

const ChannelDescriptor* findById(const ChannelMap& map, ChannelId const id) noexcept {
    auto const it = std::find_if(map.begin(), map.end(),
                                 [id](const ChannelDescriptor& c) { return c.id == id; });
    return it == map.end() ? nullptr : &(*it);
}

const ChannelDescriptor* findByBit(const ChannelMap& map, std::uint8_t const bitPosition) noexcept {
    auto const it =
        std::find_if(map.begin(), map.end(),
                     [bitPosition](const ChannelDescriptor& c) { return c.bitPosition == bitPosition; });
    return it == map.end() ? nullptr : &(*it);
}

std::uint64_t channelMaskOf(const ChannelMap& map) noexcept {
    std::uint64_t mask = 0;
    for (const auto& ch : map) {
        if (ch.enabled && ch.bitPosition < 64) {
            mask |= (std::uint64_t{1} << ch.bitPosition);
        }
    }
    return mask;
}

}  // namespace usn
