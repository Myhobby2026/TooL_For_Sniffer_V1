// -----------------------------------------------------------------------------
// istorage.h -- the write/read abstraction for .usn captures.
//
// Phase 1 delivers the interface and a validating in-memory implementation only.
// The real chunked .usn writer/reader (sections 10-11 of the master spec, with TOC,
// trailer, per-chunk CRC32C and crash recovery) is Phase 4; this interface is
// written now so that CaptureSession depends on the abstraction and not on a file
// layout that is still going to move.
//
// Invariants the interface is built around:
//   * SampleIndex is the only key. No floating-point timestamps are stored.
//   * Appends are ordered and monotonic. A non-monotonic append is an error, not a
//     silent overwrite.
//   * Nothing is lost without a DiagnosticEvent.
//   * flush()/close() report success or failure; the caller never has to guess
//     whether the capture actually made it to durable storage.
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "usn/common/status.h"
#include "usn/model/sample.h"

namespace usn::core {

// Where a capture is meant to end up. Kept explicit rather than inferred, so a
// test never accidentally writes to a real file.
enum class StorageTarget : std::uint8_t {
    InMemory = 0,   // phase 1: validated, deterministic, no filesystem
    UsnFile = 1     // phase 4: chunked .usn container
};

[[nodiscard]] std::string_view nameOf(StorageTarget target) noexcept;

struct StorageStatistics {
    std::uint64_t blocksWritten{0};
    std::uint64_t samplesWritten{0};
    std::uint64_t bytesWritten{0};
    std::uint64_t blocksRejected{0};
    std::uint64_t flushes{0};
    std::uint64_t bytesDurable{0};      // bytes acknowledged by flush()/close()
    std::uint64_t holesRecorded{0};     // discontinuities stored as holes
    std::uint64_t missingSamples{0};    // total samples inside those holes
};

// A discontinuity in the stored sample indices. Losing packets mid-capture must not
// end the capture: the hole is recorded, the data on both sides of it is kept, and
// every view can render the discontinuity. Shifting later samples down to close the
// hole would make every SampleIndex after it lie about its position.
struct StorageGap {
    SampleIndex firstMissing{};
    SampleIndex afterGap{};
    [[nodiscard]] std::uint64_t missingSamples() const noexcept {
        return afterGap.value > firstMissing.value ? afterGap.value - firstMissing.value : 0;
    }
};

struct StorageRange {
    SampleIndex first{0};
    SampleIndex endExclusive{0};        // exclusive
    [[nodiscard]] std::uint64_t sampleCount() const noexcept {
        return endExclusive.value > first.value ? endExclusive.value - first.value : 0;
    }
    [[nodiscard]] bool contains(SampleIndex const index) const noexcept {
        return index.value >= first.value && index.value < endExclusive.value;
    }
};

// Minimal metadata every storage backend must be able to report. Deliberately
// small: the full .usn META section is Phase 4 work.
struct StorageMetadata {
    std::string formatName;
    std::uint32_t formatVersion{0};
    std::uint8_t strideBytes{0};
    std::uint8_t channelCount{0};
    std::uint64_t channelMask{0};
    std::uint64_t sampleRateHz{0};
    std::int64_t createdUnixNs{0};
    std::string deviceName;
    std::string firmwareVersion;
};

class IStorage {
public:
    IStorage() = default;
    IStorage(const IStorage&) = delete;
    IStorage& operator=(const IStorage&) = delete;
    IStorage(IStorage&&) noexcept = default;
    IStorage& operator=(IStorage&&) noexcept = default;
    virtual ~IStorage() = default;

    [[nodiscard]] virtual Status open(const StorageMetadata& metadata) = 0;
    [[nodiscard]] virtual Status append(const SampleBlockView& block) = 0;
    [[nodiscard]] virtual Status flush() = 0;
    [[nodiscard]] virtual Status close() = 0;

    [[nodiscard]] virtual bool isOpen() const noexcept = 0;
    [[nodiscard]] virtual StorageTarget target() const noexcept = 0;
    [[nodiscard]] virtual StorageStatistics statistics() const noexcept = 0;
    // Overall extent, from the first stored sample to the last. May contain holes.
    [[nodiscard]] virtual StorageRange coveredRange() const noexcept = 0;
    // The contiguous runs inside that extent. Empty for a capture with no data.
    [[nodiscard]] virtual std::vector<StorageRange> coveredRanges() const noexcept = 0;
    [[nodiscard]] virtual std::vector<StorageGap> holes() const noexcept = 0;
    [[nodiscard]] virtual const StorageMetadata& metadata() const noexcept = 0;

    // Read path. Returns the requested samples in device-native packed form; the
    // caller supplies the buffer so no allocation happens on the read path.
    [[nodiscard]] virtual StatusOr<std::uint64_t> read(
        SampleIndex first, std::uint64_t count, std::span<std::byte> out) const = 0;

    [[nodiscard]] static std::unique_ptr<IStorage> create(StorageTarget target);
};

// Validating in-memory implementation. Exists so Phase 1 can exercise the whole
// capture path end to end under TSan without touching the filesystem, and so the
// append-ordering rules are enforced identically for every backend.
class InMemoryStorage final : public IStorage {
public:
    InMemoryStorage() = default;
    ~InMemoryStorage() final;

    [[nodiscard]] Status open(const StorageMetadata& metadata) final;
    [[nodiscard]] Status append(const SampleBlockView& block) final;
    [[nodiscard]] Status flush() final;
    [[nodiscard]] Status close() final;

    [[nodiscard]] bool isOpen() const noexcept final { return m_open; }
    [[nodiscard]] StorageTarget target() const noexcept final { return StorageTarget::InMemory; }
    [[nodiscard]] StorageStatistics statistics() const noexcept final { return m_stats; }
    [[nodiscard]] StorageRange coveredRange() const noexcept final { return m_range; }
    [[nodiscard]] std::vector<StorageRange> coveredRanges() const noexcept final;
    [[nodiscard]] std::vector<StorageGap> holes() const noexcept final;
    [[nodiscard]] const StorageMetadata& metadata() const noexcept final { return m_metadata; }
    [[nodiscard]] StatusOr<std::uint64_t> read(
        SampleIndex first, std::uint64_t count, std::span<std::byte> out) const final;

    // Test/inspection helpers, not part of IStorage.
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return m_bytes; }

private:
    // One contiguous run of stored samples plus where its bytes live in m_bytes.
    struct Run {
        StorageRange range{};
        std::size_t byteOffset{0};
    };
    [[nodiscard]] const Run* findRun(SampleIndex first, std::uint64_t count) const noexcept;

    bool m_open{false};
    StorageMetadata m_metadata{};
    StorageStatistics m_stats{};
    StorageRange m_range{};
    std::vector<Run> m_runs;
    std::vector<StorageGap> m_gaps;
    std::vector<std::byte> m_bytes;
};

}  // namespace usn::core
