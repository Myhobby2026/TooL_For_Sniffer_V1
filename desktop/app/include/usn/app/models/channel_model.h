// -----------------------------------------------------------------------------
// channel_model.h -- QAbstractListModel for capture channels.
// -----------------------------------------------------------------------------
#pragma once

#include <vector>
#include <QAbstractListModel>
#include <QColor>
#include <QString>

namespace usn::app {

struct ChannelItem {
    int id{0};
    QString name;
    int bitPosition{0};
    bool enabled{true};
    bool inverted{false};
    QColor color{0, 204, 102};
};

class ChannelModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        NameRole,
        BitPositionRole,
        EnabledRole,
        InvertedRole,
        ColorRole
    };

    explicit ChannelModel(QObject* parent = nullptr);
    ~ChannelModel() override = default;

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void setChannels(std::vector<ChannelItem> channels);
    [[nodiscard]] const std::vector<ChannelItem>& channels() const noexcept { return m_channels; }

signals:
    void countChanged();

private:
    std::vector<ChannelItem> m_channels;
};

}  // namespace usn::app
