#include "ui/InAppDrag.h"
#include "ui/Theme.h"
#include <QApplication>
#include <QPainter>
#include <QEventLoop>
#include <QCursor>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QFontMetrics>
#include <functional>

namespace mt {

class DragBadge : public QWidget {
public:
    DragBadge(const QString& text)
        : QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowTransparentForInput)
        , m_text(text) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_ShowWithoutActivating);
        QFontMetrics fm(theme::uiFont());
        resize(fm.horizontalAdvance(text) + 40, fm.height() + 10);
    }
    void setOk(bool ok) {
        if (ok != m_ok) {
            m_ok = ok;
            update();
        }
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QRectF r = rect().adjusted(1, 1, -1, -1);
        p.setPen(QPen(m_ok ? theme::accent() : theme::border(), 1));
        p.setBrush(theme::withAlpha(theme::raised(), m_ok ? 240 : 200));
        p.drawRoundedRect(r, 5, 5);
        p.setFont(theme::uiFont());
        p.setPen(m_ok ? theme::text() : theme::textDim());
        p.drawText(r.adjusted(10, 0, -24, 0), Qt::AlignVCenter | Qt::AlignLeft, m_text);
        QRectF b(r.right() - 20, r.center().y() - 7, 14, 14);
        p.setPen(Qt::NoPen);
        p.setBrush(m_ok ? theme::accent() : theme::withAlpha(theme::textDim(), 90));
        p.drawEllipse(b);
        p.setPen(theme::bg());
        QFont bf = theme::boldFont();
        p.setFont(bf);
        p.drawText(b, Qt::AlignCenter, m_ok ? "+" : "×");
    }

private:
    QString m_text;
    bool m_ok = false;
};

bool runInAppDrag(QWidget* source, std::unique_ptr<QMimeData> md, const QString& text) {
    if (!source || !md) return false;
    DragBadge badge(text);
    QWidget* target = nullptr;
    bool accepted = false;
    auto pos = QCursor::pos();
    badge.move(pos + QPoint(14, 18));
    badge.show();
    auto leave = [&] {
        if (target) {
            QDragLeaveEvent lv;
            QCoreApplication::sendEvent(target, &lv);
        }
        target = nullptr;
        accepted = false;
    };
    auto track = [&](const QPoint& g) {
        badge.move(g + QPoint(14, 18));
        QWidget* w = QApplication::widgetAt(g);
        // walk up to the nearest widget that takes drops
        while (w && !w->acceptDrops()) w = w->parentWidget();
        if (w != target) {
            leave();
            target = w;
            if (w) {
                QDragEnterEvent en(w->mapFromGlobal(g), Qt::CopyAction, md.get(), Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(w, &en);
                accepted = en.isAccepted();
                if (!accepted) target = nullptr;
            }
        }
        if (target) {
            QDragMoveEvent mv(target->mapFromGlobal(g), Qt::CopyAction, md.get(), Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(target, &mv);
            accepted = mv.isAccepted();
        }
        badge.setOk(target && accepted);
    };
    QEventLoop loop;
    bool dropped = false, cancelled = false;
    struct Filter : QObject {
        std::function<void(const QPoint&)> onMove;
        std::function<void()> onRelease, onCancel;
        bool eventFilter(QObject*, QEvent* e) override {
            if (e->type() == QEvent::MouseMove) {
                onMove(static_cast<QMouseEvent*>(e)->globalPosition().toPoint());
                return true;
            }
            if (e->type() == QEvent::MouseButtonRelease) {
                onRelease();
                return true;
            }
            if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape) {
                onCancel();
                return true;
            }
            return false;
        }
    } filter;
    filter.onMove = track;
    filter.onRelease = [&] {
        dropped = true;
        loop.quit();
    };
    filter.onCancel = [&] {
        cancelled = true;
        loop.quit();
    };
    qApp->installEventFilter(&filter);
    source->grabMouse();
    source->grabKeyboard();
    source->setCursor(Qt::ClosedHandCursor);
    track(pos);
    loop.exec();
    source->releaseKeyboard();
    source->releaseMouse();
    source->unsetCursor();
    qApp->removeEventFilter(&filter);
    badge.hide();
    bool ok = false;
    if (dropped && !cancelled && target && accepted) {
        QPoint g = QCursor::pos();
        QDropEvent dr(QPointF(target->mapFromGlobal(g)), Qt::CopyAction, md.get(), Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(target, &dr);
        ok = dr.isAccepted();
    } else leave();
    return ok;
}

}   // namespace mt
