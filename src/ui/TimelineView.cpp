#include "ui/TimelineView.h"
#include "ui/Theme.h"
#include <QPainter>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QNativeGestureEvent>
#include <cmath>

namespace mt {

TimelineView::TimelineView(App& app, Tick hbarStep, QWidget* parent)
    : QWidget(parent)
    , m_app(app)
    , m_hbarStep(hbarStep) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    m_ticker = new anim::Ticker(this);
    m_hbar = new QScrollBar(Qt::Horizontal, this);
    m_vbar = new QScrollBar(Qt::Vertical, this);
    m_hbar->setCursor(Qt::ArrowCursor);
    m_vbar->setCursor(Qt::ArrowCursor);   // tool cursors are for the canvas only
    connect(m_hbar, &QScrollBar::valueChanged, this, [this](int v) {
        if (!m_updatingBars) {
            m_scrollTick = (Tick)v * m_hbarStep;
            invalidate();
        }
    });
    connect(m_vbar, &QScrollBar::valueChanged, this, [this](int v) {
        if (!m_updatingBars) {
            onVScroll(v);
            invalidate();
        }
    });
}
void TimelineView::setTool(Tool t) {
    m_tool = t;
    setCursor(toolCursorFor(t));
    Q_EMIT modeChanged(t);
}
// The pointer carries the active tool's icon; the pencil points from its tip, the rest sit centred on the hotspot.
QCursor TimelineView::toolCursorFor(Tool t) {
    switch (t) {
    case Erase: return theme::toolCursor(theme::Glyph::Eraser);
    case MuteTool: return theme::toolCursor(theme::Glyph::MuteSpeaker);
    case Slice: return theme::toolCursor(theme::Glyph::Slice);
    case Select: return theme::toolCursor(theme::Glyph::Marquee);
    default: return QCursor(Qt::ArrowCursor);
    }
}   // draw tool: the regular pointer
Tick TimelineView::snapTicks() const {
    if (m_snapDiv == 0) return 1;
    if (m_snapDiv < 0) return m_app.project().transport.timeSig.ticksPerBar() * -m_snapDiv;
    return PPQ / m_snapDiv;
}
Tick TimelineView::snap(Tick t, bool force) const {
    if (m_altHeld && !force) return t;
    if (snapTicks() <= 1) return t;
    return snapToGrid(t, false);
}
Tick TimelineView::snapNear(Tick t) const {
    if (m_altHeld) return t;
    if (snapTicks() <= 1) return t;
    return snapToGrid(t, true);
}
Tick TimelineView::snapToGrid(Tick t, bool nearest) const {
    TempoMap m = m_app.project().transport.tempoMap();
    int64_t b = m.barAt(t);
    Tick s = std::max<Tick>(1, snapTicks()), lo, hi;
    if (m_snapDiv < 0) {   // whole bars
        b = b / -m_snapDiv * -m_snapDiv;
        lo = m.barStart(b);
        hi = m.barStart(b - m_snapDiv);
    } else {
        Tick bs = m.barStart(b);
        lo = bs + (t - bs) / s * s;
        hi = std::min(lo + s, m.barStart(b + 1));
    }
    return nearest && t - lo >= hi - t ? hi : lo;   // past halfway rounds up
}
void TimelineView::forEachGridLine(Tick from, Tick to, Tick step, int barEvery, const std::function<void(Tick, int64_t, int)>& fn) const {
    TempoMap m = m_app.project().transport.tempoMap();
    step = std::max<Tick>(1, step);
    for (int64_t b = std::max<int64_t>(0, m.barAt(from));; ++b) {
        Tick bs = m.barStart(b);
        if (bs > to) break;
        TimeSignature ts = m.sigAt(bs);
        Tick bar = ts.ticksPerBar(), beat = ts.ticksPerBeat();
        if (bs >= from && b % std::max(1, barEvery) == 0) fn(bs, b, 2);
        for (Tick o = step; o < bar && bs + o <= to; o += step)
            if (bs + o >= from) fn(bs + o, b, o % beat == 0 ? 1 : 0);
    }
}
void TimelineView::zoomTimeAt(int x, double factor) {
    Tick anchor = xToTick(x);
    auto [lo, hi] = pptRange();
    m_ppt = clamp(m_ppt * factor, lo, hi);
    m_scrollTick = std::max<Tick>(0, anchor - (Tick)std::llround((x - timeArea().left()) / m_ppt));
    updateScrollbars();
    invalidate();
}
bool TimelineView::event(QEvent* e) {
    // Escape clears a selection here first; with nothing selected it stays the global 'cancel / close window' shortcut
    if (e->type() == QEvent::ShortcutOverride && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape && hasSelection()) {
        e->accept();
        return true;
    }
    if (e->type() == QEvent::NativeGesture) {   // trackpad pinch
        auto* g = static_cast<QNativeGestureEvent*>(e);
        if (g->gestureType() == Qt::ZoomNativeGesture) {
            zoomTimeAt(g->position().toPoint().x(), 1.0 + g->value());
            return true;
        }
    }
    return QWidget::event(e);
}
void TimelineView::resizeEvent(QResizeEvent*) {
    int left = timeArea().left(), top = rulerHeight();
    m_hbar->setGeometry(left, height() - 14, width() - left - 14, 14);
    m_vbar->setGeometry(width() - 14, top, 14, height() - top - 14);
    updateScrollbars();
    invalidate();
}
// FL Studio keybinds both editors share (typing keyboard mode intercepts unmodified letters before this, see MainWindow).
// Subclasses handle their own keys first and pass everything else here.
bool TimelineView::handleSharedKey(QKeyEvent* e) {
    bool ctrl = e->modifiers() & Qt::ControlModifier, shift = e->modifiers() & Qt::ShiftModifier, alt = e->modifiers() & Qt::AltModifier;
    switch (e->key()) {
    case Qt::Key_Delete: ctrl ? deleteSpace() : deleteSelected(); return true;
    case Qt::Key_Insert:
        if (!ctrl) return false;
        insertSpace();
        return true;
    case Qt::Key_C:
        if (!ctrl) return false;
        copySelected();
        return true;
    case Qt::Key_X:
        if (!ctrl) return false;
        cutSelected();
        return true;
    case Qt::Key_V:
        if (!ctrl) return false;
        paste();
        return true;
    case Qt::Key_A:
        if (!ctrl) return false;
        selectAll();
        return true;
    case Qt::Key_D:
        if (!ctrl) return false;
        m_selected.clear();
        invalidate();
        return true;
    case Qt::Key_B:
        if (!ctrl) return false;
        duplicateSelected();
        return true;
    case Qt::Key_P:
    case Qt::Key_E:
    case Qt::Key_T:
        if (ctrl || alt) return false;
        pickTool(e->key() == Qt::Key_P ? Draw : e->key() == Qt::Key_E ? Select : MuteTool);
        return true;
    case Qt::Key_I:
        if (!shift) return false;
        invertSelection();
        return true;
    case Qt::Key_M:
        if (!alt) return false;
        muteSelected(!shift);
        return true;
    case Qt::Key_PageUp:
    case Qt::Key_PageDown: zoomTimeAt(timeArea().center().x(), e->key() == Qt::Key_PageUp ? 1.3 : 1 / 1.3); return true;
    case Qt::Key_Escape:
        m_selected.clear();
        invalidate();
        return true;
    case Qt::Key_1: pickTool(Draw); return true;
    case Qt::Key_2: pickTool(Select); return true;
    case Qt::Key_3: pickTool(Erase); return true;
    default: return false;
    }
}
bool TimelineView::handleTimeWheel(QWheelEvent* e) {
    int dy = e->angleDelta().y();
    if (e->modifiers() & Qt::ControlModifier) {
        if (dy != 0) zoomTimeAt(e->position().toPoint().x(), std::pow(1.15, dy / 120.0));   // proportional: a trackpad sends many small deltas
        e->accept();
        return true;
    }
    if (e->modifiers() & Qt::ShiftModifier || e->angleDelta().x() != 0) {
        int d = e->angleDelta().x() != 0 ? e->angleDelta().x() : dy;
        m_scrollTick = std::max<Tick>(0, m_scrollTick - (Tick)(d / m_ppt * 0.5));
        updateScrollbars();
        invalidate();
        e->accept();
        return true;
    }
    return false;
}
void TimelineView::prepareCache() {
    m_cache = QPixmap(size() * devicePixelRatioF());
    m_cache.setDevicePixelRatio(devicePixelRatioF());
    m_cache.fill(theme::bg());
}
void TimelineView::drawCache(QPainter& p) {
    if (m_themeRev != theme::revision()) {
        m_themeRev = theme::revision();
        m_cacheValid = false;
    }   // the cached pixmap still holds the old theme
    if (!m_cacheValid) {
        renderCache();
        m_cacheValid = true;
    }
    p.drawPixmap(0, 0, m_cache);
}
void TimelineView::paintRuler(QPainter& p, const QRect& ruler, Tick barStep, Tick last) {
    const auto& ts = m_app.project().transport.timeSig;
    Tick beat = ts.ticksPerBeat(), bar = ts.ticksPerBar();
    p.fillRect(ruler, theme::panel());
    p.setClipRect(ruler);
    p.setFont(theme::smallFont());
    Tick rstep = beat * m_ppt >= 6 ? beat : barStep;
    Tick lstep = barStep;
    while (lstep * m_ppt < 34) lstep *= 2;
    forEachGridLine(m_scrollTick, last, rstep, (int)(rstep / bar), [&](Tick t, int64_t b, int kind) {
        int x = tickToX(t);
        bool labeled = kind == 2 && b % (lstep / bar) == 0;
        p.setPen(labeled ? theme::text() : theme::textDim());
        int len = labeled ? 16 : kind == 2 ? 11 : 7;   // measured from the bottom, so rulers of any height match
        p.drawLine(x, ruler.top() + ruler.height() - len, x, ruler.bottom());
        if (labeled) p.drawText(x + 3, ruler.top() + 12, QString::number(b + 1));
    });
}
void TimelineView::paintPlayhead(QPainter& p, int top, int bottom) {
    if (m_playTick < 0) return;
    int x = tickToX((Tick)m_playTick);
    QRect a = timeArea();
    if (x < a.left() || x > a.right()) return;
    p.setPen(QPen(theme::playhead(), 1.5));
    p.drawLine(x, top, x, bottom);
}
void TimelineView::paintRubber(QPainter& p) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(QPen(theme::withAlpha(theme::accent(), 220), 1));
    p.setBrush(theme::selection());
    p.drawRect(m_rubber.normalized());
    p.restore();
}
void TimelineView::beginRubber(const QPoint& pos, Id toggleOnRelease) {
    m_toggleOnRelease = toggleOnRelease;
    m_selBase = m_selected;
    m_rubber = QRect(pos, QSize(0, 0));
    invalidate();
}
void TimelineView::dragRubber(const QPoint& pos, Qt::KeyboardModifiers mods) {
    m_rubber = QRect(m_pressPos, pos);
    QRect r = m_rubber.normalized();
    if (m_toggleOnRelease && (r.width() >= 3 || r.height() >= 3)) {
        m_toggleOnRelease = 0;
        if (!(mods & Qt::ShiftModifier)) m_selBase.clear();
    }
    m_selected = m_selBase;
    selectInRect(r);
    invalidate();
}
void TimelineView::finishRubber(Qt::KeyboardModifiers mods) {
    bool tiny = m_rubber.normalized().width() < 3 && m_rubber.normalized().height() < 3;
    if (tiny && m_toggleOnRelease) {
        m_selected = m_selBase;
        if (m_selected.count(m_toggleOnRelease)) m_selected.erase(m_toggleOnRelease);
        else m_selected.insert(m_toggleOnRelease);
    } else if (tiny && !(mods & Qt::ShiftModifier)) m_selected.clear();
    m_toggleOnRelease = 0;
}

}   // namespace mt
