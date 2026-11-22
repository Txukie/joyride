// joyride launcher - controllers through SDL3, as keys: the d-pad and left
// stick move the focus (arrows, held ones repeat), A accepts (Return), B
// goes back (Escape), Start plays, shoulders change page (Tab). The QML is
// written for the keyboard, so mouse, keys and pads all work the same.
#pragma once
#include <QObject>
#include <QTimer>

class Gamepad : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)

public:
    explicit Gamepad(QObject *parent = nullptr);
    ~Gamepad() override;
    bool connected() const { return connected_; }
    void setPaused(bool p) { paused_ = p; }              // while a game runs: the pad is the game's

signals:
    void connectedChanged();
    void startPressed();

private:
    void poll();
    void key(int qtKey, bool down);
    QTimer timer_;
    bool ok_ = false, connected_ = false, paused_ = false;
    int held_ = 0;                                       // the direction held (a Qt key), for repeats
    qint64 nextRepeat_ = 0;
    int stickDir_ = 0;
};
