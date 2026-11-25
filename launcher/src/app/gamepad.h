// joyride launcher - controllers through SDL3, as keys: the d-pad and left
// stick move the focus (arrows, held ones repeat), A accepts (Return), B
// goes back (Escape), Start plays, shoulders change page (Tab). The QML is
// written for the keyboard, so mouse, keys and pads all work the same.
//
// A pad can also reach the launcher as keys from elsewhere: Steam Input's
// desktop layout types Esc for B, Return for A, arrows for the d-pad and
// stick. The same press would come twice (B: back, then out of the
// launcher), so a key from the keyboard that the pad has just sent, or
// holds, is dropped, and the pad's own is not sent when the keyboard's has
// just come (eventFilter).
#pragma once
#include <QHash>
#include <QObject>
#include <QSet>
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

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override;

private:
    void poll();
    void key(int qtKey, bool down);
    QHash<int, qint64> sentAt_, nativeAt_;              // when each key last came from the pad, from the keyboard
    QSet<int> down_, skipped_;                           // keys the pad holds; presses of its not sent (their releases neither)
    QTimer timer_;
    bool ok_ = false, connected_ = false, paused_ = false;
    int held_ = 0;                                       // the direction held (a Qt key), for repeats
    qint64 nextRepeat_ = 0;
    int stickDir_ = 0;
};
