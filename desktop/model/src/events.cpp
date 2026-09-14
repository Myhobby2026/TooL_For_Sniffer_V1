// -----------------------------------------------------------------------------
// events.cpp -- see events.h.
// -----------------------------------------------------------------------------
#include "usn/model/events.h"

#include <fmt/format.h>

namespace usn {

std::string_view nameOf(EventType const type) noexcept {
    switch (type) {
    case EventType::Edge:             return "edge";
    case EventType::LevelChange:      return "level-change";
    case EventType::PulseWidth:       return "pulse-width";
    case EventType::Gap:              return "gap";
    case EventType::Idle:             return "idle";
    case EventType::SpiTransferBegin: return "spi.transfer-begin";
    case EventType::SpiTransferEnd:   return "spi.transfer-end";
    case EventType::SpiWord:          return "spi.word";
    case EventType::SpiCsAssert:      return "spi.cs-assert";
    case EventType::SpiCsDeassert:    return "spi.cs-deassert";
    case EventType::I2cStart:         return "i2c.start";
    case EventType::I2cRepeatedStart: return "i2c.repeated-start";
    case EventType::I2cStop:          return "i2c.stop";
    case EventType::I2cAddress:       return "i2c.address";
    case EventType::I2cAck:           return "i2c.ack";
    case EventType::I2cNack:          return "i2c.nack";
    case EventType::I2cDataByte:      return "i2c.data";
    case EventType::I2cClockStretch:  return "i2c.clock-stretch";
    case EventType::UartStartBit:     return "uart.start-bit";
    case EventType::UartDataBits:     return "uart.data-bits";
    case EventType::UartParityBit:    return "uart.parity-bit";
    case EventType::UartStopBit:      return "uart.stop-bit";
    case EventType::UartBreak:        return "uart.break";
    case EventType::UartFrame:        return "uart.frame";
    case EventType::CanFrame:         return "can.frame";
    case EventType::CanIdField:       return "can.id";
    case EventType::CanErrorFrame:    return "can.error-frame";
    case EventType::LinBreak:         return "lin.break";
    case EventType::LinSync:          return "lin.sync";
    case EventType::LinPid:           return "lin.pid";
    case EventType::LinFrame:         return "lin.frame";
    case EventType::ProtocolError:    return "protocol-error";
    case EventType::DecoderNote:      return "decoder-note";
    }
    return "unknown";
}

std::string_view nameOf(EventStatus const status) noexcept {
    switch (status) {
    case EventStatus::Ok:         return "ok";
    case EventStatus::Warning:    return "warning";
    case EventStatus::Error:      return "error";
    case EventStatus::Incomplete: return "incomplete";
    }
    return "unknown";
}

std::string_view nameOf(Direction const direction) noexcept {
    switch (direction) {
    case Direction::Unknown:       return "?";
    case Direction::Read:          return "read";
    case Direction::Write:         return "write";
    case Direction::Bidirectional: return "r/w";
    }
    return "?";
}

std::string toDisplayString(const FieldValue& value) {
    return std::visit(
        [](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) {
                return v ? "true" : "false";
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return fmt::format("{}", v);
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                return fmt::format("{}", v);
            } else if constexpr (std::is_same_v<T, double>) {
                return fmt::format("{:g}", v);
            } else if constexpr (std::is_same_v<T, std::string>) {
                return v;
            } else {
                // Byte vectors render as canonical uppercase hex with single
                // spaces, so golden-file diffs are stable and readable.
                std::string out;
                out.reserve(v.size() * 3);
                for (std::size_t i = 0; i < v.size(); ++i) {
                    if (i != 0) {
                        out += ' ';
                    }
                    out += fmt::format("{:02X}", std::to_integer<int>(v[i]));
                }
                return out;
            }
        },
        value);
}

}  // namespace usn
