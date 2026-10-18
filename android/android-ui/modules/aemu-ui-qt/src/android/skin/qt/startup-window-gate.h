#pragma once
#include <QEvent>
#include <QGuiApplication>
#include <QObject>
#include <QPixmap>
#include <QScreen>
#include <QTimer>
#include <QVariant>
#include <QWidget>
#include <cstdio>

// A first-presentation gate, not an Android boot-completed indicator.
// Handles may exist while hidden; geometry and the backing surface come first.
class StartupWindowGate final : public QObject {
public:
    StartupWindowGate(QWidget* container, QWidget* display)
        : QObject(container), mContainer(container), mDisplay(display) {
        container->setProperty("androidemu.startupGate", QVariant::fromValue(static_cast<void*>(this)));
        display->installEventFilter(this);
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
};
