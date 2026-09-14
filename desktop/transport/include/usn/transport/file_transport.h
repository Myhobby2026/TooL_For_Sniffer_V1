// -----------------------------------------------------------------------------
// file_transport.h -- read-only transport over a recorded byte stream.
//
// Makes replay and offline analysis use exactly the same code path as a live
// capture: CapturePipeline cannot tell the difference, so a bug found in replay is
// a real bug. Also the mechanism behind tools/wire_dumper.
// -----------------------------------------------------------------------------
#pragma once

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>

#include "usn/transport/itransport.h"

namespace usn::transport {

class FileTransport final : public ITransport {
public:
    // Both declared here and defined in the .cpp: with a pimpl unique_ptr<Impl>,
    // an inline "= default" constructor makes the compiler emit member-cleanup code
    // in every translation unit that constructs a FileTransport, which needs Impl
    // to be complete. Out-of-line definitions keep Impl private to the .cpp.
    FileTransport();
    ~FileTransport() final;

    Status open(const TransportConfig& config) final;
    void close() final;
    [[nodiscard]] bool isOpen() const noexcept final;
    [[nodiscard]] LinkInfo linkInfo() const final;

    [[nodiscard]] StatusOr<std::size_t> read(std::span<std::byte> dst,
                                             std::chrono::milliseconds timeout) final;

    // A recorded stream is read-only. Writing returns an error rather than
    // silently discarding, because a caller that writes to a replay session has a
    // bug and must find out.
    [[nodiscard]] Status write(std::span<const std::byte> src) final;
    [[nodiscard]] bool writeWouldBlock() const noexcept final;

    [[nodiscard]] bool atEof() const noexcept;
    [[nodiscard]] std::uint64_t fileSize() const noexcept;
    [[nodiscard]] std::uint64_t position() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::atomic<bool> m_open{false};
    std::atomic<std::uint64_t> m_position{0};
    std::atomic<std::uint64_t> m_fileSize{0};
};

}  // namespace usn::transport
