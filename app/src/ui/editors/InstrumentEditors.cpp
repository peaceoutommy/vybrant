// Visual editors for the synthesizers: Synth, Wavetable, FM Synth and Tone Generator. The envelopes, filter curve, oscillator
// and LFO shapes, wavetable and FM algorithm are drawn and dragged directly; knobs below hold the rest. While notes play the
// displays follow the newest voice (envelope stages, modulated cutoff and table position, operator levels), read from the
// instrument's atomics on a timer.
#include "ui/PluginWindow.h"
#include "ui/Theme.h"
#include "dsp/instruments/Instruments.h"
#include <QCheckBox>
#include <QGridLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace mt {
namespace {

constexpr auto kRelaxed = std::memory_order_relaxed;

Id ownerOf(IProcessor* p) {
    for (auto& [id, proc] : App::instance()->engine().registry().processors())
        if (proc.get() == p) return id;
    return 0;
}

// A painted display that edits its processor's parameters. Drags change values live and releasing commits one undo step per
// parameter moved; clicks (shape, type, bank, algorithm) commit at once.
class Display : public QWidget {
public:
    Display(IProcessor* p, QWidget* parent)
        : QWidget(parent)
        , m_p(p)
        , m_owner(ownerOf(p)) {
        setMouseTracking(true);
    }

protected:
    ParameterSet& ps() const { return m_p->params(); }
    float get(uint32_t id) const { return ps().get(id); }
    float norm(uint32_t id) const { return ps().getNormalized(id); }
    void drag(uint32_t id, double n) {
        n = clamp(n, 0.0, 1.0);
        if (std::fabs(n - norm(id)) < 1e-5) return;   // unchanged: no undo step for it
        App::instance()->setProcessorParam(m_owner, id, (float)n, false);
        if (std::find(m_moved.begin(), m_moved.end(), id) == m_moved.end()) m_moved.push_back(id);
        update();
    }
    void dragPlain(uint32_t id, double v) { drag(id, ps().descById(id)->toNormalized((float)v)); }
    void commit() {
        for (uint32_t id : m_moved) App::instance()->setProcessorParam(m_owner, id, norm(id), true);
        m_moved.clear();
    }
    void setNow(uint32_t id, float plain) {
        App::instance()->setProcessorParam(m_owner, id, ps().descById(id)->toNormalized(plain), true);
        update();
    }
    void frame(QPainter& p) const {
        p.setPen(QPen(theme::border(), 1));
        p.setBrush(theme::surface());
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), theme::radius(), theme::radius());
    }
    // the top line: what is shown on the left, a readout on the right (rightInset leaves room for tabs)
    void caption(QPainter& p, const QString& left, const QString& right = QString(), double rightInset = 0) const {
        p.setFont(theme::smallFont());
        QRectF r(8, 3, width() - 16 - rightInset, 16);
        p.setPen(theme::text());
        p.drawText(r, Qt::AlignLeft | Qt::AlignVCenter, left);
        p.setPen(theme::textDim());
        p.drawText(r, Qt::AlignRight | Qt::AlignVCenter, right);
    }
    // small tabs at the top right (filter type, wavetable bank; kSteps: previous / next)
    static inline const QStringList kSteps = {"\u2039", "\u203A"};
    std::vector<QRectF> tabs(const QStringList& names) const {
        QFontMetricsF fm(theme::smallFont());
        std::vector<QRectF> r(names.size());
        double x = width() - 5;
        for (int i = (int)names.size() - 1; i >= 0; --i) {
            double w = fm.horizontalAdvance(names[i]) + 10;
            x -= w;
            r[i] = QRectF(x, 4, w, 15);
            x -= 2;
        }
        return r;
    }
    void drawTabs(QPainter& p, const QStringList& names, int cur) const {
        p.setFont(theme::smallFont());
        auto r = tabs(names);
        for (int i = 0; i < (int)r.size(); ++i) {
            p.setPen(Qt::NoPen);
            p.setBrush(i == cur ? theme::withAlpha(theme::accent(), 60) : theme::withAlpha(theme::text(), 10));
            p.drawRoundedRect(r[i], theme::radiusFor(r[i].height()), theme::radiusFor(r[i].height()));
            p.setPen(i == cur ? theme::text() : theme::textDim());
            p.drawText(r[i], Qt::AlignCenter, names[i]);
        }
    }
    int tabAt(const QPointF& pt, const QStringList& names) const {
        auto r = tabs(names);
        for (int i = 0; i < (int)r.size(); ++i)
            if (r[i].adjusted(-1, -3, 1, 3).contains(pt)) return i;
        return -1;
    }
    void liveDot(QPainter& p, const QPointF& c) const {
        p.setPen(Qt::NoPen);
        p.setBrush(theme::withAlpha(theme::accent2(), 70));
        p.drawEllipse(c, 7, 7);
        p.setBrush(theme::accent2());
        p.drawEllipse(c, 3.5, 3.5);
    }
    IProcessor* m_p;
    Id m_owner;
    std::vector<uint32_t> m_moved;
};

// decay / release curve of dsp::ADSR (exp(-4t)), scaled to run exactly from 1 to 0 over the stage, and its inverse
const double kE4 = std::exp(-4.0);
double expShape(double t) { return (std::exp(-4 * t) - kE4) / (1 - kE4); }
double expTime(double s) { return -std::log(clamp(s, 0.0, 1.0) * (1 - kE4) + kE4) / 4; }

// ADSR envelope: drag the peak (attack time), the knee (decay time and sustain level) and the end (release time). Each time
// gets a lane of the width and sits on its knob's curve within it, so short times stay easy to set.
class EnvGraph : public Display {
public:
    using Ids = std::array<uint32_t, 4>;   // attack, decay, sustain, release; decay and sustain may be 0 (none, full)
    EnvGraph(IProcessor* p, Ids ids, const EnvTap* tap, QWidget* parent)
        : Display(p, parent)
        , m_ids(ids)
        , m_tap(tap) {
        setMinimumSize(180, 90);
        setToolTip(hasKnee() ? "Drag the points: the peak sets the attack, the knee decay and sustain, the end the release"
                             : "Drag the points: the peak sets the attack, the end the release");
    }
    void follow(Ids ids, const EnvTap* tap) {
        m_ids = ids;
        m_tap = tap;
        update();
    }
    QSize sizeHint() const override { return {260, 110}; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        frame(p);
        const Geo g = geo();
        const QRectF& r = g.r;
        const float S = sustain();
        p.setPen(QPen(theme::grid(), 1));
        for (int k = 1; k < 4; ++k)
            p.drawLine(QPointF(r.left(), r.top() + r.height() * k / 4), QPointF(r.right(), r.top() + r.height() * k / 4));
        p.setPen(QPen(theme::gridStrong(), 1, Qt::DotLine));
        for (double x : {g.xA, g.xD, g.xS}) p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.setPen(QPen(theme::gridStrong(), 1));
        p.drawLine(r.bottomLeft(), r.bottomRight());
        QPainterPath path(QPointF(r.left(), r.bottom()));
        path.lineTo(g.xA, r.top());
        for (int i = 1; i <= 32; ++i) path.lineTo(g.xA + i / 32.0 * (g.xD - g.xA), yOf(g, S + (1 - S) * expShape(i / 32.0)));
        path.lineTo(g.xS, yOf(g, S));
        for (int i = 1; i <= 32; ++i) path.lineTo(g.xS + i / 32.0 * (g.xR - g.xS), yOf(g, S * expShape(i / 32.0)));
        QLinearGradient fill(0, r.top(), 0, r.bottom());
        fill.setColorAt(0, theme::withAlpha(theme::accent(), 70));
        fill.setColorAt(1, theme::withAlpha(theme::accent(), 8));
        p.fillPath(path, fill);
        p.strokePath(path, QPen(theme::accent(), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (int st = m_tap ? m_tap->stage.load(kRelaxed) : 0; st != dsp::ADSR::Idle) {   // where the newest voice is now
            const float v = m_tap->value.load(kRelaxed), from = m_tap->from.load(kRelaxed);
            double x = st == dsp::ADSR::Attack ? r.left() + v * (g.xA - r.left())
                : st == dsp::ADSR::Decay       ? g.xA + expTime(S < 1 ? (v - S) / (1 - S) : 0) * (g.xD - g.xA)
                : st == dsp::ADSR::Sustain     ? (g.xD + g.xS) / 2
                                               : g.xS + expTime(from > 0 ? v / from : 0) * (g.xR - g.xS);
            p.setPen(QPen(theme::withAlpha(theme::accent2(), 120), 1, Qt::DashLine));
            p.drawLine(QPointF(x, yOf(g, v)), QPointF(x, r.bottom()));
            liveDot(p, QPointF(x, yOf(g, v)));
        }
        const auto h = handles(g);
        for (int i = 0; i < 3; ++i) {
            if (!hasKnee() && i == 1) continue;
            const bool hot = i == m_drag || (m_drag < 0 && i == m_hover);
            p.setPen(QPen(theme::accent(), 1.6));
            p.setBrush(hot ? theme::accent() : theme::surface());
            p.drawEllipse(h[i], hot ? 5.5 : 4.5, hot ? 5.5 : 4.5);
        }
        // readout: the values the hovered / dragged point sets in full colour
        const int on = m_drag >= 0 ? m_drag : m_hover;
        struct Item {
            QString text;
            int point;
        };
        std::vector<Item> items = {{"A " + m_p->formatValue(m_ids[0]), 0}};
        if (m_ids[1]) items.push_back({"D " + m_p->formatValue(m_ids[1]), 1});
        if (m_ids[2]) items.push_back({QString("S %1%").arg(std::lround(S * 100)), 1});
        items.push_back({"R " + m_p->formatValue(m_ids[3]), 2});
        p.setFont(theme::smallFont());
        double x = 8;
        for (auto& it : items) {
            p.setPen(it.point == on ? theme::text() : theme::textDim());
            p.drawText(QPointF(x, 15), it.text);
            x += p.fontMetrics().horizontalAdvance(it.text) + 12;
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        m_drag = nearest(e->position(), 1e9);   // the nearest point follows, wherever the drag starts
        m_press = e->position();
        for (int i = 0; i < 4; ++i) m_start[i] = m_ids[i] ? norm(m_ids[i]) : 0.f;
        update();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (m_drag < 0) {
            const int h = nearest(e->position(), 14);
            if (h != m_hover) {
                m_hover = h;
                update();
            }
            setCursor(h >= 0 ? Qt::SizeAllCursor : Qt::ArrowCursor);
            return;
        }
        const Geo g = geo();
        const QPointF d = e->position() - m_press;
        if (m_drag == 0) drag(m_ids[0], m_start[0] + d.x() / g.lane);
        else if (m_drag == 2) drag(m_ids[3], m_start[3] + d.x() / g.lane);
        else {
            if (m_ids[1]) drag(m_ids[1], m_start[1] + d.x() / g.lane);
            if (m_ids[2]) drag(m_ids[2], m_start[2] - d.y() / g.r.height());   // sustain is linear 0..1
        }
    }
    void mouseReleaseEvent(QMouseEvent*) override {
        commit();
        m_drag = -1;
        update();
    }
    void leaveEvent(QEvent*) override {
        m_hover = -1;
        update();
    }

private:
    struct Geo {
        QRectF r;
        double lane = 0, xA = 0, xD = 0, xS = 0, xR = 0;
    };
    Geo geo() const {
        Geo g;
        g.r = QRectF(rect()).adjusted(10, 24, -10, -8);
        const double hold = g.r.width() * 0.14;
        g.lane = (g.r.width() - hold) / (m_ids[1] ? 3 : 2);
        g.xA = g.r.left() + norm(m_ids[0]) * g.lane;
        g.xD = g.xA + (m_ids[1] ? norm(m_ids[1]) * g.lane : 0);
        g.xS = g.xD + hold;
        g.xR = g.xS + norm(m_ids[3]) * g.lane;
        return g;
    }
    bool hasKnee() const { return m_ids[1] || m_ids[2]; }
    float sustain() const { return m_ids[2] ? get(m_ids[2]) : 1.f; }
    double yOf(const Geo& g, double level) const { return g.r.bottom() - level * g.r.height(); }
    std::array<QPointF, 3> handles(const Geo& g) const {
        return {QPointF(g.xA, g.r.top()), QPointF(g.xD, yOf(g, sustain())), QPointF(g.xR, g.r.bottom())};
    }
    int nearest(const QPointF& pt, double within) const {
        const auto h = handles(geo());
        int best = -1;
        for (int i = 0; i < 3; ++i) {
            const double d = QLineF(pt, h[i]).length();
            if ((i != 1 || hasKnee()) && d < within) {
                within = d;
                best = i;
            }
        }
        return best;
    }
    Ids m_ids;
    const EnvTap* m_tap;
    int m_hover = -1, m_drag = -1;
    QPointF m_press;
    std::array<float, 4> m_start{};
};

// Filter response: drag anywhere to move the cutoff (sideways) and resonance (up / down); the tabs pick the type. While notes
// play a second, thinner curve shows where the envelope, LFO and key tracking have taken the cutoff.
class FilterGraph : public Display {
public:
    FilterGraph(IProcessor* p, uint32_t type, uint32_t cutoff, uint32_t reso, const std::atomic<float>* live, QWidget* parent)
        : Display(p, parent)
        , m_type(type)
        , m_cutoff(cutoff)
        , m_reso(reso)
        , m_live(live) {
        setMinimumSize(200, 90);
        setToolTip("Drag sideways for the cutoff, up and down for the resonance; the tabs pick the filter type");
    }
    QSize sizeHint() const override { return {280, 110}; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        frame(p);
        const QRectF a = area();
        p.setFont(theme::smallFont());
        for (double hz : {50., 100., 200., 500., 1000., 2000., 5000., 10000.}) {
            const bool decade = hz == 100 || hz == 1000 || hz == 10000;
            p.setPen(decade ? theme::gridStrong() : theme::grid());
            p.drawLine(QPointF(xOf(hz), a.top()), QPointF(xOf(hz), a.bottom()));
            if (decade) {
                p.setPen(theme::textDim());
                p.drawText(QPointF(xOf(hz) + 3, a.bottom() - 4), hz >= 1000 ? QString("%1k").arg(hz / 1000) : QString::number(hz));
            }
        }
        p.setPen(theme::gridStrong());
        p.drawLine(QPointF(a.left(), yOf(0)), QPointF(a.right(), yOf(0)));
        p.setClipRect(a);
        if (const float live = m_live ? m_live->load(kRelaxed) : 0.f; live > 0) {
            p.setPen(QPen(theme::accent2(), 1.3));
            p.drawPolyline(curve(shown(live)));
        }
        const QPolygonF c = curve(shown(get(m_cutoff)));
        QPainterPath path;
        path.addPolygon(c);
        path.lineTo(a.bottomRight() + QPointF(0, 2));
        path.lineTo(a.bottomLeft() + QPointF(0, 2));
        QLinearGradient fill(0, a.top(), 0, a.bottom());
        fill.setColorAt(0, theme::withAlpha(theme::accent(), 60));
        fill.setColorAt(1, theme::withAlpha(theme::accent(), 6));
        p.fillPath(path, fill);
        p.setPen(QPen(theme::accent(), 1.6));
        p.drawPolyline(c);
        p.setClipping(false);
        const QPointF h(xOf(shown(get(m_cutoff))), clamp(yOf(-20 * std::log10(damping())), a.top() + 5, a.bottom() - 5));
        p.setPen(QPen(theme::accent(), 1.6));
        p.setBrush(m_dragging ? theme::accent() : theme::surface());
        p.drawEllipse(h, 5, 5);
        caption(p, m_p->formatValue(m_cutoff), QString());
        p.setPen(theme::textDim());
        p.drawText(QPointF(8 + p.fontMetrics().horizontalAdvance(m_p->formatValue(m_cutoff)) + 10, 15),
            QString("res %1").arg(get(m_reso), 0, 'f', 2));
        drawTabs(p, kTypes, ps().getInt(m_type));
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        if (int t = tabAt(e->position(), kTypes); t >= 0) return setNow(m_type, (float)t);
        m_dragging = true;
        m_press = e->position();
        m_start = QPointF(xOf(get(m_cutoff)), yOf(-20 * std::log10(damping())));
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!m_dragging) return setCursor(tabAt(e->position(), kTypes) >= 0 ? Qt::PointingHandCursor : Qt::SizeAllCursor);
        const QPointF at = m_start + e->position() - m_press;
        dragPlain(m_cutoff, hzOf(at.x()));
        dragPlain(m_reso, 1 - std::pow(10.0, -dbOf(at.y()) / 20) / 2);   // the peak's height back to the SVF's damping
    }
    void mouseReleaseEvent(QMouseEvent*) override {
        commit();
        m_dragging = false;
        update();
    }

private:
    static inline const QStringList kTypes = {"LP", "HP", "BP", "Notch"};
    static constexpr double kTop = 30, kBottom = -42;   // dB shown
    QRectF area() const { return QRectF(rect()).adjusted(1, 22, -1, -1); }
    double xOf(double hz) const { return area().left() + std::log(hz / 20) / std::log(1000.0) * area().width(); }
    double hzOf(double x) const { return 20 * std::pow(1000.0, (x - area().left()) / area().width()); }
    double yOf(double db) const { return area().top() + (kTop - db) / (kTop - kBottom) * area().height(); }
    double dbOf(double y) const { return kTop - (y - area().top()) / area().height() * (kTop - kBottom); }
    double damping() const { return 2 - 2 * clamp(get(m_reso), 0.f, 0.98f); }   // as dsp::SVF::set
    double shown(double hz) const { return std::min(hz, App::instance()->engine().sampleRate() * 0.24); }   // the SVF's limit
    QPolygonF curve(double fc) const {   // 2-pole state-variable response
        const int type = ps().getInt(m_type);
        const double q = damping();
        QPolygonF c;
        for (double x = area().left(); x <= area().right() + 1; x += 2) {
            const double f = hzOf(x) / fc, re = 1 - f * f, den = std::hypot(re, f * q);
            const double m = type == 1 ? f * f / den : type == 2 ? f / den : type == 3 ? std::fabs(re) / den : 1 / den;
            c << QPointF(x, std::min(yOf(20 * std::log10(std::max(m, 1e-5))), area().bottom() + 2));
        }
        return c;
    }
    uint32_t m_type, m_cutoff, m_reso;
    const std::atomic<float>* m_live;
    bool m_dragging = false;
    QPointF m_press, m_start;
};

// a cycle of the named oscillator / LFO shape at phase 0..1; noise and sample & hold get fixed pseudo-random steps so the
// picture holds still
float shapeAt(const std::string& name, double ph, float pw) {
    if (name == "Sine") return (float)std::sin(ph * dsp::kTwoPi);
    if (name == "Saw") return (float)(2 * ph - 1);
    if (name == "Square") return ph < 0.5 ? 1.f : -1.f;
    if (name == "Pulse") return ph < pw ? 1.f : -1.f;
    if (name == "Triangle") return (float)(ph < 0.5 ? ph * 4 - 1 : 3 - ph * 4);
    const uint32_t h = (uint32_t)(ph * (name == "S&H" ? 6 : 48)) * 2654435761u;
    return ((h >> 8) & 0xffff) / 32767.5f - 1.f;
}

// One cycle of an oscillator or LFO shape. Click for the next shape (right-click or the left arrow: the previous one); drag up
// and down for the level, sideways for a second parameter (fine tune, rate, frequency). An LFO shows its phase as a dot riding the shape; the
// tone generator's shape fills in with its envelope.
class ShapeView : public Display {
public:
    struct Setup {
        uint32_t wave = 0, level = 0, across = 0, pw = 0;   // across: what a sideways drag moves (0: nothing)
        std::function<QString()> info;   // readout at the top right
        const std::atomic<float>* phase = nullptr;   // live LFO phase
        const EnvTap* env = nullptr;   // live envelope
    };
    ShapeView(IProcessor* p, Setup s, QWidget* parent)
        : Display(p, parent)
        , m_s(std::move(s)) {
        setMinimumSize(140, 80);
        setCursor(Qt::PointingHandCursor);
    }
    QSize sizeHint() const override { return {170, 96}; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        frame(p);
        const QRectF a = QRectF(rect()).adjusted(8, 24, -8, -8);
        const std::string name = shapeName();
        const float pw = m_s.pw ? get(m_s.pw) : 0.5f, level = m_s.level ? get(m_s.level) : 1.f;
        auto pt = [&](double ph, float amp) {
            return QPointF(a.left() + ph * a.width(), a.center().y() - shapeAt(name, std::min(ph, 0.9999), pw) * amp * a.height() / 2);
        };
        const int n = std::max(2, (int)a.width());
        QPolygonF wave, full;
        for (int i = 0; i <= n; ++i) {
            wave << pt((double)i / n, level);
            full << pt((double)i / n, 1.f);
        }
        p.setPen(theme::gridStrong());
        p.drawLine(QPointF(a.left(), a.center().y()), QPointF(a.right(), a.center().y()));
        if (level < 0.98f) {   // the shape at full level, so an oscillator turned down still shows what it is
            p.setPen(QPen(theme::withAlpha(theme::textDim(), 110), 1, Qt::DotLine));
            p.drawPolyline(full);
        }
        if (const float e = m_s.env ? m_s.env->value.load(kRelaxed) : 0.f; e > 0.001f) {
            QPainterPath fill(QPointF(a.left(), a.center().y()));
            fill.addPolygon(wave);
            fill.lineTo(a.right(), a.center().y());
            p.fillPath(fill, theme::withAlpha(theme::accent2(), (int)(25 + 110 * e)));
        }
        p.setPen(QPen(level > 0.001f ? theme::accent() : theme::textDim(), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(wave);
        if (m_s.phase) liveDot(p, pt(m_s.phase->load(kRelaxed), level));
        caption(p, QString::fromStdString(name), m_s.info ? m_s.info() : QString(), 40);
        drawTabs(p, kSteps, -1);
    }
    void mousePressEvent(QMouseEvent* e) override {
        m_press = e->position();
        m_axis = 0;
        m_startLevel = m_s.level ? norm(m_s.level) : 0.f;
        m_startAcross = m_s.across ? norm(m_s.across) : 0.f;
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton)) return;
        const QPointF d = e->position() - m_press;
        if (!m_axis) {
            if (d.manhattanLength() < 4) return;
            m_axis = std::fabs(d.x()) > std::fabs(d.y()) ? 1 : 2;   // one parameter per drag, chosen by the first move
        }
        if (m_axis == 2 && m_s.level) drag(m_s.level, m_startLevel - d.y() / (height() * 0.8));
        if (m_axis == 1 && m_s.across) drag(m_s.across, m_startAcross + d.x() / 300);
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (m_axis) return commit();
        const int count = (int)ps().descById(m_s.wave)->choices.size(), t = tabAt(e->position(), kSteps);
        const bool back = t == 0 || (t < 0 && e->button() == Qt::RightButton);
        setNow(m_s.wave, (float)((ps().getInt(m_s.wave) + (back ? count - 1 : 1)) % count));
    }

private:
    std::string shapeName() const {
        const auto& c = ps().descById(m_s.wave)->choices;
        return c[clamp(ps().getInt(m_s.wave), 0, (int)c.size() - 1)];
    }
    Setup m_s;
    QPointF m_press;
    int m_axis = 0;   // of the drag under way: 1 sideways, 2 up and down
    float m_startLevel = 0, m_startAcross = 0;
};

// The wavetable: the frame at the position large, and below it every frame of the bank in a strip that works as the position
// slider (drag sideways anywhere). The tabs pick the bank. While notes play, the frame the envelope and LFO have moved to is
// drawn over it and marked in the strip.
class WavetableView : public Display {
public:
    WavetableView(WavetableSynth* w, QWidget* parent)
        : Display(w, parent)
        , m_w(w) {
        setMinimumSize(240, 120);
        setCursor(Qt::SizeHorCursor);
        setToolTip("Drag sideways to move through the table; the tabs pick the bank");
    }
    QSize sizeHint() const override { return {340, 140}; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        frame(p);
        const int bank = ps().getInt(WavetableSynth::pBank);
        const float pos = get(WavetableSynth::pPosition), live = m_w->livePos.load(kRelaxed);
        const bool moved = m_w->liveVoices.load(kRelaxed) > 0 && std::fabs(live - pos) > 0.004f;
        const QRectF a = QRectF(rect()).adjusted(10, 24, -10, -48), s = strip();
        p.setPen(theme::gridStrong());
        p.drawLine(QPointF(a.left(), a.center().y()), QPointF(a.right(), a.center().y()));
        const QPolygonF cur = frameIn(a, bank, pos, 160);
        QPainterPath fill(QPointF(a.left(), a.center().y()));
        fill.addPolygon(cur);
        fill.lineTo(a.right(), a.center().y());
        p.fillPath(fill, theme::withAlpha(theme::accent(), 40));
        if (moved) {
            p.setPen(QPen(theme::accent2(), 1.3));
            p.drawPolyline(frameIn(a, bank, live, 160));
        }
        p.setPen(QPen(theme::accent(), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(cur);
        const double cw = s.width() / WavetableSynth::kTables;
        for (int k = 0; k < WavetableSynth::kTables; ++k) {
            const QRectF c(s.left() + k * cw + 1, s.top(), cw - 2, s.height());
            p.setPen(Qt::NoPen);
            p.setBrush(theme::withAlpha(theme::text(), 9));
            p.drawRoundedRect(c, theme::radiusFor(c.height()), theme::radiusFor(c.height()));
            p.setPen(QPen(theme::textDim(), 1));
            p.drawPolyline(frameIn(c.adjusted(4, 5, -4, -5), bank, (float)k / (WavetableSynth::kTables - 1), 40));
        }
        auto marker = [&](float at, const QColor& c, double w) {
            const double x = s.left() + (at * (WavetableSynth::kTables - 1) + 0.5) * cw;
            p.setPen(QPen(c, w));
            p.drawLine(QPointF(x, s.top() - 3), QPointF(x, s.bottom() + 3));
        };
        if (moved) marker(live, theme::accent2(), 1.4);
        marker(pos, theme::accent(), 2);
        caption(p, QString("Frame %1 / %2").arg(1 + pos * (WavetableSynth::kTables - 1), 0, 'f', 1).arg(WavetableSynth::kTables));
        drawTabs(p, kBanks, bank);
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        if (int t = tabAt(e->position(), kBanks); t >= 0) return setNow(WavetableSynth::pBank, (float)t);
        m_dragging = true;
        m_press = e->position();
        m_start = norm(WavetableSynth::pPosition);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!m_dragging) return setCursor(tabAt(e->position(), kBanks) >= 0 ? Qt::PointingHandCursor : Qt::SizeHorCursor);
        const double span = strip().width() * (WavetableSynth::kTables - 1) / WavetableSynth::kTables;   // first to last frame
        drag(WavetableSynth::pPosition, m_start + (e->position().x() - m_press.x()) / span);
    }
    void mouseReleaseEvent(QMouseEvent*) override {
        commit();
        m_dragging = false;
    }

private:
    static inline const QStringList kBanks = {"Basic", "Harmonic", "Digital"};
    QRectF strip() const { return QRectF(10, height() - 40, width() - 20, 30); }
    // the frame at a position as the voices read it (linear between the two nearest tables), fitted into r
    QPolygonF frameIn(const QRectF& r, int bank, float pos, int n) const {
        const float fp = clamp(pos, 0.f, 0.9999f) * (WavetableSynth::kTables - 1), ft = fp - (int)fp;
        const float *t0 = m_w->table(bank, (int)fp), *t1 = m_w->table(bank, std::min((int)fp + 1, WavetableSynth::kTables - 1));
        QPolygonF poly;
        for (int i = 0; i <= n; ++i) {
            const int k = std::min(i * WavetableSynth::kTableSize / n, WavetableSynth::kTableSize - 1);
            poly << QPointF(r.left() + r.width() * i / n, r.center().y() - (t0[k] + (t1[k] - t0[k]) * ft) * r.height() / 2);
        }
        return poly;
    }
    WavetableSynth* m_w;
    bool m_dragging = false;
    QPointF m_press;
    float m_start = 0;
};

// The FM algorithm as operators wired top to bottom: modulators above the operators they modulate, carriers on the output line,
// operator 1's feedback loop beside it. The arrows (or the wheel) step through the algorithms; click an operator to edit it
// beside, drag it up or down for its level. Operators light up with their envelopes while notes play.
class AlgorithmView : public Display {
public:
    AlgorithmView(FMSynth* fm, QWidget* parent)
        : Display(fm, parent)
        , m_fm(fm) {
        setMinimumSize(250, 220);
        setToolTip("Click an operator to edit it, drag it up or down for its level; the arrows or the wheel change the algorithm");
    }
    std::function<void(int)> onSelect;
    int selected = 0;
    QSize sizeHint() const override { return {280, 236}; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        frame(p);
        const int alg = algorithm();
        const Wiring& L = kAlgs[alg];
        caption(p, QString("Algorithm %1").arg(alg + 1), QString());
        p.setPen(theme::textDim());
        p.drawText(QRectF(80, 3, width() - 140, 16), Qt::AlignLeft | Qt::AlignVCenter,
            QString::fromStdString(ps().descById(FMSynth::pAlgorithm)->choices[alg]));
        drawTabs(p, kSteps, -1);
        // carriers drop onto the output line
        const double outY = top() + rows() * step() + 14;
        double x0 = 1e9, x1 = -1e9;
        p.setPen(QPen(theme::textDim(), 1.2));
        for (int o = 0; o < 4; ++o)
            if (L.carriers >> o & 1) {
                const QRectF b = box(o);
                p.drawLine(QPointF(b.center().x(), b.bottom()), QPointF(b.center().x(), outY));
                x0 = std::min(x0, b.center().x());
                x1 = std::max(x1, b.center().x());
            }
        p.drawLine(QPointF(x0 - 10, outY), QPointF(x1 + 10, outY));
        p.drawText(QPointF(x1 + 16, outY + 4), "out");
        // modulation wires with arrowheads
        for (int o = 0; o < 4; ++o)
            for (int k = 0; k < 4; ++k)
                if (L.mods[o] >> k & 1) {
                    const QPointF a(box(o).center().x(), box(o).bottom()), b(box(k).center().x(), box(k).top());
                    p.setPen(QPen(theme::textDim(), 1.2));
                    p.drawLine(a, b);
                    const QLineF dir(b, a);
                    QLineF w1 = dir, w2 = dir;
                    w1.setLength(6);
                    w2.setLength(6);
                    w1.setAngle(dir.angle() + 25);
                    w2.setAngle(dir.angle() - 25);
                    p.drawLine(w1);
                    p.drawLine(w2);
                }
        {   // operator 1 feeds back into itself: the loop gets bolder with the feedback amount
            const float fb = get(FMSynth::pFeedback);
            const QRectF b = box(0);
            QPainterPath loop(QPointF(b.right() - 10, b.top()));
            loop.lineTo(b.right() - 10, b.top() - 8);
            loop.lineTo(b.center().x(), b.top() - 8);
            loop.lineTo(b.center().x(), b.top());
            p.setPen(QPen(fb > 0.001f ? theme::accent2() : theme::withAlpha(theme::textDim(), 120), 1 + 2 * fb));
            p.setBrush(Qt::NoBrush);
            p.drawPath(loop);
        }
        p.setFont(theme::smallFont());
        for (int o = 0; o < 4; ++o) {
            const QRectF b = box(o);
            const float lvl = get(FMSynth::opParam(o, 1)), live = m_fm->opTap[o].value.load(kRelaxed) * lvl;
            p.setPen(o == selected ? QPen(theme::accent(), 2) : QPen(theme::border(), 1));
            p.setBrush(theme::mix(theme::panelLight(), theme::accent2(), 0.08 + 0.45 * live));
            p.drawRoundedRect(b, theme::radiusFor(b.height()), theme::radiusFor(b.height()));
            p.setPen(Qt::NoPen);
            p.setBrush(theme::withAlpha(theme::text(), 25));
            p.drawRect(QRectF(b.left() + 5, b.bottom() - 6, b.width() - 10, 2.5));
            p.setBrush(theme::accent());
            p.drawRect(QRectF(b.left() + 5, b.bottom() - 6, (b.width() - 10) * lvl, 2.5));
            const QRectF t = b.adjusted(6, 3, -6, -8);
            p.setPen(theme::text());
            p.drawText(t, Qt::AlignLeft | Qt::AlignVCenter, QString::number(o + 1));
            const float ratio = get(FMSynth::opParam(o, 0));
            p.setPen(theme::textDim());
            p.drawText(t, Qt::AlignRight | Qt::AlignVCenter,
                ps().getBool(FMSynth::opParam(o, 7)) ? QString("%1 Hz").arg(ratio * 100, 0, 'f', 0)
                                                     : QString("\u00D7%1").arg(ratio, 0, 'f', 2));
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (int t = tabAt(e->position(), kSteps); t >= 0) return step(t * 2 - 1);
        const int o = opAt(e->position());
        if (o < 0) return;
        selected = o;
        if (onSelect) onSelect(o);
        m_drag = o;
        m_press = e->position();
        m_start = norm(FMSynth::opParam(o, 1));
        update();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (m_drag >= 0) return drag(FMSynth::opParam(m_drag, 1), m_start - (e->position().y() - m_press.y()) / 100);
        const bool hit = opAt(e->position()) >= 0 || tabAt(e->position(), kSteps) >= 0;
        setCursor(hit ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }
    void mouseReleaseEvent(QMouseEvent*) override {
        commit();
        m_drag = -1;
    }
    void wheelEvent(QWheelEvent* e) override {
        if (e->angleDelta().y() != 0) step(e->angleDelta().y() > 0 ? -1 : 1);
    }

private:
    struct Wiring {
        float col[4];
        int row[4];   // 0: carriers, on the output line
        uint8_t mods[4];   // bit k: modulates operator k + 1
        uint8_t carriers;
    };
    static constexpr Wiring kAlgs[8] = {
        // as FMSynth::process wires them
        {{0, 0, 0, 0}, {3, 2, 1, 0}, {0b0010, 0b0100, 0b1000, 0}, 0b1000},   // 1>2>3>4
        {{-.5f, .5f, 0, 0}, {2, 2, 1, 0}, {0b0100, 0b0100, 0b1000, 0}, 0b1000},   // (1+2)>3>4
        {{-.5f, -.5f, .5f, .5f}, {1, 0, 1, 0}, {0b0010, 0, 0b1000, 0}, 0b1010},   // 1>2, 3>4
        {{0, -1, 0, 1}, {1, 0, 0, 0}, {0b1110, 0, 0, 0}, 0b1110},   // 1>(2+3+4)
        {{-.5f, -.5f, .5f, .5f}, {1, 0, 1, 0}, {0b0010, 0, 0b1000, 0}, 0b1010},   // (1>2)+(3>4)
        {{-.5f, -.5f, -.5f, .5f}, {2, 1, 0, 0}, {0b0010, 0b0100, 0, 0}, 0b1100},   // 1>2>3, 4
        {{-1.5f, -.5f, .5f, 1.5f}, {0, 0, 0, 0}, {0, 0, 0, 0}, 0b1111},   // 1+2+3+4
        {{0, 0, -.5f, .5f}, {2, 1, 0, 0}, {0b0010, 0b1100, 0, 0}, 0b1100},   // 1>2>(3+4)
    };
    int algorithm() const { return clamp(ps().getInt(FMSynth::pAlgorithm), 0, 7); }
    int rows() const { return 1 + *std::max_element(kAlgs[algorithm()].row, kAlgs[algorithm()].row + 4); }
    double step() const { return std::min(50.0, (height() - 56) / 4.0); }
    double top() const { return 26 + (height() - 52 - rows() * step()) / 2; }   // operators and output line centred
    QRectF box(int o) const {
        const Wiring& L = kAlgs[algorithm()];
        const double cx = width() / 2.0 - 6 + L.col[o] * 64, cy = top() + (rows() - 0.5 - L.row[o]) * step();
        return QRectF(cx - 29, cy - 15, 58, 30);
    }
    int opAt(const QPointF& pt) const {
        for (int o = 0; o < 4; ++o)
            if (box(o).adjusted(-3, -3, 3, 3).contains(pt)) return o;
        return -1;
    }
    void step(int dir) { setNow(FMSynth::pAlgorithm, (float)((algorithm() + dir + 8) % 8)); }
    FMSynth* m_fm;
    int m_drag = -1;
    QPointF m_press;
    float m_start = 0;
};

// The strip on top of every editor: the instrument's name, and on the right what it is doing now
class Header : public QWidget {
public:
    Header(const QString& name, std::function<QString()> status, QWidget* parent)
        : QWidget(parent)
        , m_name(name)
        , m_status(std::move(status)) {
        setFixedHeight(22);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        theme::drawPanel(p, QRectF(rect()), theme::panelLight(), theme::radiusFor(height()));
        const QRect r = rect().adjusted(9, 0, -9, 0);
        QFont f = theme::smallFont();
        f.setBold(true);
        p.setFont(f);
        p.setPen(theme::text());
        p.drawText(r, Qt::AlignLeft | Qt::AlignVCenter, m_name);
        p.setFont(theme::smallFont());
        p.setPen(theme::textDim());
        p.drawText(r, Qt::AlignRight | Qt::AlignVCenter, m_status());
    }

private:
    QString m_name;
    std::function<QString()> m_status;
};

QString voicesText(const std::atomic<int>& n) {
    const int v = n.load(kRelaxed);
    return v ? QString("%1 voice%2").arg(v).arg(v == 1 ? "" : "s") : QString("idle");
}

// An editor: the header strip, then a grid of titled sections. Every display and knob panel refreshes about 30 times a second
// (live values, automation, undo).
struct Editor {
    QWidget* w;
    QGridLayout* grid;
    std::vector<QWidget*> views;
    Editor(QWidget* parent, const QString& name, std::function<QString()> status)
        : w(new QWidget(parent))
        , grid(new QGridLayout()) {
        auto* lay = new QVBoxLayout(w);
        lay->setContentsMargins(8, 8, 8, 8);
        lay->setSpacing(8);
        views.push_back(new Header(name, std::move(status), w));
        lay->addWidget(views.back());
        grid->setHorizontalSpacing(12);
        grid->setVerticalSpacing(8);
        lay->addLayout(grid);
        lay->addStretch(1);
    }
    QVBoxLayout* section(int row, int col, const QString& title, int colSpan = 1, QLabel** label = nullptr) {
        auto* box = new QVBoxLayout();
        box->setSpacing(4);
        auto* l = theme::dimLabel(title, w, "font-weight:bold; font-size:8pt;");
        if (label) *label = l;
        box->addWidget(l);
        grid->addLayout(box, row, col, 1, colSpan, Qt::AlignTop);
        return box;
    }
    template <class D> D* view(QVBoxLayout* box, D* d, int height) {
        d->setFixedHeight(height);
        box->addWidget(d);
        views.push_back(d);
        return d;
    }
    // knobs for some parameters, named without the prefix the section title already says ("Osc 1 Octave" is "Octave" under
    // OSC 1; the tooltip keeps the full name)
    GenericParamPanel* knobs(QVBoxLayout* box, IProcessor* p, std::vector<uint32_t> ids, int cols) {
        auto* gp = new GenericParamPanel(
            *App::instance(), ownerOf(p), p, [ids](const ParamDesc& d) { return std::find(ids.begin(), ids.end(), d.id) != ids.end(); },
            cols, w);
        static const QRegularExpression prefix("^(Osc \\d|Op \\d|F\\.|LFO( >)?) ");
        for (auto* k : gp->findChildren<Knob*>()) k->setLabel(k->toolTip().remove(prefix));
        for (auto* c : gp->findChildren<QCheckBox*>()) c->setText(c->text().remove(prefix));
        gp->layout()->setContentsMargins(0, 0, 0, 0);
        box->addWidget(gp);
        return gp;
    }
    QWidget* done() {
        auto* t = new QTimer(w);
        QObject::connect(t, &QTimer::timeout, w, [w = w, views = views] {
            for (auto* v : views) v->update();
            for (auto* gp : w->findChildren<GenericParamPanel*>()) gp->sync();
        });
        t->start(33);
        return w;
    }
};

QString percentText(float v) { return QString("%1%").arg(std::lround(v * 100)); }

// ------------------------------------------------------------------------------------------------ Synth
QWidget* makeSynthEditor(Synth* s, QWidget* parent) {
    Editor e(parent, "Synth", [s] { return voicesText(s->liveVoices); });
    auto osc = [&](int col, const char* title, uint32_t wave, uint32_t level, uint32_t oct, uint32_t semi, uint32_t fine, uint32_t pw) {
        auto* box = e.section(0, col, title);
        auto* v = e.view(box,
            new ShapeView(s,
                {.wave = wave,
                    .level = level,
                    .across = fine,
                    .pw = pw,
                    .info =
                        [s, level, oct, semi, fine] {
                            auto& ps = s->params();
                            QString t = percentText(ps.get(level));
                            for (auto [id, unit] : {std::pair(oct, "oct"), std::pair(semi, "st"), std::pair(fine, "ct")})
                                if (std::fabs(ps.get(id)) >= 0.5f)
                                    t += QString("  %1%2 %3").arg(ps.get(id) > 0 ? "+" : "").arg(ps.get(id), 0, 'f', 0).arg(unit);
                            return t;
                        }},
                e.w),
            96);
        v->setToolTip("Click or use the arrows to change the shape. Drag up and down for the level, sideways to fine-tune");
        v->setObjectName(title);
        e.knobs(box, s, {oct, semi, fine, pw}, 2);
    };
    osc(0, "OSC 1", Synth::pOsc1Wave, Synth::pOsc1Level, Synth::pOsc1Octave, Synth::pOsc1Semi, Synth::pOsc1Fine, Synth::pOsc1PW);
    osc(1, "OSC 2", Synth::pOsc2Wave, Synth::pOsc2Level, Synth::pOsc2Octave, Synth::pOsc2Semi, Synth::pOsc2Fine, Synth::pOsc2PW);
    auto* filter = e.section(0, 2, "FILTER");
    e.view(filter, new FilterGraph(s, Synth::pFilterType, Synth::pCutoff, Synth::pReso, &s->liveCutoff, e.w), 96)->setObjectName("filter");
    e.knobs(filter, s, {Synth::pCutoff, Synth::pReso, Synth::pFilterEnv, Synth::pFilterKeyTrack}, 4);
    auto* lfo = e.section(0, 3, "LFO");
    e.view(lfo,
         new ShapeView(s,
             {.wave = Synth::pLfoShape,
                 .across = Synth::pLfoRate,
                 .info =
                     [s] {
                         auto& ps = s->params();
                         return ps.getBool(Synth::pLfoSync) ? QString("%1 / bar").arg(std::max(0.25f, std::round(ps.get(Synth::pLfoRate))))
                                                            : s->formatValue(Synth::pLfoRate);
                     },
                 .phase = &s->liveLfo},
             e.w),
         96)
        ->setToolTip("Click or use the arrows to change the shape. Drag sideways for the rate");
    e.knobs(lfo, s, {Synth::pLfoRate, Synth::pLfoPitch, Synth::pLfoFilter, Synth::pLfoSync}, 2);
    auto* amp = e.section(1, 0, "AMP ENVELOPE", 2);
    e.view(amp, new EnvGraph(s, {Synth::pAA, Synth::pAD, Synth::pAS, Synth::pAR}, &s->ampTap, e.w), 110)->setObjectName("ampEnv");
    auto* fenv = e.section(1, 2, "FILTER ENVELOPE", 2);
    e.view(fenv, new EnvGraph(s, {Synth::pFA, Synth::pFD, Synth::pFS, Synth::pFR}, &s->filterTap, e.w), 110);
    auto* voice = e.section(2, 0, "VOICE", 4);
    e.knobs(voice, s,
        {Synth::pUnison, Synth::pDetune, Synth::pSpread, Synth::pSub, Synth::pNoise, Synth::pDrive, Synth::pGlide, Synth::pVoices,
            Synth::pMono, Synth::pBendRange, Synth::pLevel},
        11);
    return e.done();
}

// ------------------------------------------------------------------------------------------------ Wavetable
QWidget* makeWavetableEditor(WavetableSynth* s, QWidget* parent) {
    using W = WavetableSynth;
    Editor e(parent, "Wavetable", [s] { return voicesText(s->liveVoices); });
    auto* table = e.section(0, 0, "WAVETABLE", 2);
    e.view(table, new WavetableView(s, e.w), 140);
    e.knobs(table, s, {W::pPosition, W::pPosEnv, W::pOctave, W::pSemi, W::pFine}, 5);
    auto* filter = e.section(0, 2, "FILTER");
    e.view(filter, new FilterGraph(s, W::pFilterType, W::pCutoff, W::pReso, &s->liveCutoff, e.w), 140);
    e.knobs(filter, s, {W::pCutoff, W::pReso, W::pFilterEnv}, 3);
    auto* lfo = e.section(0, 3, "LFO");
    e.view(lfo,
         new ShapeView(s,
             {.wave = W::pLfoShape, .across = W::pLfoRate, .info = [s] { return s->formatValue(W::pLfoRate); }, .phase = &s->liveLfo}, e.w),
         140)
        ->setToolTip("Click or use the arrows to change the shape. Drag sideways for the rate; Pos < LFO sets how far it moves the table");
    e.knobs(lfo, s, {W::pLfoRate, W::pPosLfo}, 2);
    auto* amp = e.section(1, 0, "AMP ENVELOPE", 2);
    e.view(amp, new EnvGraph(s, {W::pAA, W::pAD, W::pAS, W::pAR}, &s->ampTap, e.w), 110);
    auto* fenv = e.section(1, 2, "FILTER ENVELOPE", 2);
    e.view(fenv, new EnvGraph(s, {W::pFA, W::pFD, W::pFS, W::pFR}, &s->filterTap, e.w), 110);
    auto* voice = e.section(2, 0, "VOICE", 4);
    e.knobs(voice, s, {W::pUnison, W::pDetune, W::pSpread, W::pGlide, W::pLevel}, 5);
    return e.done();
}

// ------------------------------------------------------------------------------------------------ FM Synth
QWidget* makeFmEditor(FMSynth* fm, QWidget* parent) {
    Editor e(parent, "FM Synth", [fm] { return voicesText(fm->liveVoices); });
    auto* algBox = e.section(0, 0, "ALGORITHM");
    auto* alg = e.view(algBox, new AlgorithmView(fm, e.w), 236);
    QLabel* opTitle = nullptr;
    auto* op = e.section(0, 1, "OPERATOR 1", 1, &opTitle);
    auto envIds = [](int o) {
        return EnvGraph::Ids{FMSynth::opParam(o, 3), FMSynth::opParam(o, 4), FMSynth::opParam(o, 5), FMSynth::opParam(o, 6)};
    };
    auto* opEnv = e.view(op, new EnvGraph(fm, envIds(0), &fm->opTap[0], e.w), 138);
    std::array<GenericParamPanel*, 4> opKnobs;
    for (int o = 0; o < 4; ++o) {
        opKnobs[o] = e.knobs(op, fm, {FMSynth::opParam(o, 0), FMSynth::opParam(o, 1), FMSynth::opParam(o, 2), FMSynth::opParam(o, 7)}, 4);
        opKnobs[o]->setVisible(o == 0);
    }
    alg->onSelect = [fm, opTitle, opEnv, opKnobs, envIds](int o) {
        opTitle->setText(QString("OPERATOR %1").arg(o + 1));
        opEnv->follow(envIds(o), &fm->opTap[o]);
        for (int k = 0; k < 4; ++k) opKnobs[k]->setVisible(k == o);
    };
    auto* amp = e.section(1, 0, "AMP ENVELOPE");
    e.view(amp, new EnvGraph(fm, {FMSynth::pAA, FMSynth::pAD, FMSynth::pAS, FMSynth::pAR}, &fm->ampTap, e.w), 100);
    auto* global = e.section(1, 1, "GLOBAL");
    e.knobs(
        global, fm, {FMSynth::pFeedback, FMSynth::pLevel, FMSynth::pLfoRate, FMSynth::pLfoDepth, FMSynth::pGlide, FMSynth::pBendRange}, 6);
    return e.done();
}

// ------------------------------------------------------------------------------------------------ Tone Generator
QString noteOf(float hz) { return theme::noteName((int)std::lround(69 + 12 * std::log2(std::max(hz, 1.f) / 440.f))); }
QWidget* makeToneEditor(ToneGenerator* t, QWidget* parent) {
    using T = ToneGenerator;
    auto status = [t] {
        if (t->envTap.stage.load(kRelaxed) == dsp::ADSR::Idle) return QString("idle");
        const float f = t->liveFreq.load(kRelaxed);
        return QString("%1  %2 Hz").arg(noteOf(f)).arg(f, 0, 'f', 1);
    };
    Editor e(parent, "Tone Generator", status);
    auto* tone = e.section(0, 0, "TONE");
    auto* shape = e.view(tone,
        new ShapeView(t,
            {.wave = T::pWave,
                .level = T::pLevel,
                .across = T::pFreq,
                .info = [t] { return QString("%1  %2").arg(t->formatValue(T::pFreq), noteOf(t->params().get(T::pFreq))); },
                .env = &t->envTap},
            e.w),
        130);
    shape->setMinimumWidth(260);
    shape->setToolTip("Click or use the arrows to change the shape. Drag up and down for the level, sideways for the frequency");
    auto* env = e.section(0, 1, "ENVELOPE");
    e.view(env, new EnvGraph(t, {T::pAttack, 0, 0, T::pRelease}, &t->envTap, e.w), 130);
    auto* knobs = e.section(1, 0, "OUTPUT", 2);
    e.knobs(knobs, t, {T::pFreq, T::pLevel, T::pAttack, T::pRelease, T::pGate}, 5);
    return e.done();
}

NativeEditorRegistration s_synth(
    {"native.synth"}, [](IProcessor* p, QWidget* parent) { return makeSynthEditor(static_cast<Synth*>(p), parent); });
NativeEditorRegistration s_wavetable(
    {"native.wavetable"}, [](IProcessor* p, QWidget* parent) { return makeWavetableEditor(static_cast<WavetableSynth*>(p), parent); });
NativeEditorRegistration s_fm({"native.fm"}, [](IProcessor* p, QWidget* parent) { return makeFmEditor(static_cast<FMSynth*>(p), parent); });
NativeEditorRegistration s_tone(
    {"native.tonegen"}, [](IProcessor* p, QWidget* parent) { return makeToneEditor(static_cast<ToneGenerator*>(p), parent); });

}   // namespace
}   // namespace mt
