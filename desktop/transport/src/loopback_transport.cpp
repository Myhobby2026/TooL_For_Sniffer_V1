// -----------------------------------------------------------------------------
// loopback_transport.cpp -- see loopback_transport.h.
// -----------------------------------------------------------------------------
#include "usn/transport/loopback_transport.h"

#include <algorithm>

#include <fmt/format.h>

namespace usn::transport {

std::string_view nameOf(TransportKind const kind) noexcept {
    switch (kind) {
    case TransportKind::Loopback:      return "loopback";
    case TransportKind::File:          return "file";
    case TransportKind::UsbCdc:        return "usb-cdc";
    case TransportKind::UsbVendorBulk: return "usb-vendor-bulk";
    case TransportKind::Ethernet:      return "ethernet";
    }
    return "unknown";
}

Status LoopbackTransport::open(const TransportConfig& config) {
    std::scoped_lock const lock(m_mutex);
    if (m_open) {
        return Status::error(ErrorCode::DeviceBusy, "loopback transport is already open");
    }
    m_config = config;
    m_open.store(true, std::memory_order_release);
    m_link = LinkInfo{};
    m_link.name = config.endpoint.empty() ? std::string("loopback") : config.endpoint;
    m_link.kind = TransportKind::Loopback;
    m_link.open = true;
    m_link.usbSpeed = UsbSpeedClass::HighSpeed;  // simulated link; not a claim
    return Status::success();
}

void LoopbackTransport::close() {
    std::scoped_lock const lock(m_mutex);
    m_open.store(false, std::memory_order_release);
    m_link.open = false;
    m_toHost.clear();
}

bool LoopbackTransport::isOpen() const noexcept { return m_open.load(std::memory_order_acquire); }

LinkInfo LoopbackTransport::linkInfo() const {
    std::scoped_lock const lock(m_mutex);
    return m_link;
}

void LoopbackTransport::pushToDeviceOutput(std::span<const std::byte> bytes) {
    std::scoped_lock const lock(m_mutex);
    m_toHost.insert(m_toHost.end(), bytes.begin(), bytes.end());
}

void LoopbackTransport::pushToDeviceOutput(const std::vector<std::byte>& bytes) {
    pushToDeviceOutput(std::span<const std::byte>(bytes));
}

std::vector<std::byte> LoopbackTransport::takeWritten() {
    std::scoped_lock const lock(m_mutex);
    std::vector<std::byte> out;
    out.swap(m_fromHost);
    return out;
}

void LoopbackTransport::corruptNextBytes(std::size_t const count, std::uint8_t const xorMask) {
    std::scoped_lock const lock(m_mutex);
    m_corruptRemaining = count;
    m_corruptMask = xorMask;
}

void LoopbackTransport::truncateNextBytes(std::size_t const keepOnly) {
    std::scoped_lock const lock(m_mutex);
    m_truncateTo = keepOnly;
}

void LoopbackTransport::insertGarbage(std::vector<std::byte> bytes) {
    std::scoped_lock const lock(m_mutex);
    m_pendingGarbage = std::move(bytes);
}

void LoopbackTransport::stallAfter(std::uint64_t const bytes) {
    std::scoped_lock const lock(m_mutex);
    m_stallAfter = bytes;
}

void LoopbackTransport::disconnectAfter(std::uint64_t const bytes) {
    std::scoped_lock const lock(m_mutex);
    m_disconnectAfter = bytes;
}

void LoopbackTransport::setSplitPattern(std::size_t const maxBytesPerRead) {
    std::scoped_lock const lock(m_mutex);
    m_splitMax = maxBytesPerRead;
}

void LoopbackTransport::setWriteBlocks(bool const blocks) noexcept {
    m_writeBlocks.store(blocks, std::memory_order_release);
}

std::uint64_t LoopbackTransport::deliveredBytes() const noexcept {
    return m_delivered.load(std::memory_order_relaxed);
}

std::size_t LoopbackTransport::pendingBytes() const {
    std::scoped_lock const lock(m_mutex);
    return m_toHost.size();
}

StatusOr<std::size_t> LoopbackTransport::read(std::span<std::byte> dst,
                                              std::chrono::milliseconds /*timeout*/) {
    std::scoped_lock const lock(m_mutex);
    if (!m_open) {
        return Status::error(ErrorCode::UsbDisconnected, "loopback transport is not open");
    }
        std::uint64_t const delivered = m_delivered.load(std::memory_order_relaxed);
    if (m_disconnectAfter.has_value() && delivered >= *m_disconnectAfter) {
        m_open.store(false, std::memory_order_release);
        m_link.open = false;
        return Status::error(ErrorCode::UsbDisconnected,
                             fmt::format("simulated disconnect after {} bytes", delivered));
    }
    if (m_stallAfter.has_value() && delivered >= *m_stallAfter) {
        // A stall is NOT an error: it is a timeout, which the caller must treat as
        // "no data yet". Conflating the two hides real backpressure.
        return std::size_t{0};
    }

    // Garbage injection happens before real data so resync is exercised.
    if (!m_pendingGarbage.empty()) {
        std::size_t const n = std::min(dst.size(), m_pendingGarbage.size());
        std::copy_n(m_pendingGarbage.begin(), static_cast<std::ptrdiff_t>(n), dst.begin());
        m_pendingGarbage.erase(
            m_pendingGarbage.begin(), m_pendingGarbage.begin() + static_cast<std::ptrdiff_t>(n));
        m_delivered.fetch_add(n, std::memory_order_relaxed);
        m_link.bytesRead += n;
        return n;
    }

    if (m_toHost.empty()) {
        return std::size_t{0};
    }

    std::size_t want = std::min(dst.size(), m_toHost.size());
    if (m_truncateTo.has_value()) {
        want = std::min(want, *m_truncateTo);
        m_truncateTo.reset();
    }
    if (m_splitMax > 0) {
        want = std::min(want, m_splitMax);
    }
    if (want == 0) {
        return std::size_t{0};
    }

    for (std::size_t i = 0; i < want; ++i) {
        std::byte b = m_toHost.front();
        m_toHost.pop_front();
        if (m_corruptRemaining > 0) {
            b = static_cast<std::byte>(std::to_integer<std::uint8_t>(b) ^ m_corruptMask);
            --m_corruptRemaining;
        }
        dst[i] = b;
    }
    m_delivered.fetch_add(want, std::memory_order_relaxed);
    m_link.bytesRead += want;
    return want;
}

Status LoopbackTransport::write(std::span<const std::byte> src) {
    std::scoped_lock const lock(m_mutex);
    if (!m_open) {
        return Status::error(ErrorCode::UsbDisconnected, "loopback transport is not open");
    }
    if (m_writeBlocks) {
        // Reported, not swallowed: a host that ignores writeWouldBlock() and keeps
        // writing is exactly how silent data loss happens.
        return Status::error(ErrorCode::UsbTimeout,
                             "simulated write backpressure: the link is not draining");
    }
    m_fromHost.insert(m_fromHost.end(), src.begin(), src.end());
    m_link.bytesWritten += src.size();
    return Status::success();
}

bool LoopbackTransport::writeWouldBlock() const noexcept {
    return m_writeBlocks.load(std::memory_order_acquire);
}

}  // namespace usn::transport
