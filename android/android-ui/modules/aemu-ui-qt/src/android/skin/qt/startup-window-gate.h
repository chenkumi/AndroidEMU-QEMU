#pragma once
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QObject>
#include <QPixmap>
#include <QScreen>
#include <QTimer>
#include <QVariant>
#include <QWidget>
#include <QString>
#include <cstdio>
#ifdef _WIN32
#include <qt_windows.h>
#endif

// A first-presentation gate, not an Android boot-completed indicator.
// Handles may exist while hidden; geometry and the backing surface come first.
//
// The launcher can ask for the window to stay hidden while it applies the first-run setup: ANDROIDEMU_WINDOW_HOLD names a
// file holding the launcher's process id. The window is not released while that file exists and its process runs; it is
// released when the file is gone, the process is gone, or kHoldLimitMs has passed (a stuck launcher never hides it for good).
class StartupWindowGate final : public QObject {
public:
    StartupWindowGate(QWidget* container, QWidget* display)
        : QObject(container), mContainer(container), mDisplay(display) {
        container->setProperty("androidemu.startupGate", QVariant::fromValue(static_cast<void*>(this)));
        display->installEventFilter(this);
        mHoldPath = qEnvironmentVariable("ANDROIDEMU_WINDOW_HOLD");
        mHoldClock.start();
    }
    static StartupWindowGate* get(QWidget* container) {
        return static_cast<StartupWindowGate*>(container->property("androidemu.startupGate").value<void*>());
    }
    bool requestShow() {
        if (mReleased) return true;
        mRequested = true; schedule(); return false;
    }
    void positioned() { mPositioned = true; changed(); }
    void surfacePrepared(bool ready) { mSurface = ready; changed(); }
    bool released() const { return mReleased; }
    bool presented() const { return mPresented; }
    bool plannedGeometryVisible() const {
        auto screen = QGuiApplication::screenAt(mContainer->frameGeometry().center());
        if (!screen) screen = QGuiApplication::primaryScreen();
        return screen && screen->availableGeometry().contains(mContainer->frameGeometry());
    }
protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (object == mDisplay && event->type() == QEvent::Paint && mReleased && !mPresented) {
            // Filters run before paintEvent: acknowledge only after paint returns.
            QTimer::singleShot(0, this, [this] {
                if (mPresented || !mContainer->isVisible() || !mDisplay->isVisible()) return;
                mPresented = true;
                const QByteArray token = qgetenv("ANDROIDEMU_UI_READY_TOKEN");
                bool valid = token.size() == 32;
                for (char c : token) valid = valid && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
                if (valid) {
                    const QByteArray line = "ANDROIDEMU_UI_READY_V1 " + token + '\n';
                    std::fwrite(line.constData(), 1, line.size(), stdout);
                    std::fflush(stdout);
                }
            });
        }
        return QObject::eventFilter(object, event);
    }
private:
    static constexpr qint64 kHoldLimitMs = 10 * 60 * 1000;
    bool holding() const {
        if (mHoldPath.isEmpty() || mHoldClock.hasExpired(kHoldLimitMs)) return false;
        if (!QFileInfo::exists(mHoldPath)) return false;
        QFile file(mHoldPath);
        // Present but not readable right now: keep holding, the time limit bounds it.
        if (!file.open(QIODevice::ReadOnly)) return true;
        const qulonglong pid = file.readAll().trimmed().toULongLong();
        if (pid == 0) return false;
#ifdef _WIN32
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
        if (!process) return false;
        const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
        CloseHandle(process);
        return alive;
#else
        return true;
#endif
    }
    void changed() { ++mRevision; schedule(); }
    void schedule() {
        if (mReleased || mQueued || !mRequested || !mPositioned || !mSurface) return;
        mQueued = true;
        const unsigned revision = mRevision;
        QTimer::singleShot(0, this, [this, revision] {
            // Drain already queued position/monitor corrections before releasing
            // the first visible frame. Revision changes restart this fence.
            QTimer::singleShot(0, this, [this, revision] {
            mQueued = false;
            if (mReleased || !mSurface || !mPositioned) return;
            if (holding()) { QTimer::singleShot(250, this, [this] { schedule(); }); return; }
            if (revision != mRevision) { schedule(); return; }
            if (mDisplay->width() <= 0 || mDisplay->height() <= 0) return;
            // Polish/layout and prepare the complete backing image while hidden.
            QPixmap prepared(mDisplay->size()); prepared.fill(Qt::black);
            mDisplay->render(&prepared);
            if (revision != mRevision) { schedule(); return; }
            // Hidden windows need not have a QWindow/screen yet. Clamp the
            // already sized frame rather than recentering using raw guest pixels.
            auto screen = QGuiApplication::screenAt(mContainer->frameGeometry().center());
            if (!screen) screen = QGuiApplication::primaryScreen();
            if (screen) {
                const QRect area = screen->availableGeometry();
                const QRect frame = mContainer->frameGeometry();
                const int x = qBound(area.left(), frame.left(), qMax(area.left(), area.right() - frame.width() + 1));
                const int y = qBound(area.top(), frame.top(), qMax(area.top(), area.bottom() - frame.height() + 1));
                if (QPoint(x, y) != frame.topLeft()) mContainer->move(x, y);
            }
            mReleased = true;
            mContainer->show(); mDisplay->show(); mDisplay->update();
            });
        });
    }
    QWidget* mContainer;
    QWidget* mDisplay;
    bool mRequested = false, mPositioned = false, mSurface = false;
    bool mQueued = false, mReleased = false, mPresented = false;
    unsigned mRevision = 0;
    QString mHoldPath;
    QElapsedTimer mHoldClock;
};
