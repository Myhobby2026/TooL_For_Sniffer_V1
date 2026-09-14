// -----------------------------------------------------------------------------
// file_transport.cpp -- see file_transport.h.
// -----------------------------------------------------------------------------
#include "usn/transport/file_transport.h"

#include <sys/stat.h>

#include <fmt/format.h>

namespace usn::transport {

struct FileTransport::Impl {
    std::FILE* file{nullptr};
    std::string path;
    LinkInfo link;
};

FileTransport::FileTransport() = default;

FileTransport::~FileTransport() { close(); }

Status FileTransport::open(const TransportConfig& config) {
    if (m_open.load(std::memory_order_acquire)) {
        return Status::error(ErrorCode::DeviceBusy, "file transport is already open");
    }
    if (config.endpoint.empty()) {
        return Status::error(ErrorCode::FileOpenFailed, "file transport needs a path");
    }
    m_impl = std::make_unique<Impl>();
    m_impl->path = config.endpoint;
    // "rb": binary, no newline translation. Matters on Windows, where a captured
    // byte stream containing 0x0A would otherwise be mangled.
    m_impl->file = std::fopen(m_impl->path.c_str(), "rb");
    if (m_impl->file == nullptr) {
        auto st = Status::error(ErrorCode::FileOpenFailed,
                                fmt::format("could not open '{}'", m_impl->path));
        m_impl.reset();
        return st;
    }
    struct stat info {};
    std::uint64_t size = 0;
    if (stat(m_impl->path.c_str(), &info) == 0) {
        size = static_cast<std::uint64_t>(info.st_size);
    }
    m_fileSize.store(size, std::memory_order_relaxed);
    m_position.store(0, std::memory_order_relaxed);
    m_impl->link = LinkInfo{};
    m_impl->link.name = m_impl->path;
    m_impl->link.kind = TransportKind::File;
    m_impl->link.open = true;
    m_open.store(true, std::memory_order_release);
    return Status::success();
}

void FileTransport::close() {
    m_open.store(false, std::memory_order_release);
    if (m_impl != nullptr) {
        if (m_impl->file != nullptr) {
            std::fclose(m_impl->file);
            m_impl->file = nullptr;
        }
        m_impl->link.open = false;
        m_impl.reset();
    }
}

bool FileTransport::isOpen() const noexcept { return m_open.load(std::memory_order_acquire); }

LinkInfo FileTransport::linkInfo() const {
    if (m_impl == nullptr) {
        LinkInfo empty;
        empty.kind = TransportKind::File;
        return empty;
    }
    LinkInfo info = m_impl->link;
    info.bytesRead = m_position.load(std::memory_order_relaxed);
    return info;
}

StatusOr<std::size_t> FileTransport::read(std::span<std::byte> dst,
                                          std::chrono::milliseconds /*timeout*/) {
    if (!m_open.load(std::memory_order_acquire) || m_impl == nullptr || m_impl->file == nullptr) {
        return Status::error(ErrorCode::FileReadFailed, "file transport is not open");
    }
    if (dst.empty()) {
        return std::size_t{0};
    }
    std::size_t const got = std::fread(dst.data(), 1, dst.size(), m_impl->file);
    if (got == 0) {
        if (std::feof(m_impl->file) != 0) {
            return std::size_t{0};  // EOF is not an error; atEof() reports it
        }
        return Status::error(ErrorCode::FileReadFailed,
                             fmt::format("read error on '{}'", m_impl->path));
    }
    m_position.fetch_add(got, std::memory_order_relaxed);
    return got;
}

Status FileTransport::write(std::span<const std::byte> /*src*/) {
    return Status::error(ErrorCode::FileWriteFailed,
                         "file transport is read-only; writing to a replay session is a bug");
}

bool FileTransport::writeWouldBlock() const noexcept { return true; }

bool FileTransport::atEof() const noexcept {
    return m_position.load(std::memory_order_relaxed) >= m_fileSize.load(std::memory_order_relaxed);
}

std::uint64_t FileTransport::fileSize() const noexcept {
    return m_fileSize.load(std::memory_order_relaxed);
}

std::uint64_t FileTransport::position() const noexcept {
    return m_position.load(std::memory_order_relaxed);
}

}  // namespace usn::transport
