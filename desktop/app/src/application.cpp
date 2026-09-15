#include "usn/app/application.h"

#include <memory>
#include <vector>

#include "usn/app/models/channel_model.h"
#include "usn/app/models/device_model.h"
#include "usn/app/models/diagnostics_model.h"
#include "usn/app/qt_sink.h"
#include "usn/app/viewmodels/capture_view_model.h"
#include "usn/app/viewmodels/waveform_view_model.h"
#include "usn/common/log.h"
#include "usn/hal/device_manager.h"

namespace usn::app {

struct Application::Impl {
    std::shared_ptr<QtSink> qtSink;
    std::unique_ptr<DiagnosticsModel> diagnosticsModel;
    std::unique_ptr<DeviceModel> deviceModel;
    std::unique_ptr<ChannelModel> channelModel;
    std::unique_ptr<CaptureViewModel> captureViewModel;
    std::unique_ptr<WaveformViewModel> waveformViewModel;
    std::unique_ptr<hal::DeviceManager> deviceManager;
};

Application::Application(QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>()) {
    m_impl->qtSink = std::make_shared<QtSink>(this);
    m_impl->diagnosticsModel = std::make_unique<DiagnosticsModel>(this);
    m_impl->deviceModel = std::make_unique<DeviceModel>(this);
    m_impl->channelModel = std::make_unique<ChannelModel>(this);
    m_impl->captureViewModel = std::make_unique<CaptureViewModel>(this);
    m_impl->waveformViewModel = std::make_unique<WaveformViewModel>(this);
    m_impl->deviceManager = std::make_unique<hal::DeviceManager>();

    // Connect sink to diagnostics model via queued connection across threads
    connect(m_impl->qtSink.get(), &QtSink::recordLogged,
            m_impl->diagnosticsModel.get(), &DiagnosticsModel::appendRecord,
            Qt::QueuedConnection);

    // Register QtSink to the core Logger
    usn::log::Logger::instance().addSink(m_impl->qtSink);

    // Populate initial default channels (0..7)
    std::vector<ChannelItem> channels;
    for (int i = 0; i < 8; ++i) {
        ChannelItem item;
        item.id = i;
        item.name = QString("D%1").arg(i);
        item.bitPosition = i;
        item.enabled = true;
        channels.push_back(std::move(item));
    }
    m_impl->channelModel->setChannels(std::move(channels));
}

Application::~Application() {
    shutdown();
}

DiagnosticsModel* Application::diagnosticsModel() const noexcept {
    return m_impl->diagnosticsModel.get();
}

DeviceModel* Application::deviceModel() const noexcept {
    return m_impl->deviceModel.get();
}

ChannelModel* Application::channelModel() const noexcept {
    return m_impl->channelModel.get();
}

CaptureViewModel* Application::captureViewModel() const noexcept {
    return m_impl->captureViewModel.get();
}

WaveformViewModel* Application::waveformViewModel() const noexcept {
    return m_impl->waveformViewModel.get();
}

void Application::initialize() {
    USN_LOG_INFO(usn::log::cats::kApp, "Universal Sniffer initialized");
}

void Application::shutdown() {
    // Teardown
}

}  // namespace usn::app
