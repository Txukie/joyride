// joyride launcher - controllers as keys.
#include "gamepad.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QWindow>

#include <SDL3/SDL.h>

#include <cstdlib>

Gamepad::Gamepad(QObject *parent) : QObject(parent) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");   // SIGINT and SIGTERM end the launcher, as they should
    ok_ = SDL_Init(SDL_INIT_GAMEPAD);
    if (!ok_) return;
    qApp->installEventFilter(this);
    connect(&timer_, &QTimer::timeout, this, &Gamepad::poll);
    timer_.start(16);
}

Gamepad::~Gamepad() {
    if (ok_) SDL_Quit();
}

static const quint32 kPadScanCode = 0x6A6F70;            // marks the pad's own key events
static const qint64 kSameMs = 120;                       // a press from both, this close: the same one

void Gamepad::key(int qtKey, bool down) {
    QWindow *w = QGuiApplication::focusWindow();
    if (!w) return;
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (down) {
        if (now - nativeAt_.value(qtKey, -100000) < kSameMs) { skipped_.insert(qtKey); return; }   // the keyboard's came first
        down_.insert(qtKey);
    } else {
        down_.remove(qtKey);
        if (skipped_.remove(qtKey)) return;
    }
    sentAt_[qtKey] = now;
    QKeyEvent e(down ? QEvent::KeyPress : QEvent::KeyRelease, qtKey, Qt::NoModifier, kPadScanCode, 0, 0);
    QCoreApplication::sendEvent(w, &e);
}

bool Gamepad::eventFilter(QObject *obj, QEvent *ev) {
    if ((ev->type() == QEvent::KeyPress || ev->type() == QEvent::KeyRelease) && connected_ && !paused_) {
        auto *ke = static_cast<QKeyEvent *>(ev);
        int k = ke->key();
        bool mapped = k == Qt::Key_Up || k == Qt::Key_Down || k == Qt::Key_Left || k == Qt::Key_Right ||
                      k == Qt::Key_Return || k == Qt::Key_Enter || k == Qt::Key_Escape || k == Qt::Key_Tab || k == Qt::Key_Backtab;
        if (mapped && ke->nativeScanCode() != kPadScanCode && obj->isWindowType()) {
            qint64 now = QDateTime::currentMSecsSinceEpoch();
            int kk = k == Qt::Key_Enter ? Qt::Key_Return : k;
            if (down_.contains(kk) || now - sentAt_.value(kk, -100000) < kSameMs) return true;   // the pad's, again
            nativeAt_[kk] = now;
        }
    }
    return QObject::eventFilter(obj, ev);
}

void Gamepad::poll() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_EVENT_GAMEPAD_ADDED:
            SDL_OpenGamepad(ev.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (SDL_Gamepad *g = SDL_GetGamepadFromID(ev.gdevice.which)) SDL_CloseGamepad(g);
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP: {
            if (paused_) break;
            bool down = ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
            int k = 0;
            switch (ev.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP: k = Qt::Key_Up; break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN: k = Qt::Key_Down; break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT: k = Qt::Key_Left; break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: k = Qt::Key_Right; break;
            case SDL_GAMEPAD_BUTTON_SOUTH: k = Qt::Key_Return; break;
            case SDL_GAMEPAD_BUTTON_EAST: k = Qt::Key_Escape; break;
            case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: k = Qt::Key_Backtab; break;
            case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: k = Qt::Key_Tab; break;
            case SDL_GAMEPAD_BUTTON_START:
                if (down) emit startPressed();
                break;
            default: break;
            }
            if (!k) break;
            key(k, down);
            bool dir = k == Qt::Key_Up || k == Qt::Key_Down || k == Qt::Key_Left || k == Qt::Key_Right;
            if (dir && down) held_ = k, nextRepeat_ = QDateTime::currentMSecsSinceEpoch() + 400;
            else if (dir && held_ == k) held_ = 0;
            break;
        }
        default: break;
        }
    }
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if ((n > 0) != connected_) connected_ = n > 0, emit connectedChanged();
    // the left stick as a d-pad: a press on entering a direction, repeats while held
    int dir = 0;
    for (int i = 0; i < n && !dir && !paused_; i++) {
        SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i]);
        if (!g) continue;
        int x = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
        // a direction taken past 24000, kept until back under 14000: no flicker at the edge
        int dead = stickDir_ ? 14000 : 24000;
        if (std::abs(x) > std::abs(y)) dir = x > dead ? Qt::Key_Right : x < -dead ? Qt::Key_Left : 0;
        else dir = y > dead ? Qt::Key_Down : y < -dead ? Qt::Key_Up : 0;
    }
    SDL_free(ids);
    if (paused_) { held_ = stickDir_ = 0; return; }
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (dir != stickDir_) {
        if (stickDir_) key(stickDir_, false);
        if (dir) key(dir, true), nextRepeat_ = now + 400;
        stickDir_ = dir;
    }
    int rep = held_ ? held_ : stickDir_;
    if (rep && now >= nextRepeat_) {
        key(rep, true);
        nextRepeat_ = now + 160;
    }
}
