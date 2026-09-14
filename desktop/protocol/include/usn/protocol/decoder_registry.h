// -----------------------------------------------------------------------------
// decoder_registry.h -- decoder discovery and instantiation.
//
// Registration is runtime rather than a static-initialiser table, so plugin
// decoders (master spec section 39) register through exactly the same path as
// first-party ones and there is no static-initialisation-order hazard.
// -----------------------------------------------------------------------------
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "usn/protocol/decoder_api.h"

namespace usn::protocol {

class DecoderRegistry {
public:
    using Factory = std::function<std::unique_ptr<IProtocolDecoder>()>;

    DecoderRegistry() = default;
    DecoderRegistry(const DecoderRegistry&) = delete;
    DecoderRegistry& operator=(const DecoderRegistry&) = delete;

    [[nodiscard]] Status registerDecoder(std::uint32_t id, std::string name, Factory factory);
    void unregister(std::uint32_t id);

    [[nodiscard]] StatusOr<std::unique_ptr<IProtocolDecoder>> create(std::uint32_t id) const;
    [[nodiscard]] StatusOr<std::unique_ptr<IProtocolDecoder>> createByName(
        std::string_view name) const;

    [[nodiscard]] std::vector<DecoderInfo> available() const;
    [[nodiscard]] bool contains(std::uint32_t id) const;
    [[nodiscard]] std::size_t size() const;

    // Verifies that a newly registered factory actually produces a usable decoder
    // with a consistent apiVersion. Called by registerDecoder, so a broken plugin
    // is rejected at registration instead of at first use mid-capture.
    [[nodiscard]] static Status verifyFactory(const Factory& factory, std::uint32_t expectedId,
                                              std::string_view expectedName);

private:
    mutable std::mutex m_mutex;
    struct Entry {
        std::uint32_t id{0};
        std::string name;
        DecoderInfo info;
        Factory factory;
    };
    std::unordered_map<std::uint32_t, Entry> m_entries;
};

}  // namespace usn::protocol
