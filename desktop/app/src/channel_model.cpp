#include "usn/app/models/channel_model.h"

namespace usn::app {

ChannelModel::ChannelModel(QObject* parent) : QAbstractListModel(parent) {}

int ChannelModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_channels.size());
}

QVariant ChannelModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_channels.size())) {
        return QVariant();
    }

    const auto& ch = m_channels[static_cast<std::size_t>(index.row())];
    switch (role) {
    case Qt::DisplayRole:
    case NameRole:
        return ch.name;
    case IdRole:
        return ch.id;
    case BitPositionRole:
        return ch.bitPosition;
    case EnabledRole:
        return ch.enabled;
    case InvertedRole:
        return ch.inverted;
    case ColorRole:
        return ch.color;
    default:
        return QVariant();
    }
}

bool ChannelModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_channels.size())) {
        return false;
    }

    auto& ch = m_channels[static_cast<std::size_t>(index.row())];
    if (role == EnabledRole) {
        ch.enabled = value.toBool();
        emit dataChanged(index, index, {EnabledRole});
        return true;
    } else if (role == NameRole) {
        ch.name = value.toString();
        emit dataChanged(index, index, {NameRole, Qt::DisplayRole});
        return true;
    }
    return false;
}

QHash<int, QByteArray> ChannelModel::roleNames() const {
    return {
        {IdRole, "channelId"},
        {NameRole, "name"},
        {BitPositionRole, "bitPosition"},
        {EnabledRole, "enabled"},
        {InvertedRole, "inverted"},
        {ColorRole, "color"}
    };
}

void ChannelModel::setChannels(std::vector<ChannelItem> channels) {
    beginResetModel();
    m_channels = std::move(channels);
    endResetModel();
    emit countChanged();
}

}  // namespace usn::app
