// joyride launcher: installs the games, prepares joyride, sets it up and
// starts it, by pad, keys or mouse.
#include "backend.h"
#include "gamepad.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QKeyEvent>
#include <QTimer>

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    app.setApplicationName("joyride");
    app.setOrganizationName("joyride");
    QQuickStyle::setStyle("Basic");

    Backend backend;
    Gamepad pad;
    QObject::connect(&backend, &Backend::playingChanged, &pad, [&] { pad.setPaused(backend.playing()); });

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("pad", &pad);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); },
                     Qt::QueuedConnection);
    engine.loadFromModule("JoyrideLauncher", "Main");

    // For checking the interface without a screen: JOYRIDE_LAUNCHER_KEYS="Right,Return,Wait,..."
    // pressed 400 ms apart, then JOYRIDE_LAUNCHER_SHOT=<png> saves the window and quits
    // (and Play starts nothing meanwhile).
    QString shot = qEnvironmentVariable("JOYRIDE_LAUNCHER_SHOT");
    if (!shot.isEmpty() && !engine.rootObjects().isEmpty()) {
        auto *w = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QStringList keys = qEnvironmentVariable("JOYRIDE_LAUNCHER_KEYS").split(',', Qt::SkipEmptyParts);
        static const QMap<QString, int> names{ { "Left", Qt::Key_Left }, { "Right", Qt::Key_Right }, { "Up", Qt::Key_Up },
                                               { "Down", Qt::Key_Down }, { "Return", Qt::Key_Return }, { "Escape", Qt::Key_Escape } };
        int t = 800;
        for (const QString &k : keys) {
            if (k == "Wait") { t += 400; continue; }      // a pause: pages slide in
            QTimer::singleShot(t, w, [w, k] {
                QKeyEvent down(QEvent::KeyPress, names.value(k), Qt::NoModifier), up(QEvent::KeyRelease, names.value(k), Qt::NoModifier);
                QCoreApplication::sendEvent(w, &down), QCoreApplication::sendEvent(w, &up);
            });
            t += 400;
        }
        QTimer::singleShot(t + 600, w, [w, shot] { w->grabWindow().save(shot); QCoreApplication::quit(); });
    }
    return app.exec();
}
