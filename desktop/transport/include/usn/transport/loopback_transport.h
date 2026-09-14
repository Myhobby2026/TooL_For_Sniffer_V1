// -----------------------------------------------------------------------------
// loopback_transport.h -- deterministic in-process transport.
//
// Lives in the library, not in tests/, because tools/wire_dumper and the replay
// path use it too. It is also the only ITransport that can inject link faults on
// demand, which is what makes every failure mode in master spec section 14
// testable without hardware:
//
//   corruptNextBytes(n)      flip bytes in the next n delivered
//   truncateNextPacket()     deliver only part of the next packet
//   insertGarbage(bytes)     inject non-packet bytes before the next read
//   stallAfter(bytes)        stop delivering, simulating a wedged link
//   disconnectAfter(bytes)   fail every subsequent read with UsbDisconnected
//   setSplitPattern(n)       deliver at most n bytes per read, so every header
//                            and body split point can be exercised
// -----------------------------------------------------------------------------
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

#include "usn/transport/itransport.h"

namespace usn::transport {

class LoopbackTransport final : public ITransport {
public:
    LoopbackTransport() = default;
    ~LoopbackTransport() final = default;

    Status open(const TransportConfig& config) final;
    void close() final;
    [[nodiscard]] bool isOpen() const noexcept final;
    [[nodiscard]] LinkInfo linkInfo() const final;

    [[nodiscard]] StatusOr<std::size_t> read(std::span<std::byte> dst,
                                             std::chrono::milliseconds timeout) final;
    [[nodiscard]] Status write(std::span<const std::byte> src) final;
    [[nodiscard]] bool writeWouldBlock() const noexcept final;

    // --- test / tooling controls --------------------------------------------
    // Device-to-host direction: what read() will return.
    void pushToDeviceOutput(std::span<const std::byte> bytes);
    void pushToDeviceOutput(const std::vector<std::byte>& bytes);

    // Host-to-device direction: what write() recorded, for command inspection.
    [[nodiscard]] std::vector<std::byte> takeWritten();

    void corruptNextBytes(std::size_t count, std::uint8_t xorMask = 0xFF);
    void truncateNextBytes(std::size_t keepOnly);
    void insertGarbage(std::vector<std::byte> bytes);
    void stallAfter(std::uint64_t bytes);
    void disconnectAfter(std::uint64_t bytes);
    void setSplitPattern(std::size_t maxBytesPerRead);
    void setWriteBlocks(bool blocks) noexcept;

    [[nodiscard]] std::uint64_t deliveredBytes() const noexcept;
    [[nodiscard]] std::size_t pendingBytes() const;

private:
    // m_open, m_writeBlocks and m_delivered are atomic rather than mutex-guarded
    // because the ITransport accessors that read them are declared noexcept. A
    // noexcept function that locks a mutex would call std::terminate if the lock
    // ever failed -- an atomic makes the guarantee real instead of decorative.
    mutable std::mutex m_mutex;
    std::atomic<bool> m_open{false};
    TransportConfig m_config{};
    std::deque<std::byte> m_toHost;
    std::vector<std::byte> m_fromHost;
    LinkInfo m_link{};

    std::size_t m_corruptRemaining{0};
    std::uint8_t m_corruptMask{0xFF};
    std::optional<std::size_t> m_truncateTo;
    std::vector<std::byte> m_pendingGarbage;
    std::optional<std::uint64_t> m_stallAfter;
    std::optional<std::uint64_t> m_disconnectAfter;
    std::size_t m_splitMax{0};   // 0 == no artificial splitting
    std::atomic<bool> m_writeBlocks{false};
    std::atomic<std::uint64_t> m_delivered{0};
};

}  // namespace usn::transport
