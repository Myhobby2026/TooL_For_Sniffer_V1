// -----------------------------------------------------------------------------
// idevice.cpp -- state predicates and enum names.
// -----------------------------------------------------------------------------
#include "usn/hal/idevice.h"

namespace usn::hal {

std::string_view nameOf(DeviceState const state) noexcept {
    switch (state) {
    case DeviceState::Disconnected: return "disconnected";
    case DeviceState::Connecting:   return "connecting";
    case DeviceState::Connected:    return "connected";
    case DeviceState::Configuring:  return "configuring";
    case DeviceState::Armed:        return "armed";
    case DeviceState::Capturing:    return "capturing";
    case DeviceState::Flushing:     return "flushing";
    case DeviceState::Stopping:     return "stopping";
    case DeviceState::Error:        return "error";
    }
    return "unknown";
}

bool isTerminal(DeviceState const state) noexcept {
    return state == DeviceState::Disconnected || state == DeviceState::Error;
}

// Flushing is deliberately NOT a legal start state: data from the previous
// capture is still in flight, and starting now would interleave two streams.
bool canStartCapture(DeviceState const state) noexcept {
    return state == DeviceState::Connected || state == DeviceState::Armed;
}

std::string_view nameOf(Backpressure const bp) noexcept {
    switch (bp) {
    case Backpressure::Accepted:            return "accepted";
    case Backpressure::AcceptedWithWarning: return "accepted-with-warning";
    case Backpressure::RejectedStopCapture: return "rejected-stop-capture";
    }
    return "unknown";
}

}  // namespace usn::hal
