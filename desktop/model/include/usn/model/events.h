// -----------------------------------------------------------------------------
// events.h -- decoded events and transactions (docs/architecture_review.md
// section 10.2, master spec section 17).
//
// Two granularities, deliberately separated:
//   DecodedEvent  ATOMIC -- one thing that happened, over [begin, end).
//   Transaction   AGGREGATE -- a protocol-level unit of meaning (an SPI transfer,
//                 an I2C message, a UART frame).
//
// The GUI renders both WITHOUT understanding any protocol: it reads
// fields[i].name and fields[i].value. Adding CAN in a later phase must require
// zero GUI changes -- that is the test of whether this model is right.
//
// Positions are SampleIndex, never converted time. Duration is therefore exact
// and integer (master spec section 15).
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "usn/model/channel.h"
#include "usn/model/time.h"

namespace usn {

enum class EventType : std::uint16_t {
    // generic
    Edge = 0,
    LevelChange,
    PulseWidth,
    Gap,
    Idle,
    // SPI
    SpiTransferBegin,
    SpiTransferEnd,
    SpiWord,
    SpiCsAssert,
    SpiCsDeassert,
    // I2C
    I2cStart,
    I2cRepeatedStart,
    I2cStop,
    I2cAddress,
    I2cAck,
    I2cNack,
    I2cDataByte,
    I2cClockStretch,
    // UART
    UartStartBit,
    UartDataBits,
    UartParityBit,
    UartStopBit,
    UartBreak,
    UartFrame,
    // CAN / LIN
    CanFrame,
    CanIdField,
    CanErrorFrame,
    LinBreak,
    LinSync,
    LinPid,
    LinFrame,
    // meta
    ProtocolError,
    DecoderNote
};

[[nodiscard]] std::string_view nameOf(EventType type) noexcept;

enum class EventStatus : std::uint8_t { Ok = 0, Warning, Error, Incomplete };

[[nodiscard]] std::string_view nameOf(EventStatus status) noexcept;

enum class Direction : std::uint8_t { Unknown = 0, Read, Write, Bidirectional };

[[nodiscard]] std::string_view nameOf(Direction direction) noexcept;

// Field values are typed and self-describing. `double` is present because some
// protocol fields really are floating point (e.g. an IEEE-754 payload rendered as
// a float); it is NOT used for timestamps, which stay integer.
using FieldValue =
    std::variant<bool, std::int64_t, std::uint64_t, double, std::string, std::vector<std::byte>>;

struct EventField {
    std::string name;
    FieldValue value;
    std::uint8_t bitOffset{0};   // for the bitfield viewer
    std::uint8_t bitWidth{0};    // 0 == not a bitfield
    std::string unit;            // "Hz", "V", "" -- optional
};

// Convenience for the overwhelmingly common "hex bytes" rendering.
[[nodiscard]] std::string toDisplayString(const FieldValue& value);

struct DecodedEvent {
    SampleIndex begin{};                  // inclusive
    SampleIndex end{};                    // exclusive => duration is exact
    std::string protocol;                 // "SPI", "I2C", "UART"
    ChannelId primaryChannel{};
    std::vector<ChannelId> involvedChannels;
    EventType type{EventType::DecoderNote};
    EventStatus status{EventStatus::Ok};
    std::vector<EventField> fields;
    std::vector<std::byte> payload;
    std::vector<std::uint16_t> errorCodes;
    std::uint32_t decoderId{0};
    std::uint32_t decoderVersion{0};

    [[nodiscard]] std::uint64_t durationSamples() const noexcept {
        return end.value >= begin.value ? end.value - begin.value : 0;
    }
    [[nodiscard]] bool contains(SampleIndex index) const noexcept {
        return index.value >= begin.value && index.value < end.value;
    }
};

struct Transaction {
    std::uint64_t id{0};                  // stable within a session; table model key
    SampleIndex begin{};
    SampleIndex end{};
    std::string protocol;
    std::string bus;                      // user-assigned bus name
    std::string addressOrId;              // I2C addr / CAN id / SPI CS -- display string
    Direction direction{Direction::Unknown};
    std::uint32_t lengthBytes{0};
    std::vector<std::byte> payload;
    std::vector<EventField> summaryFields;
    EventStatus status{EventStatus::Ok};
    std::vector<std::uint16_t> errorCodes;
    std::vector<std::uint32_t> eventIndices;  // into the event store; lazy-loaded
    bool bookmarked{false};
    std::string note;

    [[nodiscard]] std::uint64_t durationSamples() const noexcept {
        return end.value >= begin.value ? end.value - begin.value : 0;
    }
};

}  // namespace usn
