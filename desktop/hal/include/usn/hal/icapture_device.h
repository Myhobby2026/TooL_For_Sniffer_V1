// -----------------------------------------------------------------------------
// icapture_device.h -- capture lifecycle.
//
// Flushing is a distinct state from Stopped on purpose: "the capture ended but
// data is still in flight" is the root cause of most truncated-capture bugs, and
// making it a visible state makes it testable
// (docs/architecture_review.md section 7.3).
// -----------------------------------------------------------------------------
#pragma once

#include "usn/hal/idevice.h"
#include "usn/trigger/trigger_node.h"

namespace usn::hal {

class ITriggerEngine {
public:
    ITriggerEngine() = default;
    ITriggerEngine(const ITriggerEngine&) = delete;
    ITriggerEngine& operator=(const ITriggerEngine&) = delete;
    virtual ~ITriggerEngine() = default;

    [[nodiscard]] virtual TriggerCapability capabilities() const noexcept = 0;

    // Rejects nodes the device cannot execute, with a specific reason. Compiling a
    // trigger that silently drops a node would be a lie to the user.
    [[nodiscard]] virtual Status compile(const trigger::TriggerNode& ast) = 0;
    [[nodiscard]] virtual Status arm() = 0;
    [[nodiscard]] virtual Status disarm() = 0;

    [[nodiscard]] virtual bool isArmed() const noexcept = 0;
    [[nodiscard]] virtual bool hasFired() const noexcept = 0;
    [[nodiscard]] virtual std::optional<SampleIndex> firedAt() const noexcept = 0;
};

class ICaptureDevice : public IDevice {
public:
    ~ICaptureDevice() override = default;

    // Validated against reported capabilities BEFORE anything is sent, so the user
    // gets "32 channels at 50 MSPS exceeds the reported envelope (max 5 MSPS)"
    // rather than a device NAK or, worse, a silently clamped configuration.
    [[nodiscard]] virtual Status configure(const CaptureConfiguration& config) = 0;
    [[nodiscard]] virtual CaptureConfiguration configuration() const = 0;

    [[nodiscard]] virtual Status arm() = 0;
    [[nodiscard]] virtual Status start() = 0;
    [[nodiscard]] virtual Status stop() = 0;    // graceful: flush, then stop
    [[nodiscard]] virtual Status abort() = 0;   // immediate; reports what was lost

    // Non-owning, single sink. The pipeline outlives the device by contract;
    // setSampleSink(nullptr) detaches.
    virtual void setSampleSink(ISampleSink* sink) = 0;

    [[nodiscard]] virtual CaptureProgress progress() const = 0;

    [[nodiscard]] virtual ITriggerEngine* triggerEngine() noexcept = 0;

    // Device-side self test and pattern generation (master spec section 46).
    [[nodiscard]] virtual Status runSelfTest(std::uint16_t testId) = 0;
    [[nodiscard]] virtual Status generateTestPattern(std::uint16_t patternId,
                                                     std::uint64_t rateHz,
                                                     std::uint32_t durationMs) = 0;
};

}  // namespace usn::hal
