// -----------------------------------------------------------------------------
// crc32c.h -- thin C++ facade over the single C implementation in shared/wire.
//
// There is exactly ONE CRC32C implementation in the product (shared/wire/
// usn_wire_crc.c) and the Teensy firmware compiles the same file. That is what
// stops host and device from disagreeing about integrity checking.
// -----------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "usn_wire_crc.h"

namespace usn {

class Crc32c {
public:
    // Streaming use: Crc32c c; c.update(a); c.update(b); auto crc = c.value();
    Crc32c() noexcept = default;

    void update(std::span<const std::byte> data) noexcept {
        m_crc = usn_crc32c_update(m_crc, data.data(), data.size());
    }

    void update(std::span<const std::uint8_t> data) noexcept {
        m_crc = usn_crc32c_update(m_crc, data.data(), data.size());
    }

    [[nodiscard]] std::uint32_t value() const noexcept { return usn_crc32c_final(m_crc); }

    void reset() noexcept { m_crc = USN_CRC32C_INIT; }

    // One-shot use.
    [[nodiscard]] static std::uint32_t compute(std::span<const std::byte> data) noexcept {
        return usn_crc32c(data.data(), data.size());
    }

    [[nodiscard]] static std::uint32_t compute(std::span<const std::uint8_t> data) noexcept {
        return usn_crc32c(data.data(), data.size());
    }

    [[nodiscard]] static bool verify(std::span<const std::byte> data,
                                     std::uint32_t expected) noexcept {
        return usn_crc32c_verify(data.data(), data.size(), expected) != 0;
    }

private:
    std::uint32_t m_crc{USN_CRC32C_INIT};
};

}  // namespace usn
