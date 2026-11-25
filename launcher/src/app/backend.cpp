// joyride launcher - the backend.
#include "backend.h"

#include "../core/games.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QScreen>
#include <QDirIterator>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUrl>

#ifndef JOYRIDE_SOURCE_DIR
#define JOYRIDE_SOURCE_DIR ""
#endif

static std::string s(const QString &q) { return q.toStdString(); }
static QString q(const std::string &s) { return QString::fromStdString(s); }

Backend::Backend(QObject *parent) : QObject(parent) {}

QString Backend::homeDir() const { return QDir::homePath(); }

// joyride's source: JOYRIDE_SOURCE_DIR; from an AppImage, a copy of the
// one it carries in joyride's data folder (the AppImage is read-only, and
// the build happens there); else the checkout this launcher was built from
QString Backend::sourceDir() const {
    QString e = qEnvironmentVariable("JOYRIDE_SOURCE_DIR");
    if (!e.isEmpty()) return e;
    if (!bundledSource().isEmpty()) return q(jl::dataRoot()) + "/src";
    return QString(JOYRIDE_SOURCE_DIR);
}
QString Backend::bundledSource() const {
    QString app = qEnvironmentVariable("APPDIR");
    QString b = app + "/usr/share/joyride/src";
    return !app.isEmpty() && QFileInfo::exists(b + "/tools/lift.sh") ? b : QString();
}
// the AppImage's source copied out when it is not there or not this
// version's; what was made in it (gen/, build/, .venv/) stays
bool Backend::syncSource() {
    QString from = bundledSource();
    if (from.isEmpty()) return true;
    QString to = sourceDir();
    QFile stampIn(from + "/.joyride-version"), stampOut(to + "/.joyride-version");
    QByteArray want = stampIn.open(QIODevice::ReadOnly) ? stampIn.readAll() : QByteArray();
    if (!want.isEmpty() && stampOut.open(QIODevice::ReadOnly) && stampOut.readAll() == want) return true;
    stampOut.close();
    addLog("Copying joyride's source to " + to);
    QDirIterator it(from, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QString f = it.next(), rel = QDir(from).relativeFilePath(f), dst = to + "/" + rel;
        QDir().mkpath(QFileInfo(dst).path());
        QFile::remove(dst);
        if (!QFile::copy(f, dst)) { addLog("cannot copy " + rel); return false; }
        QFile::setPermissions(dst, QFile::permissions(f) | QFileDevice::WriteOwner | QFileDevice::ReadOwner);
    }
    return true;
}

// what the steps and the game run with: from an AppImage, its libraries,
// and its Python and compiler for the tools
QString Backend::appUsr() const {
    QString b = bundledSource();
    return b.isEmpty() ? QString() : qEnvironmentVariable("APPDIR") + "/usr";
}
QProcessEnvironment Backend::toolEnv() const {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QString usr = appUsr();
    if (usr.isEmpty()) return env;
    QString ld = env.value("LD_LIBRARY_PATH");
    env.insert("LD_LIBRARY_PATH", usr + "/lib" + (ld.isEmpty() ? "" : ":" + ld));
    env.insert("JOYRIDE_PYTHON", python());
    env.insert("CC", usr + "/lib/joyride/cc/joyride-cc");
    env.remove("PYTHONHOME"), env.remove("PYTHONPATH");
    return env;
}
QString Backend::python() const {
    QString usr = appUsr();
    return usr.isEmpty() ? sourceDir() + "/.venv/bin/python" : usr + "/lib/joyride/python/bin/python3";
}

QString Backend::joyrideBinary() const { return sourceDir() + "/build/joyride"; }
bool Backend::joyrideReady() const { return QFileInfo(joyrideBinary()).isExecutable(); }
QString Backend::cfgPath() const { return q(jl::dataRoot()) + "/joyride.cfg"; }

QStringList Backend::steamInstalls() const {
    QStringList out;
    for (auto &p : jl::steamGtaInstalls()) out << q(p);
    return out;
}

QVariantList Backend::games() const {
    QVariantList out;
    for (auto &g : jl::games()) {
        QVariantMap m;
        m["id"] = q(g.id), m["title"] = q(g.title), m["subtitle"] = q(g.subtitle);
        m["installed"] = jl::installed(g.id);
        m["playDir"] = q(jl::playDir(g.id));
        m["playable"] = g.playable;
        m["needs"] = q(g.needs);
        const jl::GameInfo *n = g.needs.empty() ? nullptr : jl::game(g.needs);
        m["needsTitle"] = n ? q(n->title) : QString();
        m["needsMet"] = g.needs.empty() || jl::installed(g.needs);
        out << m;
    }
    return out;
}

// ---- tasks -------------------------------------------------------------------

void Backend::begin(const QString &task) {
    task_ = task, error_.clear(), progress_ = 0;
    log_.clear();
    emit logChanged();
    emit busyChanged();
    emit progressChanged();
}
void Backend::end(bool ok, const QString &message) {
    if (!ok) error_ = message;
    addLog(ok ? "Done." : "Failed: " + message);
    task_.clear();
    emit busyChanged();
    emit changed();
    emit finished(ok, message);
}
void Backend::addLog(const QString &line) {
    for (const QString &l : line.split('\n', Qt::SkipEmptyParts)) log_ << l;
    while (log_.size() > 400) log_.removeFirst();
    emit logChanged();
}
void Backend::setProgress(double p) {
    progress_ = p;
    emit progressChanged();
}
void Backend::clearError() {
    error_.clear();
    emit busyChanged();
}

void Backend::installFrom(const QString &id, const QString &path) {
    if (busy()) return;
    const jl::GameInfo *g = jl::game(s(id));
    begin("Installing " + (g ? q(g->title) : id));
    runInstall(id, path);
}

// the installing itself, off the interface's thread
void Backend::runInstall(const QString &id, const QString &path) {
    double base = 0, span = 1;
    QThread *t = QThread::create([this, id, path, base, span] {
        std::string err;
        auto log = [this](const std::string &l) { QMetaObject::invokeMethod(this, [this, l] { addLog(q(l)); }); };
        auto prog = [this, base, span](double p) { QMetaObject::invokeMethod(this, [this, p, base, span] { setProgress(base + span * p); }); };
        bool ok = jl::install(s(id), s(path), err, log, prog);
        QMetaObject::invokeMethod(this, [this, ok, err] { end(ok, q(err)); });
    });
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
}

void Backend::uninstall(const QString &id) {
    if (busy()) return;
    std::string err;
    if (!jl::uninstall(s(id), err)) error_ = q(err);
    // what was built on it goes too
    for (auto &g : jl::games())
        if (g.needs == s(id)) jl::uninstall(g.id, err);
    emit changed();
    emit busyChanged();
}

void Backend::cancel() {
    if (proc_) proc_->kill();
}

// ---- preparing joyride -----------------------------------------------------------
//
// joyride is made on this computer from GTA's own executable (the code it
// generates may not be shared): tools/lift.sh, then the build. The Python
// tools want their environment first.

void Backend::prepare() {
    if (busy()) return;
    QString src = sourceDir(), gta = q(jl::playDir("gta1"));
    if (gta.isEmpty()) { error_ = "Install Grand Theft Auto first"; emit busyChanged(); return; }
    bool app = !appUsr().isEmpty();             // an AppImage: everything needed is in it
    if (!app) {
        // what the build needs on this computer (the libraries' development
        // files are CMake's to find: it says which in the log)
        QStringList missing;
        for (const char *t : { "python3", "cmake", "ninja", "gcc", "pkg-config" })
            if (QStandardPaths::findExecutable(t).isEmpty()) missing << t;
        if (!missing.isEmpty()) {
            error_ = "joyride is built on this computer, and it needs " + missing.join(", ") +
                     ". Install them (and the development files of SDL3, SDL3_ttf, ffmpeg and Vulkan), then try again.";
            emit busyChanged();
            return;
        }
    }
    begin("Preparing joyride");
    if (!syncSource()) { end(false, "cannot copy joyride's source to " + src); return; }
    if (!QFileInfo::exists(src + "/tools/lift.sh")) { end(false, "joyride's source folder was not found (set JOYRIDE_SOURCE_DIR)"); return; }
    steps_.clear();
    if (app) {
        // the game made from GTA's own executable, linked to the runtime the AppImage carries
        QString rt = appUsr() + "/lib/joyride";
        steps_ << qMakePair(python(), QStringList{ "tools/lift.py", gta + "/WINO/Grand Theft Auto.exe", "--cc", rt + "/cc/joyride-cc" });
        steps_ << qMakePair(python(), QStringList{ "tools/link_game.py", rt });
    } else {
        if (!QFileInfo::exists(src + "/external/pcrecomp/tools/__main__.py"))
            steps_ << qMakePair(QString("git"), QStringList{ "submodule", "update", "--init", "--recursive" });
        if (!QFileInfo::exists(src + "/.venv/bin/python"))
            steps_ << qMakePair(QString("python3"), QStringList{ "-m", "venv", ".venv" });
        steps_ << qMakePair(src + "/.venv/bin/pip", QStringList{ "install", "-q", "-r", "tools/requirements.txt" });   // new ones too
        steps_ << qMakePair(python(), QStringList{ "tools/lift.py", gta + "/WINO/Grand Theft Auto.exe" });
        if (!QFileInfo::exists(src + "/build/build.ninja"))
            steps_ << qMakePair(QString("cmake"), QStringList{ "-S", ".", "-B", "build", "-G", "Ninja" });
        steps_ << qMakePair(QString("cmake"), QStringList{ "--build", "build" });
    }
    // the pictures around the menus on wide screens, from each game's own
    steps_ << qMakePair(python(), QStringList{ "tools/backdrops.py", "--all", q(jl::dataRoot()) });
    setProgress(0.02);
    runSteps();
}

void Backend::runSteps() {
    static int total = 0;
    if (progress_ < 0.03) total = (int)steps_.size();
    if (steps_.isEmpty()) { end(joyrideReady(), joyrideReady() ? "" : "the build gave no joyride"); return; }
    auto [prog, args] = steps_.takeFirst();
    addLog("$ " + QFileInfo(prog).fileName() + " " + args.join(' '));
    proc_ = new QProcess(this);
    proc_->setWorkingDirectory(sourceDir());
    proc_->setProcessEnvironment(toolEnv());
    proc_->setProcessChannelMode(QProcess::MergedChannels);
    connect(proc_, &QProcess::readyRead, this, [this] { addLog(QString::fromLocal8Bit(proc_->readAll()).trimmed()); });
    connect(proc_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        proc_->deleteLater(), proc_ = nullptr;
        if (st != QProcess::NormalExit || code != 0) { steps_.clear(); end(false, "a step failed (see the log)"); return; }
        setProgress(1.0 - (double)steps_.size() / std::max(1, total) * 0.98);
        runSteps();
    });
    connect(proc_, &QProcess::errorOccurred, this, [this, prog](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        proc_->deleteLater(), proc_ = nullptr;
        steps_.clear();
        end(false, "cannot run " + prog + " (is it installed?)");
    });
    proc_->start(prog, args);
}

// ---- playing -------------------------------------------------------------------------

void Backend::play(const QString &id) {
    QString dir = q(jl::playDir(s(id)));
    if (dir.isEmpty() || !joyrideReady()) return;
    if (!qEnvironmentVariableIsEmpty("JOYRIDE_LAUNCHER_SHOT")) {   // checking the interface: start nothing
        qInfo("[launcher] would play %s from %s", qPrintable(id), qPrintable(dir));
        return;
    }
    // this game's picture around the menus, made the first time (a few seconds)
    QString pic = q(jl::dataRoot()) + "/extracted/menu/" + id + ".png", py = python();
    if (!QFileInfo::exists(pic) && QFileInfo::exists(py)) {
        QProcess mk;
        mk.setWorkingDirectory(sourceDir());
        mk.setProcessEnvironment(toolEnv());
        mk.start(py, { "tools/backdrops.py", id, dir + "/GTADATA", pic });
        if (!mk.waitForFinished(60000)) mk.kill();
    }
    QProcess p;                                          // its program, arguments and environment
    p.setProgram(joyrideBinary());
    p.setArguments({ dir });
    QProcessEnvironment env = toolEnv();                  // an AppImage's libraries too
    if (id != "gta1") env.insert("JOYRIDE_PROFILE", id);    // London's saves and maps apart from GTA's
    p.setProcessEnvironment(env);
    auto *game = new QProcess(this);                     // the launcher steps aside while it runs
    game->setProgram(p.program()), game->setArguments(p.arguments());
    game->setProcessEnvironment(env), game->setWorkingDirectory(sourceDir());
    connect(game, &QProcess::finished, this, [this, game] {
        game->deleteLater();
        playing_ = false;
        emit playingChanged();
        emit settingsChanged();                          // the F1 menu may have changed them
    });
    game->start();
    if (!game->waitForStarted(5000)) {
        error_ = "cannot start " + joyrideBinary();
        game->deleteLater();
        emit busyChanged();
        return;
    }
    playing_ = true;
    emit playingChanged();
}

// ---- settings ------------------------------------------------------------------------

// the same defaults as joyride's own (overlay.c)
QMap<QString, QString> Backend::readCfg() const {
    QMap<QString, QString> m{ { "resolution", "desktop" }, { "filter", "1" }, { "scaling", "sharp" }, { "fullscreen", "1" },
                              { "smooth", "1" }, { "renderer", "gpu" }, { "controller", "modern" }, { "hud", "auto" },
                              { "driveby", "game" }, { "retry", "1" } };
    QFile f(cfgPath());
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
        for (const QString &line : QString::fromUtf8(f.readAll()).split('\n')) {
            int eq = line.indexOf('=');
            if (eq > 0 && !line.startsWith('#')) m[line.left(eq).trimmed()] = line.mid(eq + 1).trimmed();
        }
    return m;
}

static QVariantMap opt(const QString &v, const QString &l) { return QVariantMap{ { "value", v }, { "label", l } }; }

// the desktop's size in pixels (the screen the launcher is on)
static QSize desktopSize() {
    QScreen *sc = QGuiApplication::primaryScreen();
    if (!sc) return {};
    return sc->size() * sc->devicePixelRatio();
}

QVariantList Backend::settingsModel() const {
    auto cfg = readCfg();
    QSize d = desktopSize();
    QString dname = d.isValid() ? QString("Desktop (%1×%2)").arg(d.width()).arg(d.height()) : QString("Desktop");
    QVariantList res{ opt("desktop", dname), opt("game", "The game's own (1024×768)") };
    for (const char *r : { "1280x720", "1280x800", "1366x768", "1600x900", "1680x1050", "1920x1080", "1920x1200",
                           "2560x1080", "2560x1440", "2560x1600", "3440x1440", "3840x2160" }) {
        QStringList wh = QString(r).split('x');
        if (d.isValid() && (wh[0].toInt() > d.width() || wh[1].toInt() > d.height())) continue;   // bigger than the screen
        res << opt(r, QString(r).replace('x', "×"));
    }
    bool known = false;                                  // a size set elsewhere (the F1 menu)
    for (auto &o : res) known |= o.toMap()["value"] == cfg["resolution"];
    if (!known) res << opt(cfg["resolution"], QString(cfg["resolution"]).replace('x', "×"));
    auto item = [&](const QString &group, const QString &key, const QString &label, const QVariantList &options) {
        return QVariantMap{ { "group", group }, { "key", key }, { "label", label }, { "options", options }, { "value", cfg.value(key) } };
    };
    // as the F1 menu has them: the picture, then the play
    return {
        item("Graphics", "resolution", "Screen size", res),
        item("Graphics", "fullscreen", "Fullscreen", { opt("1", "Yes"), opt("0", "No") }),
        item("Graphics", "scaling", "Scaling", { opt("sharp", "Sharp"), opt("smooth", "Smooth"), opt("nearest", "Nearest"), opt("integer", "Integer") }),
        item("Graphics", "renderer", "Renderer", { opt("gpu", "GPU (any size at full speed)"), opt("software", "Software (as released)") }),
        item("Graphics", "filter", "Textures", { opt("1", "Bilinear"), opt("0", "Sharp") }),
        item("Graphics", "smooth", "Motion", { opt("1", "Smooth (the display's rate)"), opt("0", "As released (23 fps)") }),
        item("Graphics", "hud", "HUD size", { opt("auto", "Auto"), opt("1", "×1"), opt("2", "×2"), opt("3", "×3"), opt("4", "×4") }),
        item("Gameplay", "controller", "Controller", { opt("modern", "Modern (twin stick)"), opt("classic", "Classic (tank)") }),
        item("Gameplay", "driveby", "Drive-bys", { opt("game", "Like in the game"), opt("always", "Always (online, the host's choice)") }),
        item("Gameplay", "retry", "Retry missions", { opt("1", "Yes (the phone rings again, the car is back)"), opt("0", "No (a failed one is lost)") }),
    };
}

void Backend::setSetting(const QString &key, const QString &value) {
    auto cfg = readCfg();
    cfg[key] = value;
    QDir().mkpath(QFileInfo(cfgPath()).path());
    QFile f(cfgPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) { error_ = "cannot write " + cfgPath(); emit busyChanged(); return; }
    QString out = "# joyride settings (the F1 overlay writes this)\n";
    for (const char *k : { "resolution", "filter", "scaling", "fullscreen", "smooth", "renderer", "controller", "hud", "driveby", "retry" })
        out += QString(k) + "=" + cfg[k] + "\n";
    f.write(out.toUtf8());
    f.close();
    emit settingsChanged();
}

QStringList Backend::sourceFilters(const QString &id) const {
    if (id == "gta1") return { "*.exe", "*.EXE" };
    if (id == "london1969") return { "*.cue", "*.CUE", "*.iso", "*.ISO" };
    if (id == "london1961") return { "*.exe", "*.EXE" };
    return {};
}
QString Backend::sourceHint(const QString &id) const {
    if (id == "gta1") return "GTAINSTALLER.exe (the 2002 Rockstar installer), or a folder holding WINO and GTADATA (Steam's, say): open the folder and choose “Use this folder”.";
    if (id == "london1969") return "Your London 1969 disc: a .cue (with its .bin beside it), an .iso, or the mounted disc's folder.";
    if (id == "london1961") return "GTAL1961.EXE, London 1961's installer.";
    return "";
}
