// -----------------------------------------------------------------------------
// itransport.h -- the transport abstraction (docs/architecture_review.md 4.5).
//
// Transport moves bytes. It does not know what they mean: no decoding, no capture
// policy, no session state. Framing and integrity belong to PacketCodec, which is
// a pure function over byte streams and therefore testable without any transport.
//
// Deviation D3: the concrete serial back-end uses Win32 overlapped I/O / POSIX
// termios rather than QSerialPort, so that usn::transport -- and everything above
// it except app/gui -- stays Qt-free and headless-testable.
// -----------------------------------------------------------------------------
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "usn/common/status.h"
#include "usn/model/capabilities.h"

namespace usn::transport {

enum class TransportKind : std::uint8_t {
    Loopback = 0,   // in-process; tests and offline analysis
    File,           // read-only replay of a recorded byte stream
    UsbCdc,         // Teensy native USB serial (Phase 3)
    UsbVendorBulk,  // future: WinUSB/libusb custom endpoint
    Ethernet        // future: 10/100 PHY
};

[[nodiscard]] std::string_view nameOf(TransportKind kind) noexcept;

struct TransportConfig {
    TransportKind kind{TransportKind::Loopback};
    std::string endpoint;                     // "COM7", "/dev/ttyACM0", file path
    std::uint32_t baudRate{0};                // 0 == native USB, no baud concept
    std::chrono::milliseconds readTimeout{250};
    std::chrono::milliseconds writeTimeout{1000};
    std::size_t readChunkBytes{64 * 1024};
};

struct LinkInfo {
    std::string name;
    TransportKind kind{TransportKind::Loopback};
    bool open{false};
    UsbSpeedClass usbSpeed{UsbSpeedClass::Unknown};
    // Measured, continuously updated. Zero until enough bytes have moved to say
    // anything meaningful -- never a vendor claim.
    std::uint64_t measuredBytesPerSec{0};
    std::uint64_t bytesRead{0};
    std::uint64_t bytesWritten{0};
    bool flowControlled{false};
};

class ITransport {
public:
    ITransport() = default;
    ITransport(const ITransport&) = delete;
    ITransport& operator=(const ITransport&) = delete;
    ITransport(ITransport&&) = delete;
    ITransport& operator=(ITransport&&) = delete;
    virtual ~ITransport() = default;

    virtual Status open(const TransportConfig& config) = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool isOpen() const noexcept = 0;
    [[nodiscard]] virtual LinkInfo linkInfo() const = 0;

    // Blocking read with a timeout. Returns the number of bytes written into dst
    // (0 on timeout, which is NOT an error). Never throws.
    [[nodiscard]] virtual StatusOr<std::size_t> read(std::span<std::byte> dst,
                                                     std::chrono::milliseconds timeout) = 0;
    [[nodiscard]] virtual Status write(std::span<const std::byte> src) = 0;

    // Backpressure must be observable, not inferred from a failed write: when the
    // host stops consuming, the device blocks and samples are lost. That loss has
    // to be reportable (master spec section 14).
    [[nodiscard]] virtual bool writeWouldBlock() const noexcept = 0;
};

// Descriptor for a device found by discovery, before a transport is opened.
struct DiscoveredDevice {
    std::string name;
    std::string serial;
    std::string endpoint;
    TransportKind kind{TransportKind::UsbCdc};
    std::string description;
};

struct ReconnectPolicy {
    std::uint32_t maxAttempts{5};          // 0 == retry forever
    std::chrono::milliseconds initialBackoff{100};
    std::chrono::milliseconds maxBackoff{5000};
    double backoffMultiplier{2.0};
    bool jitter{true};                     // avoid thundering-herd on multi-device rigs
};

class TransportManager {
public:
    using Factory =
        std::function<std::unique_ptr<ITransport>(const TransportConfig&)>;

    TransportManager();
    ~TransportManager();

    void registerFactory(TransportKind kind, Factory factory);
    [[nodiscard]] StatusOr<std::unique_ptr<ITransport>> create(const TransportConfig& config);

    void setReconnectPolicy(ReconnectPolicy policy) noexcept;
    [[nodiscard]] const ReconnectPolicy& reconnectPolicy() const noexcept;

    // Backoff for attempt N (0-based). Exposed so reconnect behaviour is testable
    // without sleeping in tests.
    [[nodiscard]] std::chrono::milliseconds backoffForAttempt(std::uint32_t attempt) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace usn::transport
