// Visual editors for the dynamics effects: compressor, limiter, gate, expander, clipper and transient processor. Each is one
// display - the shape on the left (the transfer curve: level in -> level out; for the transient processor a drum hit before
// and after), a scrolling history of input level, output level and gain change, a gain meter - with knobs below for whatever
// the display does not drag. The handles on the shape (and the threshold line across the history) set the main parameters.
#include "ui/PluginWindow.h"
#include "ui/Theme.h"
#include "dsp/effects/Effects.h"
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <deque>
#include <memory>

namespace mt {
namespace {

Id ownerOf(IProcessor* p) {
    for (auto& [id, proc] : App::instance()->engine().registry().processors())
        if (proc.get() == p) return id;
    return 0;
}

// A point on the shape the mouse drags, in the shape's units (x: level in, dB - or time, ms; y: level out, dB)
struct Handle {
    QString name;
    std::vector<uint32_t> ids;   // the parameters it sets; each becomes an undo step on release
    std::function<QPointF()> at;
    std::function<void(QPointF)> drag;   // the mouse position, in the shape's units
};

class DynamicsView : public QWidget {
public:
    // set up by each editor
    float x0 = -66, x1 = 6, y0 = -66, y1 = 6, xStep = 12, yStep = 12;   // the shape's ranges and grid
    QString xUnit;   // after the x labels when they are not dB
    std::function<float(float)> curve, ref;   // the processed shape and the untouched one (dashed)
    std::function<std::pair<float, float>()> band;   // an x span to shade: knee, hysteresis, soft clip, attack
    std::vector<Handle> handles;
    int line = 0;   // the handle whose level also runs across the history as a line to drag (threshold, ceiling); -1: none
    uint32_t wheelId = 0;   // the wheel over the shape steps this parameter (knee, softness, hysteresis)
    float wheelStep = 1;
    float grRange = 24;   // dB of gain change the meter spans
    bool bipolar = false;   // the gain goes both ways (transient processor): zero sits in the middle
    bool linear = false;   // the shape is in linear amplitude, not dB (clipper); the meter's dB levels are converted

    DynamicsView(IProcessor* proc, DynamicsMeter& meter, QWidget* parent)
        : QWidget(parent)
        , m_proc(proc)
        , m_meter(meter)
        , m_owner(ownerOf(proc)) {
        setObjectName("dynamicsView");
        setMouseTracking(true);
        setMinimumSize(480, 200);
        setToolTip("Drag the handles on the curve, or the dashed line across the history. Double-click a handle: default.");
    }
    QSize sizeHint() const override { return {560, 240}; }
    float get(uint32_t id) const { return m_proc->params().get(id); }
    void set(uint32_t id, float plain, bool commit = false) {
        if (auto* d = m_proc->params().descById(id)) App::instance()->setProcessorParam(m_owner, id, d->toNormalized(plain), commit);
    }
    QPointF shownPx(int i) const {   // where handle i is drawn: one out of range waits at the edge
        const QRectF r = shapeRect().adjusted(6, 6, -6, -6);
        const QPointF p = toPx(handles[i].at());
        return {std::clamp(p.x(), r.left(), r.right()), std::clamp(p.y(), r.top(), r.bottom())};
    }
    void tick() {   // the editor's timer: take what the audio thread posted into the history
        constexpr auto rlx = std::memory_order_relaxed;
        constexpr float kNone = -1000;   // below any level: nothing was posted since the last look
        const float in = m_meter.in.exchange(kNone, rlx), out = m_meter.out.exchange(kNone, rlx), gr = m_meter.gr.exchange(0, rlx);
        if (in > kNone) {
            m_idle = 0;
            m_last = {in, out, gr};
            m_dot = {m_meter.x.load(rlx), m_meter.y.load(rlx)};
        } else if (++m_idle > 8) {   // no longer processed (bypassed, removed from the graph): fall silent
            m_last = {DynamicsMeter::kFloor, DynamicsMeter::kFloor, 0};
            m_dot = {DynamicsMeter::kFloor, DynamicsMeter::kFloor};
        }
        m_hist.push_front(m_last);
        if (m_hist.size() > 600) m_hist.pop_back();
        update();
    }

protected:
    static constexpr int kMeterW = 44, kColW = 2;   // gain meter width; history pixels per tick
    QRectF shapeRect() const { return QRectF(0, 0, height(), height()); }
    QRectF histRect() const { return QRectF(height() + 1, 0, width() - height() - kMeterW - 2, height()); }
    QRectF meterRect() const { return QRectF(width() - kMeterW, 0, kMeterW, height()); }
    QPointF toPx(QPointF v) const {
        QRectF r = shapeRect();
        return {r.left() + (v.x() - x0) / (x1 - x0) * r.width(), r.bottom() - (v.y() - y0) / (y1 - y0) * r.height()};
    }
    QPointF toUnits(QPointF px) const {
        QRectF r = shapeRect();
        return {x0 + (px.x() - r.left()) / r.width() * (x1 - x0), y0 + (r.bottom() - px.y()) / r.height() * (y1 - y0)};
    }
    double yPx(float v) const { return std::clamp(toPx({x0, v}).y(), -2.0, height() + 2.0); }
    float level(float db) const { return linear ? dbToGain(db) : db; }   // a level from the meter in the shape's units
    int handleAt(QPointF pos) const {
        if (!shapeRect().contains(pos)) return -1;
        for (int i = 0; i < (int)handles.size(); ++i)
            if (QLineF(shownPx(i), pos).length() <= 9) return i;
        return -1;
    }
    bool onLine(QPointF pos) const { return line >= 0 && histRect().contains(pos) && std::abs(pos.y() - yPx(handles[line].at().y())) <= 5; }
    QString valueText(const Handle& h) const {
        QStringList s;
        for (uint32_t id : h.ids) s << QString::fromStdString(m_proc->params().descById(id)->name) + " " + m_proc->formatValue(id);
        return s.join("  ·  ");
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setFont(theme::smallFont());
        QPainterPath frame;
        frame.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), theme::radius(), theme::radius());
        p.fillPath(frame, theme::surface());
        p.setClipPath(frame);
        paintShape(p);
        paintHistory(p);
        paintMeter(p);
        p.setClipping(false);
        p.setPen(theme::border());
        p.drawLine(QPointF(histRect().left() - 0.5, 0), QPointF(histRect().left() - 0.5, height()));
        p.drawLine(QPointF(meterRect().left() - 0.5, 0), QPointF(meterRect().left() - 0.5, height()));
        p.setBrush(Qt::NoBrush);
        p.drawPath(frame);
    }
    void gridY(QPainter& p, const QRectF& r, bool labels) {   // level lines, shared by the shape and the history
        for (float v = std::ceil((y0 + 0.01f) / yStep) * yStep; v < y1 - 0.01f; v += yStep) {
            const double y = std::round(yPx(v)) + 0.5;
            p.setPen(v == 0 ? theme::gridStrong() : theme::grid());
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
            if (labels) {
                p.setPen(theme::textDim());
                p.drawText(QPointF(r.left() + 3, y - 3), QString::number(v));
            }
        }
    }
    void paintShape(QPainter& p) {
        const QRectF r = shapeRect();
        p.save();
        p.setClipRect(r, Qt::IntersectClip);
        if (band) {
            auto [a, b] = band();
            const double xa = toPx({a, 0}).x(), xb = toPx({b, 0}).x();
            p.fillRect(
                QRectF(QPointF(std::min(xa, xb), r.top()), QPointF(std::max(xa, xb), r.bottom())), theme::withAlpha(theme::accent(), 16));
        }
        for (float v = std::ceil((x0 + 0.01f) / xStep) * xStep; v < x1 - 0.01f; v += xStep) {
            const double x = std::round(toPx({v, 0}).x()) + 0.5;
            p.setPen(v == 0 && xUnit.isEmpty() ? theme::gridStrong() : theme::grid());
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
            p.setPen(theme::textDim());
            p.drawText(QPointF(x + 3, r.bottom() - 4), QString::number(v));
        }
        p.drawText(QRectF(r.left(), r.top() + 3, r.width() - 5, 14), Qt::AlignRight, xUnit);
        gridY(p, r, true);
        auto trace = [&](const std::function<float(float)>& f) {
            QPolygonF poly;
            for (double x = r.left(); x <= r.right() + 1; x += 1)
                poly << QPointF(x, std::clamp(toPx({0, f(toUnits({x, 0}).x())}).y(), r.top() - 2, r.bottom() + 2));
            return poly;
        };
        if (ref) {
            p.setPen(QPen(theme::withAlpha(theme::textDim(), 150), 1, Qt::DashLine));
            p.drawPolyline(trace(ref));
        }
        QPolygonF c = trace(curve);
        QPolygonF fill = c;
        fill << r.bottomRight() + QPointF(1, 2) << r.bottomLeft() + QPointF(0, 2);
        QLinearGradient g(r.topLeft(), r.bottomLeft());
        g.setColorAt(0, theme::withAlpha(theme::accent(), 60));
        g.setColorAt(1, theme::withAlpha(theme::accent(), 0));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawPolygon(fill);
        p.setPen(QPen(theme::accent(), 2));
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(c);
        if (level(m_dot.x()) > x0) {   // where the signal sits on the curve right now
            QPointF d = toPx({level(m_dot.x()), level(m_dot.y())});
            d = {std::clamp(d.x(), r.left(), r.right()), std::clamp(d.y(), r.top(), r.bottom())};
            p.setPen(Qt::NoPen);
            p.setBrush(theme::withAlpha(theme::playhead(), 70));
            p.drawEllipse(d, 8, 8);
            p.setPen(QPen(theme::surface(), 1.5));   // keeps it apart from the curve where the playhead colour is close to accent
            p.setBrush(theme::playhead());
            p.drawEllipse(d, 4, 4);
        }
        for (int i = 0; i < (int)handles.size(); ++i) {
            const bool hot = i == m_drag || (i == m_hover && m_drag < 0) || (i == line && (m_hoverLine || m_lineDrag));
            const QPointF h = shownPx(i);
            p.setPen(QPen(theme::accent(), 2));
            p.setBrush(hot ? theme::accent() : theme::surface());
            p.drawEllipse(h, 5.5, 5.5);
            const QString text = hot ? valueText(handles[i]) : handles[i].name;
            QRectF box = p.fontMetrics().boundingRect(text).adjusted(-5, -2, 5, 2);
            box.moveTopLeft(h + QPointF(10, r.bottom() - h.y() > 40 ? 6 : -box.height() - 6));   // below, clear of the curve
            if (box.right() > r.right() - 2) box.moveRight(h.x() - 10);
            if (box.left() < r.left() + 2) box.moveLeft(r.left() + 2);
            if (hot) {
                p.setPen(theme::border());
                p.setBrush(theme::withAlpha(theme::panel(), 235));
                p.drawRoundedRect(box, 3, 3);
            }
            p.setPen(hot ? theme::text() : theme::textDim());
            p.drawText(box, Qt::AlignCenter, text);
        }
        p.restore();
    }
    void paintHistory(QPainter& p) {
        const QRectF r = histRect();
        p.save();
        p.setClipRect(r, Qt::IntersectClip);
        gridY(p, r, false);
        const int n = std::min<int>((int)m_hist.size(), (int)r.width() / kColW + 2);
        QPolygonF in, out, gr;   // reduction hangs from the top (a transient processor's gain shows as out vs in instead)
        for (int i = 0; i < n; ++i) {
            const double x = r.right() - i * kColW;
            in << QPointF(x, yPx(level(m_hist[i][0])));
            out << QPointF(x, yPx(level(m_hist[i][1])));
            gr << QPointF(x, r.top() - std::clamp(m_hist[i][2], -grRange, 0.f) / grRange * r.height());
        }
        if (n > 1) {
            const QPointF lastX(r.right() - (n - 1) * kColW, 0), firstX(r.right(), 0);
            QPolygonF inFill = in;
            inFill << QPointF(lastX.x(), r.bottom() + 1) << QPointF(firstX.x(), r.bottom() + 1);
            p.setPen(QPen(theme::withAlpha(theme::accent2(), 170), 1));
            p.setBrush(theme::withAlpha(theme::accent2(), 50));
            p.drawPolygon(inFill);
            if (!bipolar) {
                QPolygonF grFill = gr;
                grFill << QPointF(lastX.x(), r.top()) << QPointF(firstX.x(), r.top());
                p.setPen(Qt::NoPen);
                p.setBrush(theme::withAlpha(theme::red(), 60));
                p.drawPolygon(grFill);
                p.setPen(QPen(theme::red(), 1));
                p.drawPolyline(gr);
            }
            p.setPen(QPen(theme::accent(), 1.5));
            p.drawPolyline(out);
        }
        if (line >= 0) {
            const bool hot = m_hoverLine || m_lineDrag;
            const double y = std::round(yPx(handles[line].at().y())) + 0.5;
            p.setPen(QPen(theme::withAlpha(theme::accent(), hot ? 255 : 170), hot ? 1.5 : 1, Qt::DashLine));
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
            const QString t = m_proc->formatValue(handles[line].ids[0]);
            p.setPen(hot ? theme::text() : theme::textDim());
            p.drawText(QRectF(r.left() + 6, y - 16, r.width(), 14), Qt::AlignLeft | Qt::AlignBottom, t);
        }
        double lx = r.left() + 8;   // legend
        const std::pair<QString, QColor> legend[] = {{"in", theme::accent2()}, {"out", theme::accent()}, {"reduction", theme::red()}};
        for (auto& [name, col] : legend) {
            if (bipolar && name == "reduction") break;
            p.fillRect(QRectF(lx, 9, 10, 2), col);
            p.setPen(theme::textDim());
            p.drawText(QPointF(lx + 14, 14), name);
            lx += 22 + p.fontMetrics().horizontalAdvance(name);
        }
        p.restore();
    }
    void paintMeter(QPainter& p) {
        const QRectF r = meterRect();
        const QRectF bar(r.right() - 15, r.top() + 22, 8, r.height() - 44);
        const double zero = bipolar ? bar.center().y() : bar.top(), scale = (bipolar ? bar.height() / 2 : bar.height()) / grRange;
        p.setPen(theme::textDim());
        p.drawText(QRectF(r.left(), r.top() + 4, r.width(), 14), Qt::AlignCenter, bipolar ? "GAIN" : "GR");
        for (int k = 0; k <= 4; ++k) {
            const float v = bipolar ? grRange * (k - 2) / 2 : -grRange * k / 4;
            const double y = zero - v * scale;
            p.drawText(
                QRectF(r.left(), y - 7, bar.left() - r.left() - 4, 14), Qt::AlignRight | Qt::AlignVCenter, QString::number(std::abs(v)));
        }
        p.fillRect(bar, theme::withAlpha(theme::textDim(), 40));
        const float g = m_hist.empty() ? 0.f : std::clamp(m_hist.front()[2], -grRange, grRange);
        const double y = zero - g * scale;
        p.fillRect(
            QRectF(QPointF(bar.left(), std::min(zero, y)), QPointF(bar.right(), std::max(zero, y))), g > 0 ? theme::green() : theme::red());
        p.setPen(theme::text());
        p.drawText(
            QRectF(r.left(), r.bottom() - 18, r.width(), 14), Qt::AlignCenter, QString::number(std::abs(g) < 0.05f ? 0.f : g, 'f', 1));
    }

    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        m_drag = handleAt(e->position());
        m_lineDrag = m_drag < 0 && onLine(e->position());
        if (m_lineDrag) m_drag = line;
        m_moved = false;
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (m_drag >= 0) {
            const Handle& h = handles[m_drag];
            QPointF u = toUnits(e->position());
            if (m_lineDrag) {   // the line moves the handle along the diagonal: the level changes, nothing else
                const QPointF a = h.at();
                u = linear ? a * (u.y() / std::max(a.y(), 1e-4)) : a + QPointF(u.y() - a.y(), u.y() - a.y());
            }
            h.drag(u);
            m_moved = true;
            update();
            return;
        }
        const int hv = handleAt(e->position());
        const bool ln = hv < 0 && onLine(e->position());
        if (hv != m_hover || ln != m_hoverLine) {
            m_hover = hv;
            m_hoverLine = ln;
            setCursor(hv >= 0 ? Qt::SizeAllCursor : ln ? Qt::SizeVerCursor : Qt::ArrowCursor);
            update();
        }
    }
    void mouseReleaseEvent(QMouseEvent*) override {
        if (m_drag >= 0 && m_moved)
            for (uint32_t id : handles[m_drag].ids)
                App::instance()->setProcessorParam(m_owner, id, m_proc->params().getNormalized(id), true);
        m_drag = -1;
        m_lineDrag = false;
        update();
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        const int i = handleAt(e->position());
        if (i >= 0)
            for (uint32_t id : handles[i].ids) set(id, m_proc->params().descById(id)->def, true);
    }
    void wheelEvent(QWheelEvent* e) override {
        if (!wheelId || !shapeRect().contains(e->position())) return e->ignore();
        set(wheelId, get(wheelId) + (e->angleDelta().y() > 0 ? wheelStep : -wheelStep), true);
        update();
    }
    void leaveEvent(QEvent*) override {
        m_hover = -1;
        m_hoverLine = false;
        update();
    }

private:
    IProcessor* m_proc;
    DynamicsMeter& m_meter;
    Id m_owner;
    std::deque<std::array<float, 3>> m_hist;   // newest first: input, output, gain change
    std::array<float, 3> m_last{DynamicsMeter::kFloor, DynamicsMeter::kFloor, 0};
    QPointF m_dot{DynamicsMeter::kFloor, DynamicsMeter::kFloor};
    int m_idle = 0, m_drag = -1, m_hover = -1;
    bool m_lineDrag = false, m_hoverLine = false, m_moved = false;
};

// The editor around a view: a strip with the effect's name and the values the display drags, the display, knobs for the rest
QWidget* dynamicsEditor(IProcessor* p, DynamicsMeter& meter, QWidget* parent, const std::function<void(DynamicsView*)>& setup) {
    auto* w = new QWidget(parent);
    auto* lay = new QVBoxLayout(w);
    lay->setContentsMargins(8, 6, 8, 4);
    lay->setSpacing(4);
    auto* view = new DynamicsView(p, meter, w);
    setup(view);
    std::vector<uint32_t> dragged;
    for (auto& h : view->handles) dragged.insert(dragged.end(), h.ids.begin(), h.ids.end());
    auto* head = new QHBoxLayout();
    head->addWidget(theme::dimLabel(p->displayName().toUpper(), w, "font-size:8pt; font-weight:bold; letter-spacing:1px; color:@text@;"));
    head->addStretch(1);
    auto* values = theme::dimLabel(QString(), w, "font-size:8pt;");
    head->addWidget(values);
    lay->addLayout(head);
    lay->addWidget(view, 1);
    auto* knobs = new GenericParamPanel(
        *App::instance(), ownerOf(p), p,
        [dragged](const ParamDesc& d) { return std::find(dragged.begin(), dragged.end(), d.id) == dragged.end(); }, 8, w);
    lay->addWidget(knobs);
    auto* t = new QTimer(w);
    QObject::connect(t, &QTimer::timeout, w, [view, knobs, values, p, dragged] {
        view->tick();
        knobs->sync();
        QStringList s;
        for (uint32_t id : dragged) s << QString::fromStdString(p->params().descById(id)->name) + " " + p->formatValue(id);
        values->setText(s.join("    "));
    });
    t->start(33);
    return w;
}

// ---------------------------------------------------------------- compressor
QWidget* compressorEditor(IProcessor* proc, QWidget* parent) {
    auto* c = static_cast<Compressor*>(proc);
    return dynamicsEditor(proc, c->meter, parent, [](DynamicsView* v) {
        using C = Compressor;
        const float xr = v->x1 - 3;   // the ratio handle rides the curve near the right edge
        v->curve = [v](float x) { return x + C::gainDb(x - v->get(C::pThreshold), v->get(C::pRatio), v->get(C::pKnee)); };
        v->ref = [](float x) { return x; };
        v->band = [v] {
            const float th = v->get(C::pThreshold), k = v->get(C::pKnee) / 2;
            return std::pair{th - k, th + k};
        };
        v->handles = {{"threshold", {C::pThreshold}, [v] { return QPointF(v->get(C::pThreshold), v->get(C::pThreshold)); },
                          [v](QPointF u) { v->set(C::pThreshold, (u.x() + u.y()) / 2); }},
            {"ratio", {C::pRatio}, [v, xr] { return QPointF(xr, v->curve(xr)); },
                [v, xr](QPointF u) {
                    const float th = v->get(C::pThreshold);
                    v->set(C::pRatio, u.y() > th + 0.05f ? (xr - th) / (u.y() - th) : 1000.f);
                }}};
        v->wheelId = C::pKnee;
    });
}

// ---------------------------------------------------------------- limiter
QWidget* limiterEditor(IProcessor* proc, QWidget* parent) {
    auto* l = static_cast<Limiter*>(proc);
    return dynamicsEditor(proc, l->meter, parent, [](DynamicsView* v) {
        using L = Limiter;
        v->x0 = v->y0 = -30, v->xStep = v->yStep = 6;
        v->grRange = 12;
        v->curve = [v](float x) { return std::min(x + v->get(L::pInputGain), v->get(L::pCeiling)); };
        v->ref = [](float x) { return x; };
        // the knee: up / down is the ceiling, left / right the input gain (further left = louder)
        v->handles = {{"ceiling", {L::pCeiling, L::pInputGain},
            [v] { return QPointF(v->get(L::pCeiling) - v->get(L::pInputGain), v->get(L::pCeiling)); },
            [v](QPointF u) {
                const float ceil = std::clamp((float)u.y(), -24.f, 0.f);
                v->set(L::pCeiling, ceil);
                v->set(L::pInputGain, ceil - u.x());
            }}};
    });
}

// ---------------------------------------------------------------- gate
QWidget* gateEditor(IProcessor* proc, QWidget* parent) {
    auto* g = static_cast<Gate*>(proc);
    return dynamicsEditor(proc, g->meter, parent, [](DynamicsView* v) {
        using G = Gate;
        v->x0 = v->y0 = -96, v->xStep = v->yStep = 24;
        v->grRange = 48;
        v->curve = [v](float x) { return x >= v->get(G::pThreshold) ? x : x + v->get(G::pRange); };
        v->ref = [](float x) { return x; };
        v->band = [v] {   // hysteresis: once open, the gate closes only below the lower edge
            const float th = v->get(G::pThreshold);
            return std::pair{th - v->get(G::pHysteresis), th};
        };
        v->handles = {{"threshold", {G::pThreshold}, [v] { return QPointF(v->get(G::pThreshold), v->get(G::pThreshold)); },
                          [v](QPointF u) { v->set(G::pThreshold, (u.x() + u.y()) / 2); }},
            {"range", {G::pRange},
                [v] {
                    const float x = v->get(G::pThreshold) - 12;
                    return QPointF(x, x + v->get(G::pRange));
                },
                [v](QPointF u) {   // down to the floor of the view = fully closed
                    v->set(G::pRange, u.y() <= v->y0 + 3 ? -96.f : (float)u.y() - (v->get(G::pThreshold) - 12));
                }}};
        v->wheelId = G::pHysteresis, v->wheelStep = 0.5f;
    });
}

// ---------------------------------------------------------------- expander
QWidget* expanderEditor(IProcessor* proc, QWidget* parent) {
    auto* e = static_cast<Expander*>(proc);
    return dynamicsEditor(proc, e->meter, parent, [](DynamicsView* v) {
        using E = Expander;
        v->x0 = v->y0 = -96, v->xStep = v->yStep = 24;
        v->grRange = 48;
        v->curve = [v](float x) {
            return x + E::gainDb(v->get(E::pThreshold) - x, v->get(E::pRatio), v->get(E::pKnee), v->get(E::pRange));
        };
        v->ref = [](float x) { return x; };
        v->band = [v] { return std::pair{v->get(E::pThreshold) - v->get(E::pKnee), v->get(E::pThreshold)}; };
        // the slope below the threshold, as a lever pivoting on it: where it points is the ratio (range stays a knob: its floor
        // is mostly below the view)
        auto lever = [v] {
            const float th = v->get(E::pThreshold), r = v->get(E::pRatio), len = 24 / std::hypot(1.f, r);
            return QPointF(th - len, th - len * r);
        };
        v->handles = {{"threshold", {E::pThreshold}, [v] { return QPointF(v->get(E::pThreshold), v->get(E::pThreshold)); },
                          [v](QPointF u) { v->set(E::pThreshold, (u.x() + u.y()) / 2); }},
            {"ratio", {E::pRatio}, lever, [v](QPointF u) {
                 const float th = v->get(E::pThreshold);
                 v->set(E::pRatio, (th - u.y()) / std::max(th - (float)u.x(), 0.1f));
             }}};
        v->wheelId = E::pKnee;
    });
}

// ---------------------------------------------------------------- clipper
QWidget* clipperEditor(IProcessor* proc, QWidget* parent) {
    auto* c = static_cast<Clipper*>(proc);
    return dynamicsEditor(proc, c->meter, parent, [](DynamicsView* v) {
        using C = Clipper;
        // linear amplitude in and out: the clip shape as it bends the waveform
        v->linear = true;
        v->x0 = v->y0 = 0, v->x1 = v->y1 = 1.5f, v->xStep = v->yStep = 0.25f;
        v->grRange = 12;
        auto th = [v] { return dbToGain(v->get(C::pThreshold)); };
        auto gain = [v] { return dbToGain(v->get(C::pInput)); };
        v->curve = [v, th, gain](float x) { return C::shape(x * gain(), th(), v->get(C::pSoftness)); };
        v->ref = [](float x) { return x; };
        auto knee = [v, th] { return th() * (1 - v->get(C::pSoftness)); };   // where the soft bend begins
        v->band = [th, gain, knee] { return std::pair{knee() / gain(), th() / gain()}; };
        // like the limiter's knee: up / down is the threshold, left / right the input gain
        v->handles = {{"threshold", {C::pThreshold, C::pInput}, [th, gain] { return QPointF(th() / gain(), th()); },
                          [v](QPointF u) {
                              const float t = std::clamp(gainToDb((float)u.y()), -24.f, 0.f);
                              v->set(C::pThreshold, t);
                              v->set(C::pInput, t - gainToDb((float)u.x()));
                          }},
            {"softness", {C::pSoftness}, [gain, knee] { return QPointF(knee() / gain(), knee()); },
                [v, th, gain](QPointF u) { v->set(C::pSoftness, 1 - (float)u.x() * gain() / th()); }}};
        v->wheelId = C::pSoftness, v->wheelStep = 0.05f;
    });
}

// ---------------------------------------------------------------- transient processor
// A drum hit through a private TransientProcessor set like the real one: the before / after the shape draws
struct HitShape {
    static constexpr int kRate = 8000, kLen = kRate * 3 / 10, kStart = kRate / 50;   // 300 ms, the hit at 20 ms
    std::vector<float> dry, wet;   // dB per sample
    int ta = 0, ts = 0, attackEnd = 0;   // where the attack / sustain handles sit, where the attack boost ends
    float slopeA = 1, slopeS = 1;   // dB the knobs' full turn moves those points
    std::array<float, 3> key{NAN, NAN, NAN};
    static float ms(int i) { return i * 1000.f / kRate; }
    static std::vector<float> run(float att, float sus, float speed) {
        TransientProcessor tp;
        tp.params().set(TransientProcessor::pAttack, att);
        tp.params().set(TransientProcessor::pSustain, sus);
        tp.params().set(TransientProcessor::pSpeed, speed);
        tp.prepare(kRate, kLen);
        std::vector<float> l(kLen);
        for (int i = kStart; i < kLen; ++i) {
            const float t = float(i - kStart) / kRate;
            l[i] = std::min(1.f, t / 0.0005f) * std::exp(-t / 0.06f);
        }
        std::vector<float> r = l;
        static const MidiEventList midi{};
        tp.process(l.data(), r.data(), kLen, midi, ProcessContext());
        for (float& x : l) x = gainToDb(x);
        return l;
    }
    HitShape* update(float att, float sus, float speed) {
        if (key == std::array{att, sus, speed}) return this;
        if (speed != key[2]) {   // where the handles sit and how far they reach depends on the speed only
            dry = run(0, 0, speed);
            const auto a = run(1, 0, speed), s = run(0, 1, speed);
            ta = kStart;
            for (int i = kStart; i < kStart + kRate / 10; ++i)
                if (a[i] - dry[i] > a[ta] - dry[ta]) ta = i;
            for (attackEnd = ta; attackEnd < kLen - 1 && a[attackEnd] - dry[attackEnd] > 0.5f;) ++attackEnd;
            ts = kStart + kRate * 14 / 100;
            slopeA = std::max(a[ta] - dry[ta], 0.5f);
            slopeS = std::max(s[ts] - dry[ts], 0.5f);
        }
        wet = run(att, sus, speed);
        key = {att, sus, speed};
        return this;
    }
};
QWidget* transientEditor(IProcessor* proc, QWidget* parent) {
    auto* tp = static_cast<TransientProcessor*>(proc);
    return dynamicsEditor(proc, tp->meter, parent, [](DynamicsView* v) {
        using T = TransientProcessor;
        auto hs = std::make_shared<HitShape>();
        auto shape = [v, hs] { return hs->update(v->get(T::pAttack), v->get(T::pSustain), v->get(T::pSpeed)); };
        auto at = [](const std::vector<float>& db, float ms) {
            return db[std::clamp((int)(ms * HitShape::kRate / 1000), 0, HitShape::kLen - 1)];
        };
        v->x0 = 0, v->x1 = HitShape::ms(HitShape::kLen), v->xStep = 50, v->xUnit = "ms";
        v->y0 = -48, v->y1 = 18;
        v->grRange = 18, v->bipolar = true, v->line = -1;
        v->curve = [shape, at](float ms) { return at(shape()->wet, ms); };
        v->ref = [shape, at](float ms) { return at(shape()->dry, ms); };
        v->band = [shape] { return std::pair{HitShape::ms(HitShape::kStart), HitShape::ms(shape()->attackEnd)}; };
        v->handles = {{"attack", {T::pAttack},
                          [shape] {
                              auto* s = shape();
                              return QPointF(HitShape::ms(s->ta), s->wet[s->ta]);
                          },
                          [v, shape](QPointF u) {
                              auto* s = shape();
                              v->set(T::pAttack, (u.y() - s->dry[s->ta]) / s->slopeA);
                          }},
            {"sustain", {T::pSustain},
                [shape] {
                    auto* s = shape();
                    return QPointF(HitShape::ms(s->ts), s->wet[s->ts]);
                },
                [v, shape](QPointF u) {
                    auto* s = shape();
                    v->set(T::pSustain, (u.y() - s->dry[s->ts]) / s->slopeS);
                }}};
    });
}

NativeEditorRegistration s_compressor({"native.compressor"}, compressorEditor);
NativeEditorRegistration s_limiter({"native.limiter"}, limiterEditor);
NativeEditorRegistration s_gate({"native.gate"}, gateEditor);
NativeEditorRegistration s_expander({"native.expander"}, expanderEditor);
NativeEditorRegistration s_clipper({"native.clipper"}, clipperEditor);
NativeEditorRegistration s_transient({"native.transient"}, transientEditor);

}   // namespace

// For the self-test: the display inside a dynamics editor and where its handle i shows on it (nullptr past the last handle)
QWidget* dynamicsHandle(QWidget* editor, int i, QPoint& pos) {
    auto* v = static_cast<DynamicsView*>(editor->findChild<QWidget*>("dynamicsView"));
    if (!v || i >= (int)v->handles.size()) return nullptr;
    pos = v->shownPx(i).toPoint();
    return v;
}

}   // namespace mt
