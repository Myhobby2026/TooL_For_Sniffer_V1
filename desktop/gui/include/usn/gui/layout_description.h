// -----------------------------------------------------------------------------
// layout_description.h -- declarative UI panel layout serializable to JSON
// (docs/architecture_review.md section 5.5).
// -----------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>
#include <QObject>
#include <QString>

namespace usn::gui {

struct PanelLayoutItem {
    QString panelId;
    bool visible{true};
    int width{400};
    int height{300};
};

class LayoutDescription : public QObject {
    Q_OBJECT

public:
    explicit LayoutDescription(QObject* parent = nullptr);
    ~LayoutDescription() override = default;

    [[nodiscard]] QString toJson() const;
    bool fromJson(const QString& jsonString);

    void addPanel(PanelLayoutItem item);
    [[nodiscard]] const std::vector<PanelLayoutItem>& items() const noexcept { return m_items; }

signals:
    void layoutChanged();

private:
    std::vector<PanelLayoutItem> m_items;
};

}  // namespace usn::gui
