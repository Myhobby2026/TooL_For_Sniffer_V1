#include "usn/app/qml_registration.h"

#include "usn/app/application.h"
#include "usn/app/models/channel_model.h"
#include "usn/app/models/device_model.h"
#include "usn/app/models/diagnostics_model.h"
#include "usn/app/viewmodels/capture_view_model.h"
#include "usn/app/viewmodels/waveform_view_model.h"

namespace usn::app {

void registerQmlTypes() {
    qmlRegisterType<DiagnosticsModel>("UniversalSniffer.App", 1, 0, "DiagnosticsModel");
    qmlRegisterType<DeviceModel>("UniversalSniffer.App", 1, 0, "DeviceModel");
    qmlRegisterType<ChannelModel>("UniversalSniffer.App", 1, 0, "ChannelModel");
    qmlRegisterType<CaptureViewModel>("UniversalSniffer.App", 1, 0, "CaptureViewModel");
    qmlRegisterType<WaveformViewModel>("UniversalSniffer.App", 1, 0, "WaveformViewModel");
}

}  // namespace usn::app
