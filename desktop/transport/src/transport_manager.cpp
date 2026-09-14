// -----------------------------------------------------------------------------
// transport_manager.cpp -- transport creation and reconnect policy.
// -----------------------------------------------------------------------------
#include "usn/transport/itransport.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <mutex>
#include <random>
#include <unordered_map>

#include <fmt/format.h>

#include "usn/common/log.h"
#include "usn/transport/file_transport.h"
#include "usn/transport/loopback_transport.h"

namespace usn::transport {

struct TransportManager::Impl {
    std::mutex mutex;
    std::unordered_map<TransportKind, Factory> factories;
    ReconnectPolicy policy;
};

TransportManager::TransportManager() : m_impl(std::make_unique<Impl>()) {
    // Built-in kinds. UsbCdc is registered in Phase 3; until then a request for
    // it fails with a precise message rather than returning a wrong transport.
    registerFactory(TransportKind::Loopback, [](const TransportConfig&) {
        return std::make_unique<LoopbackTransport>();
    });
    registerFactory(TransportKind::File, [](const TransportConfig&) {
        return std::make_unique<FileTransport>();
    });
}

TransportManager::~TransportManager() = default;

void TransportManager::registerFactory(TransportKind const kind, Factory factory) {
    std::scoped_lock const lock(m_impl->mutex);
    m_impl->factories[kind] = std::move(factory);
}

StatusOr<std::unique_ptr<ITransport>> TransportManager::create(const TransportConfig& config) {
    Factory factory;
    {
        std::scoped_lock const lock(m_impl->mutex);
        auto const it = m_impl->factories.find(config.kind);
        if (it == m_impl->factories.end()) {
            auto st = Status::error(
                ErrorCode::NotImplemented,
                fmt::format("no transport factory registered for kind '{}'", nameOf(config.kind)));
            if (config.kind == TransportKind::UsbCdc) {
                st = std::move(st).withContext("phase", "3 -- USB transport is not implemented yet");
            }
            USN_LOG_WARN(log::cats::kUsb, "{}", st.toString());
            return st;
        }
        factory = it->second;
    }
    auto transport = factory(config);
    if (transport == nullptr) {
        return Status::error(ErrorCode::Unknown,
                             fmt::format("factory for '{}' returned null", nameOf(config.kind)));
    }
    return transport;
}

void TransportManager::setReconnectPolicy(ReconnectPolicy const policy) noexcept {
    std::scoped_lock const lock(m_impl->mutex);
    m_impl->policy = policy;
}

const ReconnectPolicy& TransportManager::reconnectPolicy() const noexcept {
    // Safe without a lock: the policy is written before capture starts and read
    // during reconnect handling. Documented as "configure then run", and the
    // reconnect path copies it once rather than reading fields repeatedly.
    return m_impl->policy;
}

std::chrono::milliseconds TransportManager::backoffForAttempt(
    std::uint32_t const attempt) const noexcept {
    const ReconnectPolicy policy = reconnectPolicy();
    if (policy.maxAttempts != 0 && attempt >= policy.maxAttempts) {
        return std::chrono::milliseconds(0);  // give up
    }
    double backoff = static_cast<double>(policy.initialBackoff.count());
    for (std::uint32_t i = 0; i < attempt; ++i) {
        backoff *= policy.backoffMultiplier;
        if (backoff >= static_cast<double>(policy.maxBackoff.count())) {
            backoff = static_cast<double>(policy.maxBackoff.count());
            break;
        }
    }
    auto result = std::chrono::milliseconds(static_cast<long long>(backoff));
    if (result > policy.maxBackoff) {
        result = policy.maxBackoff;
    }
    if (policy.jitter && result.count() > 0) {
        // Deterministic jitter: derived from the attempt number, so reconnect
        // timing is reproducible in tests instead of randomly flaky.
        std::uint32_t const hash = attempt * 2654435761U;
        double const fraction = static_cast<double>(hash % 1000U) / 1000.0;  // 0..1
        auto const span = static_cast<double>(result.count()) * 0.25;
        result = std::chrono::milliseconds(
            static_cast<long long>(static_cast<double>(result.count()) - span + fraction * 2.0 * span));
        if (result.count() < 0) {
            result = std::chrono::milliseconds(0);
        }
    }
    return result;
}

}  // namespace usn::transport
