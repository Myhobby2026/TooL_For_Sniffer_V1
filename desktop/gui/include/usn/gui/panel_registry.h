// -----------------------------------------------------------------------------
// panel_registry.h -- panel discovery and metadata (docs/architecture_review.md 5.5).
// -----------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>
#include <QObject>
#include <QString>

namespace usn::gui {

struct PanelInfo {
    QString id;
    QString title;
    QString qmlSource;
    bool defaultVisible{true};
};

class PanelRegistry : public QObject {
    Q_OBJECT

public:
    explicit PanelRegistry(QObject* parent = nullptr);
    ~PanelRegistry() override = default;

    void registerPanel(PanelInfo info);
    [[nodiscard]] const std::vector<PanelInfo>& panels() const noexcept { return m_panels; }
    [[nodiscard]] const PanelInfo* find(const QString& id) const noexcept;

signals:
    void panelsChanged();

private:
    std::vector<PanelInfo> m_panels;
};

}  // namespace usn::gui
