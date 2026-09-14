// -----------------------------------------------------------------------------
// perf.h -- in-product performance instrumentation
// (docs/architecture_review.md section 15.2).
//
// Always compiled in, disabled by default at runtime. A disabled probe costs one
// relaxed atomic load and nothing else, so it can stay in the capture hot path.
//
// Percentiles come from a fixed 64-bucket log2 histogram, which is bounded
// memory and approximate by construction. They are labelled approximate wherever
// they are surfaced; a measurement report must state the method
// (docs/architecture_review.md section 15.4).
// -----------------------------------------------------------------------------
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace usn::perf {

// The fixed probe point list from section 15.2. Keeping them as named constants
// rather than free-form strings makes measurements comparable across versions.
namespace probes {
inline constexpr std::string_view kTransportRead{"transport.read"};
inline constexpr std::string_view kCodecParse{"codec.parse"};
inline constexpr std::string_view kCodecCrc{"codec.crc"};
inline constexpr std::string_view kPipelineDispatch{"pipeline.dispatch"};
inline constexpr std::string_view kStorageAppend{"storage.append"};
inline constexpr std::string_view kStorageFlush{"storage.flush"};
inline constexpr std::string_view kDecoderProcess{"decoder.process"};
inline constexpr std::string_view kLodBuild{"analysis.lod.build"};
inline constexpr std::string_view kAnalysisMeasure{"analysis.measure"};
inline constexpr std::string_view kSearchEvaluate{"search.evaluate"};
inline constexpr std::string_view kGuiSnapshotPublish{"gui.snapshot.publish"};
inline constexpr std::string_view kGuiFrame{"gui.frame"};
inline constexpr std::string_view kFileChunkRead{"file.chunk.read"};
inline constexpr std::string_view kFileIndexSeek{"file.index.seek"};
}  // namespace probes

struct ProbeStats {
    std::string name;
    std::uint64_t count{0};
    std::uint64_t totalNs{0};
    std::uint64_t minNs{0};
    std::uint64_t maxNs{0};
    std::uint64_t bytesTotal{0};

    // Approximate, from a bounded log2 histogram.
    std::uint64_t p50Ns{0};
    std::uint64_t p95Ns{0};
    std::uint64_t p99Ns{0};

    [[nodiscard]] double meanNs() const noexcept {
        return count == 0 ? 0.0 : static_cast<double>(totalNs) / static_cast<double>(count);
    }
    [[nodiscard]] double bytesPerSecond() const noexcept {
        return totalNs == 0 ? 0.0
                            : static_cast<double>(bytesTotal) * 1e9 / static_cast<double>(totalNs);
    }
};

class PerfRegistry {
public:
    static PerfRegistry& instance();

    PerfRegistry(const PerfRegistry&) = delete;
    PerfRegistry& operator=(const PerfRegistry&) = delete;

    void setEnabled(bool enabled) noexcept;
    [[nodiscard]] bool enabled() const noexcept {
        return m_enabled.load(std::memory_order_relaxed);
    }

    void record(std::string_view name, std::uint64_t elapsedNs, std::uint64_t bytes);

    [[nodiscard]] std::vector<ProbeStats> snapshot() const;
    [[nodiscard]] std::vector<ProbeStats> topByTotal(std::size_t limit) const;

    // Canonical JSON for diagnostics bundles and committed measurement records.
    [[nodiscard]] std::string toJson() const;

    void reset();

private:
    PerfRegistry() = default;

    struct Accumulator {
        std::uint64_t count{0};
        std::uint64_t totalNs{0};
        std::uint64_t minNs{0};
        std::uint64_t maxNs{0};
        std::uint64_t bytesTotal{0};
        std::array<std::uint64_t, 64> buckets{};  // log2(ns), bucket 0 == 0 ns
    };

    static std::size_t bucketFor(std::uint64_t ns) noexcept;
    static std::uint64_t bucketUpperBound(std::size_t bucket) noexcept;
    static void computePercentiles(const Accumulator& acc, ProbeStats& out) noexcept;

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, Accumulator> m_stats;
    std::atomic<bool> m_enabled{false};
};

// RAII scope probe. Set bytes before destruction for throughput accounting.
class Probe {
public:
    Probe(std::string_view name, std::uint64_t bytes = 0)
        : m_name(name), m_bytes(bytes) {
        if (PerfRegistry::instance().enabled()) {
            m_start = std::chrono::steady_clock::now();
            m_active = true;
        }
    }

    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;

    ~Probe() {
        if (m_active) {
            auto const end = std::chrono::steady_clock::now();
            auto const ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - m_start).count());
            PerfRegistry::instance().record(m_name, ns, m_bytes);
        }
    }

    void setBytes(std::uint64_t bytes) noexcept { m_bytes = bytes; }

private:
    std::string_view m_name;
    std::uint64_t m_bytes;
    std::chrono::steady_clock::time_point m_start{};
    bool m_active{false};
};

}  // namespace usn::perf
