#include "usn/gui/layout_description.h"

#include <nlohmann/json.hpp>

namespace usn::gui {

using json = nlohmann::json;

LayoutDescription::LayoutDescription(QObject* parent) : QObject(parent) {
    addPanel({QStringLiteral("capture"), true, 350, 600});
    addPanel({QStringLiteral("diagnostics"), true, 600, 300});
}

void LayoutDescription::addPanel(PanelLayoutItem item) {
    m_items.push_back(std::move(item));
    emit layoutChanged();
}

QString LayoutDescription::toJson() const {
    json arr = json::array();
    for (const auto& item : m_items) {
        json obj;
        obj["panelId"] = item.panelId.toStdString();
        obj["visible"] = item.visible;
        obj["width"] = item.width;
        obj["height"] = item.height;
        arr.push_back(std::move(obj));
    }
    return QString::fromStdString(arr.dump(2));
}

bool LayoutDescription::fromJson(const QString& jsonString) {
    try {
        auto arr = json::parse(jsonString.toStdString());
        if (!arr.is_array()) {
            return false;
        }
        std::vector<PanelLayoutItem> newItems;
        for (const auto& obj : arr) {
            PanelLayoutItem item;
            item.panelId = QString::fromStdString(obj.value("panelId", ""));
            item.visible = obj.value("visible", true);
            item.width = obj.value("width", 400);
            item.height = obj.value("height", 300);
            newItems.push_back(std::move(item));
        }
        m_items = std::move(newItems);
        emit layoutChanged();
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace usn::gui
