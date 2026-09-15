#include "usn/app/qt_sink.h"

namespace usn::app {

QtSink::QtSink(QObject* parent) : QObject(parent) {}

void QtSink::write(const usn::log::Record& record) {
    emit recordLogged(static_cast<int>(record.level),
                      QString::fromUtf8(record.category ? record.category : ""),
                      QString::fromUtf8(record.message.data(), static_cast<qsizetype>(record.message.size())),
                      static_cast<quint64>(record.monotonicNs));
}

}  // namespace usn::app
