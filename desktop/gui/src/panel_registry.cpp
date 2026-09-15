#include "usn/gui/panel_registry.h"

namespace usn::gui {

PanelRegistry::PanelRegistry(QObject* parent) : QObject(parent) {
    registerPanel({
        QStringLiteral("capture"),
        QStringLiteral("Capture Control"),
        QStringLiteral("qrc:/UniversalSniffer/Gui/qml/panels/CapturePanel.qml"),
        true
    });

    registerPanel({
        QStringLiteral("diagnostics"),
        QStringLiteral("Diagnostics Log"),
        QStringLiteral("qrc:/UniversalSniffer/Gui/qml/panels/DiagnosticsPanel.qml"),
        true
    });
}

void PanelRegistry::registerPanel(PanelInfo info) {
    for (auto& p : m_panels) {
        if (p.id == info.id) {
            p = std::move(info);
            emit panelsChanged();
            return;
        }
    }
    m_panels.push_back(std::move(info));
    emit panelsChanged();
}

const PanelInfo* PanelRegistry::find(const QString& id) const noexcept {
    for (const auto& p : m_panels) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

}  // namespace usn::gui
