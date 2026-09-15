// -----------------------------------------------------------------------------
// qt_sink.h -- bridge from Qt-free usn::log::ISink to Qt signal/slot.
// -----------------------------------------------------------------------------
#pragma once

#include <QObject>
#include <QString>
#include "usn/common/log.h"

namespace usn::app {

class QtSink : public QObject, public usn::log::ISink {
    Q_OBJECT

public:
    explicit QtSink(QObject* parent = nullptr);
    ~QtSink() override = default;

    void write(const usn::log::Record& record) override;

signals:
    void recordLogged(int level, const QString& category, const QString& message, quint64 monotonicNs);
};

}  // namespace usn::app
