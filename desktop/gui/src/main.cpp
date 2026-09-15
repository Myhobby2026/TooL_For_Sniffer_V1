// -----------------------------------------------------------------------------
// main.cpp -- Universal Sniffer GUI bootstrap (Qt 6 / QML).
// -----------------------------------------------------------------------------
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

#include "usn/app/application.h"
#include "usn/app/qml_registration.h"
#include "usn/gui/layout_description.h"
#include "usn/gui/panel_registry.h"

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("UniversalSniffer"));
    app.setApplicationName(QStringLiteral("UniversalSniffer"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));

    QQuickStyle::setStyle(QStringLiteral("Basic"));

    usn::app::registerQmlTypes();

    usn::app::Application snifferApp;
    snifferApp.initialize();

    usn::gui::PanelRegistry panelRegistry;
    usn::gui::LayoutDescription layoutDescription;

    QQmlApplicationEngine engine;

    engine.rootContext()->setContextProperty(QStringLiteral("appController"), &snifferApp);
    engine.rootContext()->setContextProperty(QStringLiteral("diagnosticsModel"), snifferApp.diagnosticsModel());
    engine.rootContext()->setContextProperty(QStringLiteral("deviceModel"), snifferApp.deviceModel());
    engine.rootContext()->setContextProperty(QStringLiteral("channelModel"), snifferApp.channelModel());
    engine.rootContext()->setContextProperty(QStringLiteral("captureViewModel"), snifferApp.captureViewModel());
    engine.rootContext()->setContextProperty(QStringLiteral("waveformViewModel"), snifferApp.waveformViewModel());
    engine.rootContext()->setContextProperty(QStringLiteral("panelRegistry"), &panelRegistry);
    engine.rootContext()->setContextProperty(QStringLiteral("layoutDescription"), &layoutDescription);

    const QUrl url(QStringLiteral("qrc:/UniversalSniffer/Gui/qml/Main.qml"));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url](QObject* obj, const QUrl& objUrl) {
                         if (!obj && url == objUrl) {
                             QCoreApplication::exit(-1);
                         }
                     }, Qt::QueuedConnection);

    engine.load(url);

    return app.exec();
}
