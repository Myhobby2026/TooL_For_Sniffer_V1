// -----------------------------------------------------------------------------
// fake_capture_device.h -- deterministic capture device with fault injection.
//
// Ships in the library rather than in tests/ because tools and the replay path use
// it too. Every failure mode in master spec section 14 is injectable here, which
// is what makes "never silently discard data" a tested property instead of an
// intention:
//
//   injectDeviceOverflow(n)    sets OVERFLOW_BEFORE on the next n blocks
//   injectSequenceGap(n)       skips n sequence numbers
//   injectSampleIndexGap(n)    jumps the sample index forward by n
//   injectDeviceReset()        changes streamId + bootNonce mid-stream
//   injectCrcCorruption()      (used with LoopbackTransport) corrupts the link
//   setBackpressure(bp)        makes the device observe a rejecting sink
//
// Blocks are produced by an explicit pump(), not a timer, so tests are
// deterministic and never flaky.
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "usn/hal/icapture_device.h"

namespace usn::hal {

class FakeTriggerEngine final : public ITriggerEngine {
public:
    explicit FakeTriggerEngine(TriggerCapability caps);

    [[nodiscard]] TriggerCapability capabilities() const noexcept final { return m_caps; }
    [[nodiscard]] Status compile(const trigger::TriggerNode& ast) final;
    [[nodiscard]] Status arm() final;
    [[nodiscard]] Status disarm() final;
    [[nodiscard]] bool isArmed() const noexcept final { return m_armed; }
    [[nodiscard]] bool hasFired() const noexcept final { return m_firedAt.has_value(); }
    [[nodiscard]] std::optional<SampleIndex> firedAt() const noexcept final { return m_firedAt; }

    // Test control.
    // A setter rather than assignment: ITriggerEngine deletes copy/move
    // assignment, so `trigger = FakeTriggerEngine(caps)` cannot compile.
    void setCapabilities(TriggerCapability caps) noexcept;
    void fire(SampleIndex at) noexcept;
    [[nodiscard]] const trigger::TriggerNode* compiled() const noexcept;

private:
    TriggerCapability m_caps;
    std::optional<trigger::TriggerNode> m_compiled;
    bool m_armed{false};
    std::optional<SampleIndex> m_firedAt;
};

struct FakeDeviceOptions {
    DeviceIdentity identity;
    DeviceCapabilities capabilities;
    std::uint32_t samplesPerBlock{1024};
    // Synthetic waveform selector. Counter patterns make lost or duplicated
    // samples obvious, which is exactly what a fake device is for.
    enum class Pattern : std::uint8_t { Zeros, Counter, Toggle, SpiLike, I2cLike, UartLike };
    Pattern pattern{Pattern::Counter};
};

class FakeCaptureDevice final : public ICaptureDevice {
public:
    explicit FakeCaptureDevice(FakeDeviceOptions options);
    ~FakeCaptureDevice() final;

    [[nodiscard]] DeviceIdentity identity() const final;
    [[nodiscard]] StatusOr<DeviceCapabilities> queryCapabilities() final;
    [[nodiscard]] Status connect() final;
    void disconnect() final;
    [[nodiscard]] DeviceState state() const noexcept final;
    [[nodiscard]] Status healthCheck() final;
    [[nodiscard]] DeviceCounters counters() const final;

    [[nodiscard]] Status configure(const CaptureConfiguration& config) final;
    [[nodiscard]] CaptureConfiguration configuration() const final;
    [[nodiscard]] Status arm() final;
    [[nodiscard]] Status start() final;
    [[nodiscard]] Status stop() final;
    [[nodiscard]] Status abort() final;
    void setSampleSink(ISampleSink* sink) final;
    [[nodiscard]] CaptureProgress progress() const final;
    [[nodiscard]] ITriggerEngine* triggerEngine() noexcept final;
    [[nodiscard]] Status runSelfTest(std::uint16_t testId) final;
    [[nodiscard]] Status generateTestPattern(std::uint16_t patternId, std::uint64_t rateHz,
                                             std::uint32_t durationMs) final;

    // --- deterministic production -------------------------------------------
    // Produces `count` blocks and pushes them to the sink. Returns how many were
    // accepted. Honours RejectedStopCapture by moving to the Error state, exactly
    // as a real device would.
    std::size_t pump(std::size_t count);

    // --- fault injection -----------------------------------------------------
    void injectDeviceOverflow(std::uint32_t blocks);
    void injectSequenceGap(std::uint32_t skip);
    void injectSampleIndexGap(std::uint64_t skip);
    void injectDeviceReset();
    void setCapabilitiesOverride(DeviceCapabilities caps);
    void setFailsCapabilityQuery(bool fails) noexcept;

    [[nodiscard]] std::uint64_t currentStreamId() const noexcept;
    [[nodiscard]] std::uint32_t currentSequence() const noexcept;
    [[nodiscard]] SampleIndex nextSampleIndex() const noexcept;

private:
    std::uint32_t sampleWordAt(std::uint64_t index) const noexcept;
    // string_view, not string: Status::message() returns a view.
    void emitDiagnostic(ErrorCode code, std::string_view message);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace usn::hal
