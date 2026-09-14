// -----------------------------------------------------------------------------
// perf.cpp -- see perf.h.
// -----------------------------------------------------------------------------
#include "usn/common/perf.h"

#include <algorithm>
#include <sstream>

namespace usn::perf {

PerfRegistry& PerfRegistry::instance() {
    static PerfRegistry registry;
    return registry;
}

void PerfRegistry::setEnabled(bool const enabled) noexcept {
    m_enabled.store(enabled, std::memory_order_relaxed);
}

std::size_t PerfRegistry::bucketFor(std::uint64_t const ns) noexcept {
    if (ns == 0) {
        return 0;
    }
    std::size_t bucket = 0;
    std::uint64_t value = ns;
    while (value > 1 && bucket < 63) {
        value >>= 1;
        ++bucket;
    }
    return bucket;
}

std::uint64_t PerfRegistry::bucketUpperBound(std::size_t const bucket) noexcept {
    if (bucket == 0) {
        return 0;
    }
    if (bucket >= 63) {
        return UINT64_MAX;
    }
    return (std::uint64_t{1} << bucket) - 1;
}

void PerfRegistry::computePercentiles(const Accumulator& acc, ProbeStats& out) noexcept {
    if (acc.count == 0) {
        return;
    }
    struct Target {
        std::uint64_t rank;
        std::uint64_t* dest;
    };
    std::array<Target, 3> targets{
        Target{static_cast<std::uint64_t>(static_cast<double>(acc.count) * 0.50), &out.p50Ns},
        Target{static_cast<std::uint64_t>(static_cast<double>(acc.count) * 0.95), &out.p95Ns},
        Target{static_cast<std::uint64_t>(static_cast<double>(acc.count) * 0.99), &out.p99Ns}};

    std::size_t targetIdx = 0;
    std::uint64_t cumulative = 0;
    for (std::size_t bucket = 0; bucket < acc.buckets.size() && targetIdx < targets.size();
         ++bucket) {
        cumulative += acc.buckets[bucket];
        while (targetIdx < targets.size() && cumulative > targets[targetIdx].rank) {
            *targets[targetIdx].dest = bucketUpperBound(bucket);
            ++targetIdx;
        }
    }
    // Any percentile not resolved (rounding at the tail) is the observed maximum.
    while (targetIdx < targets.size()) {
        *targets[targetIdx].dest = acc.maxNs;
        ++targetIdx;
    }
}

void PerfRegistry::record(std::string_view const name, std::uint64_t const elapsedNs,
                          std::uint64_t const bytes) {
    std::scoped_lock const lock(m_mutex);
    auto& acc = m_stats[std::string(name)];
    if (acc.count == 0) {
        acc.minNs = elapsedNs;
        acc.maxNs = elapsedNs;
    } else {
        acc.minNs = std::min(acc.minNs, elapsedNs);
        acc.maxNs = std::max(acc.maxNs, elapsedNs);
    }
    acc.count += 1;
    acc.totalNs += elapsedNs;
    acc.bytesTotal += bytes;
    acc.buckets[bucketFor(elapsedNs)] += 1;
}

std::vector<ProbeStats> PerfRegistry::snapshot() const {
    std::scoped_lock const lock(m_mutex);
    std::vector<ProbeStats> out;
    out.reserve(m_stats.size());
    for (const auto& [name, acc] : m_stats) {
        ProbeStats stats;
        stats.name = name;
        stats.count = acc.count;
        stats.totalNs = acc.totalNs;
        stats.minNs = acc.minNs;
        stats.maxNs = acc.maxNs;
        stats.bytesTotal = acc.bytesTotal;
        computePercentiles(acc, stats);
        out.push_back(std::move(stats));
    }
    // Sorted by name so output is deterministic across runs and platforms.
    std::sort(out.begin(), out.end(),
              [](const ProbeStats& a, const ProbeStats& b) { return a.name < b.name; });
    return out;
}

std::vector<ProbeStats> PerfRegistry::topByTotal(std::size_t const limit) const {
    auto all = snapshot();
    std::sort(all.begin(), all.end(), [](const ProbeStats& a, const ProbeStats& b) {
        return a.totalNs > b.totalNs;
    });
    if (all.size() > limit) {
        all.resize(limit);
    }
    return all;
}

std::string PerfRegistry::toJson() const {
    std::ostringstream os;
    os << "{\n  \"probes\": [\n";
    auto const all = snapshot();
    for (std::size_t i = 0; i < all.size(); ++i) {
        const auto& s = all[i];
        os << "    {\"name\":\"" << s.name << "\",\"count\":" << s.count << ",\"totalNs\":"
           << s.totalNs << ",\"minNs\":" << s.minNs << ",\"maxNs\":" << s.maxNs << ",\"p50Ns\":"
           << s.p50Ns << ",\"p95Ns\":" << s.p95Ns << ",\"p99Ns\":" << s.p99Ns << ",\"bytesTotal\":"
           << s.bytesTotal << ",\"meanNs\":" << s.meanNs() << ",\"bytesPerSecond\":"
           << s.bytesPerSecond() << '}';
        if (i + 1 < all.size()) {
            os << ',';
        }
        os << '\n';
    }
    os << "  ]\n}\n";
    return os.str();
}

void PerfRegistry::reset() {
    std::scoped_lock const lock(m_mutex);
    m_stats.clear();
}

}  // namespace usn::perf
