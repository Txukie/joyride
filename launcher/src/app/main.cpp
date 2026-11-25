// joyride launcher: installs the games, prepares joyride, sets it up and
// starts it, by pad, keys or mouse.
#include "backend.h"
#include "gamepad.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QKeyEvent>
#include <QTimer>

// The desktop knows an application by its .desktop file: without one the
// portal refuses the app ID ("App info not found for 'joyride'") and
// Wayland shows no icon. This launcher's, and its icon, in the user's own
// data folder; written again if the launcher has moved.
static void install_desktop_entry() {
#ifdef _WIN32
    // Windows: a Start menu shortcut to this launcher (Qt makes it a .lnk)
    QString menu = QDir::fromNativeSeparators(qEnvironmentVariable("APPDATA")) + "/Microsoft/Windows/Start Menu/Programs";
    if (!qEnvironmentVariableIsEmpty("APPDATA") && QDir().mkpath(menu)) {
        QString lnk = menu + "/joyride.lnk";
        QFile::remove(lnk);
        QFile::link(QCoreApplication::applicationFilePath(), lnk);
    }
    return;
#endif
    QString data = qEnvironmentVariable("XDG_DATA_HOME");
    if (data.isEmpty()) data = QDir::homePath() + "/.local/share";
    QString exe = qEnvironmentVariable("APPIMAGE");        // an AppImage: the file itself, not its mount
    if (exe.isEmpty()) exe = QFileInfo("/proc/self/exe").canonicalFilePath();
    if (exe.isEmpty()) return;
    QString entry = "[Desktop Entry]\nType=Application\nName=joyride\nComment=Grand Theft Auto, London 1969 and 1961, recompiled\n"
                    "Exec=\"" + exe + "\"\nIcon=joyride\nTerminal=false\nCategories=Game;\nStartupWMClass=joyride\n";
    QString path = data + "/applications/joyride.desktop";
    QFile old(path);
    if (!old.open(QIODevice::ReadOnly) || QString::fromUtf8(old.readAll()) != entry) {
        old.close();
        QDir().mkpath(data + "/applications");
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(entry.toUtf8());
    }
    for (int n : { 48, 128, 256 }) {
        QString dir = data + QString("/icons/hicolor/%1x%1/apps").arg(n), icon = dir + "/joyride.png";
        QFile src(QString(":/art/joyride-%1.png").arg(n));
        if (!src.open(QIODevice::ReadOnly)) continue;
        QByteArray png = src.readAll();
        QFile cur(icon);
        if (cur.open(QIODevice::ReadOnly) && cur.readAll() == png) continue;
        cur.close();
        QDir().mkpath(dir);
        QFile out(icon);
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) out.write(png);
    }
}

int main(int argc, char **argv) {
    // from an AppImage: Qt's plugins and QML modules are the ones it carries
    // (its qt.conf is not always found, by where Qt takes the program to be)
    QByteArray appdir = qgetenv("APPDIR");
    if (!appdir.isEmpty()) {
        if (qEnvironmentVariableIsEmpty("QT_PLUGIN_PATH")) qputenv("QT_PLUGIN_PATH", appdir + "/usr/plugins");
        if (qEnvironmentVariableIsEmpty("QML_IMPORT_PATH")) qputenv("QML_IMPORT_PATH", appdir + "/usr/qml");
    }
    QGuiApplication app(argc, argv);
    if (qEnvironmentVariableIsEmpty("JOYRIDE_LAUNCHER_SHOT")) install_desktop_entry();   // (Windows: needs the app's path)
    app.setApplicationName("joyride");
    app.setOrganizationName("joyride");
    app.setApplicationVersion(JOYRIDE_VERSION);
    app.setDesktopFileName("joyride");
    QIcon icon;
    for (int n : { 16, 32, 48, 64, 128, 256 }) icon.addFile(QString(":/art/joyride-%1.png").arg(n), QSize(n, n));
    app.setWindowIcon(icon);
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
