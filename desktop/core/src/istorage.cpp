// -----------------------------------------------------------------------------
// istorage.cpp -- validating in-memory storage backend (Phase 1).
// -----------------------------------------------------------------------------
#include "usn/core/istorage.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include <fmt/format.h>

#include "usn/common/log.h"

namespace usn::core {

std::string_view nameOf(StorageTarget const target) noexcept {
    switch (target) {
    case StorageTarget::InMemory: return "in-memory";
    case StorageTarget::UsnFile:  return "usn-file";
    }
    return "unknown";
}

std::unique_ptr<IStorage> IStorage::create(StorageTarget const target) {
    switch (target) {
    case StorageTarget::InMemory:
        return std::make_unique<InMemoryStorage>();
    case StorageTarget::UsnFile:
        // Phase 4 delivers the chunked .usn container. Returning a null pointer
        // with a real error is honest; returning an in-memory fallback would let a
        // user believe their capture is durable when it is not.
        return nullptr;
    }
    return nullptr;
}

InMemoryStorage::~InMemoryStorage() {
    if (m_open) {
        USN_LOG_WARN(log::cats::kStorage,
                     "InMemoryStorage destroyed while open ({} blocks); closing",
                     m_stats.blocksWritten);
        (void)close();
    }
}

Status InMemoryStorage::open(const StorageMetadata& metadata) {
    if (m_open) {
        return Status::error(ErrorCode::FileOpenFailed, "storage is already open");
    }
    if (metadata.strideBytes != 1 && metadata.strideBytes != 2 && metadata.strideBytes != 4) {
        return Status::error(ErrorCode::StrideUnsupported,
                             fmt::format("stride {} is not 1, 2 or 4 bytes", metadata.strideBytes));
    }
    if (metadata.channelCount == 0 || metadata.channelCount > 32) {
        return Status::error(ErrorCode::ChannelCountUnsupported,
                             fmt::format("channel count {} is outside the supported range 1..32",
                                         metadata.channelCount));
    }
    if (metadata.sampleRateHz == 0) {
        return Status::error(ErrorCode::SampleRateUnsupported, "sample rate must not be zero");
    }
    if (metadata.formatName.empty()) {
        return Status::error(ErrorCode::ConfigurationError, "storage format name is empty");
    }
    // A channel mask that does not agree with the channel count would produce a
    // file that cannot be re-opened with the right channel mapping.
    auto const popcount = std::popcount(metadata.channelMask);
    if (popcount != static_cast<int>(metadata.channelCount)) {
        return Status::error(
            ErrorCode::ConfigurationError,
            fmt::format("channel mask has {} bits set but channelCount is {}", popcount,
                        metadata.channelCount));
    }

    m_metadata = metadata;
    m_bytes.clear();
    m_runs.clear();
    m_gaps.clear();
    m_stats = StorageStatistics{};
    m_range = StorageRange{};
    m_open = true;
    USN_LOG_INFO(log::cats::kStorage, "in-memory storage opened: {} ch @ {} Hz, stride {} B",
                 metadata.channelCount, metadata.sampleRateHz, metadata.strideBytes);
    return Status::success();
}

Status InMemoryStorage::append(const SampleBlockView& block) {
    if (!m_open) {
        return Status::error(ErrorCode::FileWriteFailed, "storage is not open");
    }
    auto const validity = block.validate();
    if (!validity.ok()) {
        m_stats.blocksRejected += 1;
        return validity;
    }
    const auto& h = block.header();
    if (h.strideBytes != m_metadata.strideBytes || h.channelCount != m_metadata.channelCount ||
        h.sampleRateHz != m_metadata.sampleRateHz) {
        // Changing geometry mid-capture would make every stored SampleIndex
        // meaningless. Reject loudly.
        m_stats.blocksRejected += 1;
        return Status::error(ErrorCode::ConfigurationError,
                             fmt::format("block geometry ({}/{}/{} Hz) does not match the storage "
                                         "geometry ({}/{}/{} Hz)",
                                         h.channelCount, h.strideBytes, h.sampleRateHz,
                                         m_metadata.channelCount, m_metadata.strideBytes,
                                         m_metadata.sampleRateHz));
    }
    if (block.payload().size() != h.payloadBytes()) {
        m_stats.blocksRejected += 1;
        return Status::error(ErrorCode::InvalidPacketLength,
                             fmt::format("payload is {} bytes but the header claims {}",
                                         block.payload().size(), h.payloadBytes()));
    }

    auto const stride = static_cast<std::uint64_t>(m_metadata.strideBytes);
    auto const first = h.firstSampleIndex;
    auto const endExclusive = h.lastSampleIndexExclusive();

    if (!m_runs.empty()) {
        auto const storedEnd = m_runs.back().range.endExclusive.value;
        if (first.value < storedEnd) {
            // Overlap or rewind would corrupt data that is already stored, and every
            // index after it. Unlike a hole, this cannot be represented faithfully.
            m_stats.blocksRejected += 1;
            return Status::error(
                ErrorCode::SampleIndexGap,
                fmt::format("block at sample {} overlaps or precedes already-stored data ending "
                            "at {}",
                            first.value, storedEnd));
        }
        if (first.value > storedEnd) {
            // A hole. Record it and keep going: losing packets mid-capture must not
            // end the capture, and the samples on both sides are still worth having.
            StorageGap gap;
            gap.firstMissing = SampleIndex(storedEnd);
            gap.afterGap = first;
            m_gaps.push_back(gap);
            m_stats.holesRecorded += 1;
            m_stats.missingSamples += gap.missingSamples();
            USN_LOG_WARN(log::cats::kStorage,
                         "storage hole: {} sample(s) missing between {} and {}",
                         gap.missingSamples(), storedEnd, first.value);
            // The hole starts a new contiguous run.
            Run run;
            run.range.first = first;
            run.range.endExclusive = endExclusive;
            run.byteOffset = m_bytes.size();
            m_runs.push_back(run);
        } else {
            m_runs.back().range.endExclusive = endExclusive;
        }
    } else {
        Run run;
        run.range.first = first;
        run.range.endExclusive = endExclusive;
        run.byteOffset = m_bytes.size();
        m_runs.push_back(run);
    }

    m_bytes.insert(m_bytes.end(), block.payload().begin(), block.payload().end());
    if (m_range.endExclusive.value == 0 && m_range.first.value == 0) {
        m_range.first = first;
    }
    m_range.endExclusive = endExclusive;
    m_stats.blocksWritten += 1;
    m_stats.samplesWritten += h.sampleCount;
    m_stats.bytesWritten += h.payloadBytes();
    (void)stride;
    return Status::success();
}

std::vector<StorageRange> InMemoryStorage::coveredRanges() const noexcept {
    std::vector<StorageRange> out;
    out.reserve(m_runs.size());
    for (const auto& run : m_runs) {
        out.push_back(run.range);
    }
    return out;
}

std::vector<StorageGap> InMemoryStorage::holes() const noexcept { return m_gaps; }

const InMemoryStorage::Run* InMemoryStorage::findRun(SampleIndex const first,
                                                     std::uint64_t const count) const noexcept {
    for (const auto& run : m_runs) {
        if (first.value >= run.range.first.value &&
            first.value + count <= run.range.endExclusive.value) {
            return &run;
        }
    }
    return nullptr;
}

Status InMemoryStorage::flush() {
    if (!m_open) {
        return Status::error(ErrorCode::FileWriteFailed, "storage is not open");
    }
    // Nothing to push for an in-memory backend, but the accounting must still
    // reflect "durable", otherwise the session would report bytes that were never
    // acknowledged.
    m_stats.flushes += 1;
    m_stats.bytesDurable = m_stats.bytesWritten;
    return Status::success();
}

Status InMemoryStorage::close() {
    if (!m_open) {
        return Status::success();
    }
    auto const flushResult = flush();
    m_open = false;
    USN_LOG_INFO(log::cats::kStorage,
                 "in-memory storage closed: {} blocks, {} samples, {} bytes, {} hole(s)",
                 m_stats.blocksWritten, m_stats.samplesWritten, m_stats.bytesWritten,
                 m_stats.holesRecorded);
    return flushResult;
}

StatusOr<std::uint64_t> InMemoryStorage::read(SampleIndex const first, std::uint64_t const count,
                                              std::span<std::byte> out) const {
    if (m_runs.empty()) {
        return Status::error(ErrorCode::FileReadFailed, "storage holds no data");
    }
    if (count == 0) {
        return std::uint64_t{0};
    }
    auto const stride = static_cast<std::uint64_t>(m_metadata.strideBytes);
    auto const needed = count * stride;
    if (out.size() < needed) {
        return Status::error(ErrorCode::InvalidPacketLength,
                             fmt::format("output buffer holds {} bytes but {} are needed",
                                         out.size(), needed));
    }
    const Run* run = findRun(first, count);
    if (run == nullptr) {
        // Distinguish "outside the capture" from "spans a hole": the second is
        // recoverable by asking for less, and the caller needs to know which it is.
        bool insideExtent = first.value >= m_range.first.value &&
                            first.value + count <= m_range.endExclusive.value;
        if (insideExtent) {
            return Status::error(
                ErrorCode::FileReadFailed,
                fmt::format("read of {} sample(s) at {} spans a hole in the stored data; {} "
                            "hole(s) are recorded",
                            count, first.value, m_gaps.size()));
        }
        return Status::error(ErrorCode::FileReadFailed,
                             fmt::format("read of {} sample(s) at {} is outside the stored range "
                                         "[{}..{})",
                                         count, first.value, m_range.first.value,
                                         m_range.endExclusive.value));
    }
    auto const offset = run->byteOffset + (first.value - run->range.first.value) * stride;
    if (offset + needed > m_bytes.size()) {
        return Status::error(ErrorCode::FileCorrupt,
                             fmt::format("stored payload is {} bytes but offset {} + {} exceeds it",
                                         m_bytes.size(), offset, needed));
    }
    std::memcpy(out.data(), m_bytes.data() + offset, needed);
    return count;
}

}  // namespace usn::core
