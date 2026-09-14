// -----------------------------------------------------------------------------
// decoder_api.h -- the protocol decoder contract
// (docs/architecture_review.md section 11, master spec section 16).
//
// Rule B: a decoder's entire universe is a SampleWindow in and a vector of
// DecodedEvents out, plus its own configuration. No Qt, no sessions, no files, no
// transports, no threads. A decoder must be constructible and runnable inside a
// unit test from a byte vector and nothing else.
//
// The input is SampleWindow rather than std::span<const DigitalSample> (deviation
// D2): samples stay packed at 1/2/4 bytes so an 8-channel capture costs a quarter
// of the RAM, disk and cache of a widened one, which is what makes
// larger-than-RAM capture possible at all. widen() / materializeU32() are there
// for decoders and tests that would rather not deal with the packing.
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "usn/common/status.h"
#include "usn/model/channel.h"
#include "usn/model/events.h"
#include "usn/model/sample.h"

namespace usn::protocol {

// Bumped when the interface changes in a way that breaks existing decoders.
// Plugins declare the version they were built against; a mismatch is refused at
// load time rather than producing undefined behaviour (master spec section 39).
inline constexpr std::uint32_t kDecoderApiVersion = 1;

enum class DecoderCapability : std::uint32_t {
    None = 0,
    Streaming = 1U << 0,          // can process incrementally without lookback
    LazyDecode = 1U << 1,         // supports checkpoint/restore for viewport decode
    ProducesTransactions = 1U << 2,
    NeedsBackfill = 1U << 3,      // requires samples before the window start
    SupportsErrorInjection = 1U << 4
};

[[nodiscard]] constexpr DecoderCapability operator|(DecoderCapability a,
                                                    DecoderCapability b) noexcept {
    return static_cast<DecoderCapability>(static_cast<std::uint32_t>(a) |
                                          static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr DecoderCapability operator&(DecoderCapability a,
                                                    DecoderCapability b) noexcept {
    return static_cast<DecoderCapability>(static_cast<std::uint32_t>(a) &
                                          static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr bool has(DecoderCapability set, DecoderCapability flag) noexcept {
    return flag != DecoderCapability::None && (set & flag) == flag;
}

// --- configuration model -----------------------------------------------------
// These descriptors drive the ENTIRE decoder configuration UI generically. SPI's
// CPOL/CPHA/bit-order/word-length/CS-polarity become EnumOption lists and Int
// ranges -- data, not code. A new protocol adds no GUI code (master spec 17).

enum class ConfigType : std::uint8_t {
    Bool = 0, Int, UInt, Enum, Double, Bytes, String, ChannelRole
};

[[nodiscard]] std::string_view nameOf(ConfigType type) noexcept;

struct EnumOption {
    std::int64_t value{0};
    std::string label;
};

struct Range {
    std::int64_t min{0};
    std::int64_t max{0};
};

struct ConfigParameterDescriptor {
    std::string key;
    std::string label;
    std::string tooltip;
    std::string group;
    ConfigType type{ConfigType::Bool};
    FieldValue defaultValue{false};
    std::optional<Range> range;
    std::vector<EnumOption> options;
    bool required{false};
    std::uint32_t apiVersion{kDecoderApiVersion};
};

struct ConfigValue {
    std::string key;
    FieldValue value;
};

struct ChannelBinding {
    ChannelRole role{ChannelRole::Unassigned};
    ChannelId channel{};
};

struct DecoderConfiguration {
    std::uint32_t decoderId{0};
    std::vector<ChannelBinding> bindings;
    std::vector<ConfigValue> parameters;
    SampleIndex startHint{};
    bool allowPartialFrames{true};

    [[nodiscard]] const ConfigValue* find(std::string_view key) const noexcept;
    [[nodiscard]] const ChannelBinding* bindingFor(ChannelRole role) const noexcept;
};

struct DecoderInfo {
    std::uint32_t id{0};                 // stable numeric id, used in files + plugins
    std::string name;                    // "SPI"
    std::string version;                 // "1.0.0"
    std::uint32_t apiVersion{kDecoderApiVersion};
    std::string description;
    std::vector<std::string> authors;
    std::vector<ChannelRole> requiredRoles;
    std::vector<ChannelRole> optionalRoles;
    DecoderCapability capabilities{DecoderCapability::None};
    std::vector<ConfigParameterDescriptor> parameters;

    // Declared bound on output size, used by the conformance test to prove a
    // decoder cannot buffer without limit while waiting for a condition that never
    // arrives (e.g. an I2C STOP that never comes).
    std::uint64_t maxEventsPerSample{4};
};

// Opaque, serializable decoder state at a SampleIndex. This is what makes lazy and
// parallel decode of larger-than-RAM captures CORRECT rather than merely fast:
// restoring at index k and processing [k,n) must equal uninterrupted processing.
using DecoderCheckpoint = std::vector<std::byte>;

class IProtocolDecoder {
public:
    IProtocolDecoder() = default;
    IProtocolDecoder(const IProtocolDecoder&) = delete;
    IProtocolDecoder& operator=(const IProtocolDecoder&) = delete;
    IProtocolDecoder(IProtocolDecoder&&) = delete;
    IProtocolDecoder& operator=(IProtocolDecoder&&) = delete;
    virtual ~IProtocolDecoder() = default;

    [[nodiscard]] virtual DecoderInfo info() const = 0;

    // Must validate fully and apply atomically: a partially applied configuration
    // would leave the decoder in a state nobody can reason about.
    [[nodiscard]] virtual Status configure(const DecoderConfiguration& config) = 0;
    [[nodiscard]] virtual DecoderConfiguration configuration() const = 0;

    virtual void reset() = 0;

    // The window is borrowed and must not be retained past the call.
    // Deterministic: same window + same state => same events, always.
    [[nodiscard]] virtual Status process(const SampleWindow& window) = 0;

    // Moves out everything produced since the last call.
    [[nodiscard]] virtual std::vector<DecodedEvent> takeEvents() = 0;

    // Aggregation into protocol-level units. Default empty for decoders that only
    // emit atomic events.
    [[nodiscard]] virtual std::vector<Transaction> takeTransactions() { return {}; }

    [[nodiscard]] virtual DecoderCheckpoint saveCheckpoint() const = 0;
    [[nodiscard]] virtual Status restoreCheckpoint(const DecoderCheckpoint& checkpoint) = 0;

    [[nodiscard]] virtual std::uint64_t samplesProcessed() const noexcept = 0;

    // Convenience for tests and for decoders that prefer widened input.
    [[nodiscard]] std::vector<std::uint32_t> widen(const SampleWindow& window) const {
        return window.materializeU32();
    }
};

// Validates a configuration against the decoder's declared parameters: presence of
// required keys, type agreement, enum membership, numeric ranges, and required
// channel bindings. Shared by every decoder so validation cannot drift.
[[nodiscard]] Status validateConfiguration(const DecoderInfo& info,
                                           const DecoderConfiguration& config);

}  // namespace usn::protocol
