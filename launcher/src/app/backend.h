// joyride launcher - what the QML front end drives: the games and their
// state, installing (from the player's own copy), preparing joyride (the
// recompilation, from GTA's executable), the settings, playing.
#pragma once
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QVariant>

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList games READ games NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString task READ task NOTIFY busyChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString log READ log NOTIFY logChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY busyChanged)
    Q_PROPERTY(bool joyrideReady READ joyrideReady NOTIFY changed)
    Q_PROPERTY(QStringList steamInstalls READ steamInstalls NOTIFY changed)
    Q_PROPERTY(QString homeDir READ homeDir CONSTANT)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(QString backdrop READ backdrop CONSTANT)

public:
    explicit Backend(QObject *parent = nullptr);

    QVariantList games() const;
    bool busy() const { return !task_.isEmpty(); }
    QString task() const { return task_; }
    double progress() const { return progress_; }
    QString log() const { return log_.join('\n'); }
    QString lastError() const { return error_; }
    bool joyrideReady() const;
    QStringList steamInstalls() const;
    QString homeDir() const;
    bool playing() const { return playing_; }
    QString backdrop() const;                                       // the menus' wide picture, if the player has one

    Q_INVOKABLE void installFrom(const QString &id, const QString &path);
    Q_INVOKABLE void uninstall(const QString &id);
    Q_INVOKABLE void prepare();                                     // build joyride from GTA's executable
    Q_INVOKABLE void play(const QString &id);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void clearError();

    // the settings (joyride.cfg, shared with the in-game F1 menu)
    Q_INVOKABLE QVariantList settingsModel() const;                 // [{key, label, options:[{value,label}], value}]
    Q_INVOKABLE void setSetting(const QString &key, const QString &value);

    // what to look for when choosing a file for `id` (the browser's filter)
    Q_INVOKABLE QStringList sourceFilters(const QString &id) const;
    Q_INVOKABLE QString sourceHint(const QString &id) const;

signals:
    void changed();
    void busyChanged();
    void progressChanged();
    void logChanged();
    void finished(bool ok, const QString &message);
    void playingChanged();

private:
    void begin(const QString &task);
    void end(bool ok, const QString &message);
    void addLog(const QString &line);
    void setProgress(double p);
    void runInstall(const QString &id, const QString &path);
    void runSteps();                                                // prepare()'s commands, one after another

    QString sourceDir() const;                                      // joyride's checkout (build/, tools/)
    QString joyrideBinary() const;
    QString cfgPath() const;
    QMap<QString, QString> readCfg() const;

    QProcess *proc_ = nullptr;
    QList<QPair<QString, QStringList>> steps_;
    QString task_, error_;
    QStringList log_;
    double progress_ = 0;
    bool playing_ = false;
};
