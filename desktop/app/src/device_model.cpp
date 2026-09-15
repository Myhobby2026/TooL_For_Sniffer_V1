#include "usn/app/models/device_model.h"

namespace usn::app {

DeviceModel::DeviceModel(QObject* parent) : QAbstractListModel(parent) {}

int DeviceModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_devices.size());
}

QVariant DeviceModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_devices.size())) {
        return QVariant();
    }

    const auto& dev = m_devices[static_cast<std::size_t>(index.row())];
    switch (role) {
    case Qt::DisplayRole:
    case NameRole:
        return dev.name;
    case SerialRole:
        return dev.serial;
    case EndpointRole:
        return dev.endpoint;
    case DescriptionRole:
        return dev.description;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> DeviceModel::roleNames() const {
    return {
        {NameRole, "name"},
        {SerialRole, "serial"},
        {EndpointRole, "endpoint"},
        {DescriptionRole, "description"}
    };
}

void DeviceModel::setDevices(std::vector<DiscoveredDeviceEntry> devices) {
    beginResetModel();
    m_devices = std::move(devices);
    endResetModel();
    emit countChanged();
}

}  // namespace usn::app
