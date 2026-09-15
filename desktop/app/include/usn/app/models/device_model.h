// -----------------------------------------------------------------------------
// device_model.h -- QAbstractListModel for discovered hardware / capture devices.
// -----------------------------------------------------------------------------
#pragma once

#include <vector>
#include <QAbstractListModel>
#include <QString>

namespace usn::app {

struct DiscoveredDeviceEntry {
    QString name;
    QString serial;
    QString endpoint;
    QString description;
};

class DeviceModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        SerialRole,
        EndpointRole,
        DescriptionRole
    };

    explicit DeviceModel(QObject* parent = nullptr);
    ~DeviceModel() override = default;

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void setDevices(std::vector<DiscoveredDeviceEntry> devices);

signals:
    void countChanged();

private:
    std::vector<DiscoveredDeviceEntry> m_devices;
};

}  // namespace usn::app
