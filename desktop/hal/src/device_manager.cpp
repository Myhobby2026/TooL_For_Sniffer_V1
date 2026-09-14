// -----------------------------------------------------------------------------
// device_manager.cpp -- see device_manager.h.
// -----------------------------------------------------------------------------
#include "usn/hal/device_manager.h"

#include <mutex>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>

#include "usn/common/log.h"

namespace usn::hal {

StatusOr<std::vector<transport::DiscoveredDevice>> NullProbe::scan() {
    return std::vector<transport::DiscoveredDevice>{};
}

void ManualProbe::addEndpoint(std::string endpoint, std::string description) {
    transport::DiscoveredDevice device;
    device.endpoint = endpoint;
    device.name = endpoint;
    device.description = std::move(description);
    device.kind = transport::TransportKind::UsbCdc;
    m_endpoints.push_back(std::move(device));
}

void ManualProbe::clear() { m_endpoints.clear(); }

StatusOr<std::vector<transport::DiscoveredDevice>> ManualProbe::scan() { return m_endpoints; }

struct DeviceManager::Impl {
    mutable std::mutex mutex;
    std::vector<std::unique_ptr<IDeviceProbe>> probes;
    std::vector<transport::DiscoveredDevice> discovered;
    std::vector<Status> probeErrors;
    std::unordered_map<std::string, std::unique_ptr<ICaptureDevice>> owned;
    std::unordered_map<std::string, bool> inUse;
    DeviceFactory factory;
};

DeviceManager::DeviceManager() : m_impl(std::make_unique<Impl>()) {}
DeviceManager::~DeviceManager() = default;

void DeviceManager::addProbe(std::unique_ptr<IDeviceProbe> probe) {
    if (probe == nullptr) {
        return;
    }
    std::scoped_lock const lock(m_impl->mutex);
    m_impl->probes.push_back(std::move(probe));
}

void DeviceManager::setDeviceFactory(DeviceFactory factory) {
    std::scoped_lock const lock(m_impl->mutex);
    m_impl->factory = std::move(factory);
}

Status DeviceManager::rescan() {
    std::vector<std::unique_ptr<IDeviceProbe>>* probes = nullptr;
    {
        std::scoped_lock const lock(m_impl->mutex);
        m_impl->discovered.clear();
        m_impl->probeErrors.clear();
        probes = &m_impl->probes;
    }
    std::vector<transport::DiscoveredDevice> all;
    std::vector<Status> errors;
    for (const auto& probe : *probes) {
        auto result = probe->scan();
        if (!result.ok()) {
            errors.push_back(std::move(result).takeStatus().withContext("probe", probe->name()));
            USN_LOG_WARN(log::cats::kDevice, "probe '{}' failed: {}", probe->name(),
                         errors.back().toString());
            continue;
        }
        for (auto& device : *result) {
            all.push_back(std::move(device));
        }
    }
    std::scoped_lock const lock(m_impl->mutex);
    m_impl->discovered = std::move(all);
    m_impl->probeErrors = std::move(errors);
    USN_LOG_INFO(log::cats::kDevice, "device scan found {} device(s), {} probe error(s)",
                 m_impl->discovered.size(), m_impl->probeErrors.size());
    return Status::success();
}

const std::vector<transport::DiscoveredDevice>& DeviceManager::discovered() const {
    std::scoped_lock const lock(m_impl->mutex);
    return m_impl->discovered;
}

std::vector<DeviceSummary> DeviceManager::devices() const {
    std::scoped_lock const lock(m_impl->mutex);
    std::vector<DeviceSummary> out;
    for (const auto& d : m_impl->discovered) {
        DeviceSummary summary;
        summary.identity.name = d.name;
        summary.identity.serial = d.serial;
        summary.identity.endpoint = d.endpoint;
        auto const it = m_impl->owned.find(d.endpoint);
        if (it != m_impl->owned.end()) {
            summary.state = it->second->state();
            summary.identity = it->second->identity();
        } else {
            summary.state = DeviceState::Disconnected;
        }
        // Capabilities are only reported once the device has actually answered.
        // Until then the summary says so rather than showing a zero that looks
        // like a real limit (master spec section 10).
        summary.rateSummary = "not characterised";
        out.push_back(std::move(summary));
    }
    return out;
}

StatusOr<ICaptureDevice*> DeviceManager::acquire(const std::string& endpoint) {
    std::scoped_lock const lock(m_impl->mutex);
    auto const it = m_impl->owned.find(endpoint);
    if (it != m_impl->owned.end()) {
        if (m_impl->inUse[endpoint]) {
            return Status::error(ErrorCode::DeviceBusy,
                                 fmt::format("device '{}' is already acquired", endpoint));
        }
        m_impl->inUse[endpoint] = true;
        return it->second.get();
    }
    if (m_impl->factory == nullptr) {
        return Status::error(ErrorCode::NotImplemented,
                             "no device factory registered with the DeviceManager");
    }
    transport::DiscoveredDevice descriptor;
    bool found = false;
    for (const auto& d : m_impl->discovered) {
        if (d.endpoint == endpoint) {
            descriptor = d;
            found = true;
            break;
        }
    }
    if (!found) {
        descriptor.endpoint = endpoint;
        descriptor.name = endpoint;
    }
    auto device = m_impl->factory(descriptor);
    if (device == nullptr) {
        return Status::error(ErrorCode::DeviceNotFound,
                             fmt::format("factory could not create a device for '{}'", endpoint));
    }
    auto* raw = device.get();
    m_impl->owned[endpoint] = std::move(device);
    m_impl->inUse[endpoint] = true;
    return raw;
}

void DeviceManager::release(ICaptureDevice* const device) {
    if (device == nullptr) {
        return;
    }
    std::scoped_lock const lock(m_impl->mutex);
    for (auto& [endpoint, owned] : m_impl->owned) {
        if (owned.get() == device) {
            m_impl->inUse[endpoint] = false;
            return;
        }
    }
}

std::size_t DeviceManager::probeCount() const {
    std::scoped_lock const lock(m_impl->mutex);
    return m_impl->probes.size();
}

const std::vector<Status>& DeviceManager::probeErrors() const {
    std::scoped_lock const lock(m_impl->mutex);
    return m_impl->probeErrors;
}

}  // namespace usn::hal
