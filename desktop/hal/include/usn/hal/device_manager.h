// -----------------------------------------------------------------------------
// device_manager.h -- discovery and lifecycle (master spec section 40).
//
// Discovery is a strategy (IDeviceProbe), not a hard-coded platform scan. Phase 1
// ships ManualProbe (the user supplies the endpoint) and NullProbe; the platform
// probes (Windows SetupAPI, Linux /sys/class/tty, macOS IOKit) arrive in Phase 3
// when there is real hardware to enumerate.
// -----------------------------------------------------------------------------
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "usn/hal/icapture_device.h"
#include "usn/transport/itransport.h"

namespace usn::hal {

struct DeviceSummary {
    DeviceIdentity identity;
    DeviceState state{DeviceState::Disconnected};
    bool capabilitiesKnown{false};
    std::uint16_t channelCountMax{0};
    std::string rateSummary;   // human text, or "not characterised" when unknown
};

class IDeviceProbe {
public:
    IDeviceProbe() = default;
    IDeviceProbe(const IDeviceProbe&) = delete;
    IDeviceProbe& operator=(const IDeviceProbe&) = delete;
    virtual ~IDeviceProbe() = default;

    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual StatusOr<std::vector<transport::DiscoveredDevice>> scan() = 0;
};

// Returns nothing. Present so that a "no probes configured yet" system behaves
// predictably instead of accidentally matching every serial port on the machine.
class NullProbe final : public IDeviceProbe {
public:
    [[nodiscard]] std::string name() const final { return "null"; }
    [[nodiscard]] StatusOr<std::vector<transport::DiscoveredDevice>> scan() final;
};

// Wraps a user-supplied list of endpoints.
class ManualProbe final : public IDeviceProbe {
public:
    void addEndpoint(std::string endpoint, std::string description = {});
    void clear();
    [[nodiscard]] std::string name() const final { return "manual"; }
    [[nodiscard]] StatusOr<std::vector<transport::DiscoveredDevice>> scan() final;

private:
    std::vector<transport::DiscoveredDevice> m_endpoints;
};

class DeviceManager {
public:
    using DeviceFactory =
        std::function<std::unique_ptr<ICaptureDevice>(const transport::DiscoveredDevice&)>;

    DeviceManager();
    ~DeviceManager();

    void addProbe(std::unique_ptr<IDeviceProbe> probe);
    void setDeviceFactory(DeviceFactory factory);

    // Re-runs every probe. Failures from individual probes are collected rather
    // than aborting the scan: one broken probe must not hide a working device.
    [[nodiscard]] Status rescan();

    [[nodiscard]] const std::vector<transport::DiscoveredDevice>& discovered() const;
    [[nodiscard]] std::vector<DeviceSummary> devices() const;

    // Non-owning borrow. The manager owns the device.
    [[nodiscard]] StatusOr<ICaptureDevice*> acquire(const std::string& endpoint);
    void release(ICaptureDevice* device);

    [[nodiscard]] std::size_t probeCount() const;
    [[nodiscard]] const std::vector<Status>& probeErrors() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace usn::hal
