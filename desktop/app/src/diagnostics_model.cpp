#include "usn/app/models/diagnostics_model.h"

namespace usn::app {

namespace {

QString levelToString(int level) {
    switch (level) {
    case 0: return QStringLiteral("TRACE");
    case 1: return QStringLiteral("DEBUG");
    case 2: return QStringLiteral("INFO");
    case 3: return QStringLiteral("WARN");
    case 4: return QStringLiteral("ERROR");
    case 5: return QStringLiteral("FATAL");
    default: return QStringLiteral("INFO");
    }
}

}  // namespace

DiagnosticsModel::DiagnosticsModel(QObject* parent, std::size_t maxEntries)
    : QAbstractListModel(parent), m_maxEntries(maxEntries) {}

int DiagnosticsModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_entries.size());
}

QVariant DiagnosticsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_entries.size())) {
        return QVariant();
    }

    const auto& entry = m_entries[static_cast<std::size_t>(index.row())];
    switch (role) {
    case Qt::DisplayRole:
    case MessageRole:
        return entry.message;
    case LevelRole:
        return entry.level;
    case CategoryRole:
        return entry.category;
    case TimestampRole:
        return entry.monotonicNs;
    case LevelNameRole:
        return levelToString(entry.level);
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> DiagnosticsModel::roleNames() const {
    return {
        {LevelRole, "level"},
        {CategoryRole, "category"},
        {MessageRole, "message"},
        {TimestampRole, "timestamp"},
        {LevelNameRole, "levelName"}
    };
}

void DiagnosticsModel::appendRecord(int level, const QString& category, const QString& message, quint64 monotonicNs) {
    if (m_entries.size() >= m_maxEntries) {
        beginRemoveRows(QModelIndex(), 0, 0);
        m_entries.pop_front();
        endRemoveRows();
        ++m_evictedCount;
        emit evictedCountChanged();
    }

    int newRow = static_cast<int>(m_entries.size());
    beginInsertRows(QModelIndex(), newRow, newRow);
    m_entries.push_back(DiagnosticEntry{level, category, message, monotonicNs});
    endInsertRows();
    emit countChanged();
}

void DiagnosticsModel::clear() {
    beginResetModel();
    m_entries.clear();
    m_evictedCount = 0;
    endResetModel();
    emit countChanged();
    emit evictedCountChanged();
}

}  // namespace usn::app
