// -----------------------------------------------------------------------------
// diagnostics_model.h -- QAbstractListModel for live diagnostic log events.
// -----------------------------------------------------------------------------
#pragma once

#include <deque>
#include <QAbstractListModel>
#include <QString>

namespace usn::app {

struct DiagnosticEntry {
    int level{0};
    QString category;
    QString message;
    quint64 monotonicNs{0};
};

class DiagnosticsModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(quint64 evictedCount READ evictedCount NOTIFY evictedCountChanged)

public:
    enum Roles {
        LevelRole = Qt::UserRole + 1,
        CategoryRole,
        MessageRole,
        TimestampRole,
        LevelNameRole
    };

    explicit DiagnosticsModel(QObject* parent = nullptr, std::size_t maxEntries = 1000);
    ~DiagnosticsModel() override = default;

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] quint64 evictedCount() const noexcept { return m_evictedCount; }

public slots:
    void appendRecord(int level, const QString& category, const QString& message, quint64 monotonicNs);
    void clear();

signals:
    void countChanged();
    void evictedCountChanged();

private:
    std::size_t m_maxEntries{1000};
    std::deque<DiagnosticEntry> m_entries;
    quint64 m_evictedCount{0};
};

}  // namespace usn::app
