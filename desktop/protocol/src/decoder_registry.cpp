// -----------------------------------------------------------------------------
// decoder_registry.cpp -- see decoder_registry.h.
// -----------------------------------------------------------------------------
#include "usn/protocol/decoder_registry.h"

#include <utility>

#include <fmt/format.h>

#include "usn/common/log.h"

namespace usn::protocol {

Status DecoderRegistry::verifyFactory(const Factory& factory, std::uint32_t const expectedId,
                                      std::string_view const expectedName) {
    if (!factory) {
        return Status::error(ErrorCode::DecoderNotFound, "decoder factory is empty");
    }
    auto instance = factory();
    if (instance == nullptr) {
        return Status::error(ErrorCode::DecoderNotFound,
                             fmt::format("decoder factory for '{}' returned null", expectedName));
    }
    auto const info = instance->info();
    if (info.apiVersion != kDecoderApiVersion) {
        // Refused at registration, not at first use mid-capture. A plugin built
        // against a different ABI would otherwise produce undefined behaviour.
        return Status::error(ErrorCode::PluginApiMismatch,
                             fmt::format("decoder '{}' was built against API version {}, this "
                                         "build expects {}",
                                         info.name, info.apiVersion, kDecoderApiVersion));
    }
    if (info.id != expectedId) {
        return Status::error(ErrorCode::DecoderConfigurationInvalid,
                             fmt::format("decoder '{}' reports id {} but was registered as {}",
                                         info.name, info.id, expectedId));
    }
    if (info.name != expectedName) {
        return Status::error(ErrorCode::DecoderConfigurationInvalid,
                             fmt::format("decoder reports name '{}' but was registered as '{}'",
                                         info.name, expectedName));
    }
    if (info.name.empty()) {
        return Status::error(ErrorCode::DecoderConfigurationInvalid, "decoder name is empty");
    }
    return Status::success();
}

Status DecoderRegistry::registerDecoder(std::uint32_t const id, std::string name,
                                        Factory factory) {
    auto const verified = verifyFactory(factory, id, name);
    if (!verified.ok()) {
        USN_LOG_ERROR(log::cats::kDecoder, "refusing to register decoder {}: {}", name,
                      verified.toString());
        return verified;
    }
    auto probe = factory();
    DecoderInfo info = probe->info();

    std::scoped_lock const lock(m_mutex);
    if (m_entries.contains(id)) {
        return Status::error(ErrorCode::DecoderAlreadyRegistered,
                             fmt::format("decoder id {} is already registered as '{}'", id,
                                         m_entries[id].name));
    }
    for (const auto& [otherId, entry] : m_entries) {
        if (entry.name == name) {
            return Status::error(ErrorCode::DecoderAlreadyRegistered,
                                 fmt::format("decoder name '{}' is already registered with id {}",
                                             name, otherId));
        }
    }
    Entry entry;
    entry.id = id;
    entry.name = std::move(name);
    entry.info = std::move(info);
    entry.factory = std::move(factory);
    USN_LOG_INFO(log::cats::kDecoder, "registered decoder '{}' (id {}, version {})", entry.name,
                 entry.id, entry.info.version);
    m_entries[id] = std::move(entry);
    return Status::success();
}

void DecoderRegistry::unregister(std::uint32_t const id) {
    std::scoped_lock const lock(m_mutex);
    m_entries.erase(id);
}

StatusOr<std::unique_ptr<IProtocolDecoder>> DecoderRegistry::create(std::uint32_t const id) const {
    Factory factory;
    std::string name;
    {
        std::scoped_lock const lock(m_mutex);
        auto const it = m_entries.find(id);
        if (it == m_entries.end()) {
            return Status::error(ErrorCode::DecoderNotFound,
                                 fmt::format("no decoder registered with id {}", id));
        }
        factory = it->second.factory;
        name = it->second.name;
    }
    auto instance = factory();
    if (instance == nullptr) {
        return Status::error(ErrorCode::DecoderInternalError,
                             fmt::format("factory for decoder '{}' returned null", name));
    }
    return instance;
}

StatusOr<std::unique_ptr<IProtocolDecoder>> DecoderRegistry::createByName(
    std::string_view const name) const {
    std::uint32_t id = 0;
    {
        std::scoped_lock const lock(m_mutex);
        for (const auto& [entryId, entry] : m_entries) {
            if (entry.name == name) {
                id = entryId;
                break;
            }
        }
    }
    if (id == 0) {
        return Status::error(ErrorCode::DecoderNotFound,
                             fmt::format("no decoder registered with name '{}'", name));
    }
    return create(id);
}

std::vector<DecoderInfo> DecoderRegistry::available() const {
    std::scoped_lock const lock(m_mutex);
    std::vector<DecoderInfo> out;
    out.reserve(m_entries.size());
    for (const auto& [id, entry] : m_entries) {
        out.push_back(entry.info);
    }
    std::sort(out.begin(), out.end(),
              [](const DecoderInfo& a, const DecoderInfo& b) { return a.id < b.id; });
    return out;
}

bool DecoderRegistry::contains(std::uint32_t const id) const {
    std::scoped_lock const lock(m_mutex);
    return m_entries.contains(id);
}

std::size_t DecoderRegistry::size() const {
    std::scoped_lock const lock(m_mutex);
    return m_entries.size();
}

}  // namespace usn::protocol
