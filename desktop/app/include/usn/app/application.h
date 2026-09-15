// -----------------------------------------------------------------------------
// application.h -- application lifecycle coordinator for Qt/QML shell.
// -----------------------------------------------------------------------------
#pragma once

#include <memory>
#include <QObject>

namespace usn::core {
class CaptureSession;
class DiagnosticsLog;
}

namespace usn::hal {
class DeviceManager;
}

namespace usn::app {

class ChannelModel;
class DeviceModel;
class DiagnosticsModel;
class CaptureViewModel;
class WaveformViewModel;
class QtSink;

class Application : public QObject {
    Q_OBJECT

public:
    explicit Application(QObject* parent = nullptr);
    ~Application() override;

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    [[nodiscard]] DiagnosticsModel* diagnosticsModel() const noexcept;
    [[nodiscard]] DeviceModel* deviceModel() const noexcept;
    [[nodiscard]] ChannelModel* channelModel() const noexcept;
    [[nodiscard]] CaptureViewModel* captureViewModel() const noexcept;
    [[nodiscard]] WaveformViewModel* waveformViewModel() const noexcept;

    void initialize();
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace usn::app
