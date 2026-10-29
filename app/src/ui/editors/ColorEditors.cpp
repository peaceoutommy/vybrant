// Visual editors for the distortion, stereo / utility and analysis effects: a display you drag on, the knobs below it, live
// scopes and meters fed lock-free from the audio thread (StereoTap, the analyzer's spectrum, the meter's levels).
#include "ui/PluginWindow.h"
#include "ui/Theme.h"
#include "dsp/effects/Effects.h"
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QVBoxLayout>
#include <array>
#include <cmath>
#include <functional>

namespace mt {
namespace {

Id ownerOf(IProcessor* p) {
    for (auto& [id, proc] : App::instance()->engine().registry().processors())
        if (proc.get() == p) return id;
    return 0;
}

constexpr int kTap = (int)StereoTap::kSize;
enum { InL, InR, OutL, OutR };

// The UI's copy of an effect's StereoTap, oldest sample first
struct TapCopy {
    std::vector<float> ch[4];
    uint32_t last = 0;
    int fresh = 0;   // samples that came in since the previous read
    void read(const StereoTap& t) {
        const uint32_t end = t.pos.load(std::memory_order_acquire);
        fresh = (int)std::min<uint32_t>(end - last, StereoTap::kSize);
        last = end;
        for (int c = 0; c < 4; ++c) {
            ch[c].resize(kTap);
            for (int i = 0; i < kTap; ++i) ch[c][i] = t.buf[c][(end + i) & (kTap - 1)].load(std::memory_order_relaxed);
        }
    }
    float peak(int c) const {
        float m = 0;
        for (int i = kTap - fresh; i < kTap; ++i) m = std::max(m, std::fabs(ch[c][i]));
        return m;
    }
    std::vector<float> mid(int c0, int from, int n) const {   // (L + R) / 2 of the input (InL) or the output (OutL)
        std::vector<float> v(n);
        for (int i = 0; i < n; ++i) v[i] = (ch[c0][from + i] + ch[c0 + 1][from + i]) * 0.5f;
        return v;
    }
    // where an n-sample scope window starts: the newest rising zero crossing of the input that leaves room for the window
    int trigger(int n) const {
        for (int i = kTap - n; i > kTap - n - 2048 && i > 0; --i)
            if (ch[InL][i - 1] + ch[InR][i - 1] <= 0.f && ch[InL][i] + ch[InR][i] > 0.f) return i;
        return kTap - n;
    }
    // of the output's two sides over the newest n samples: +1 mono, 0 unrelated, -1 one side inverted (0 when silent)
    float correlation(int n) const {
        double lr = 0, ll = 0, rr = 0;
        for (int i = kTap - n; i < kTap; ++i) {
            lr += ch[OutL][i] * ch[OutR][i];
            ll += ch[OutL][i] * ch[OutL][i];
            rr += ch[OutR][i] * ch[OutR][i];
        }
        return ll * rr > 1e-12 ? (float)(lr / std::sqrt(ll * rr)) : 0.f;
    }
};

// ---------------------------------------------------------------- drawing helpers shared by the displays
QColor dryColor() { return theme::withAlpha(theme::textDim(), 170); }   // the input, the reference
QColor wetColor() { return theme::accent(); }   // what the effect makes of it, and every handle
QColor dotColor() { return theme::mix(theme::accent2(), theme::text(), 0.35); }

void box(QPainter& p, const QRectF& r) {
    p.setPen(QPen(theme::border(), 1));
    p.setBrush(theme::surface());
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), theme::radius(), theme::radius());
}
void label(QPainter& p, const QRectF& r, int align, const QString& s, const QColor& c) {
    p.setFont(theme::smallFont());
    p.setPen(c);
    p.drawText(r, align, s);
}
// a wave across r, full scale at +-range
QPainterPath trace(const float* s, int n, const QRectF& r, float range = 1.1f) {
    QPainterPath path;
    for (int i = 0; i < n; ++i) {
        const QPointF pt(r.left() + r.width() * i / (n - 1), r.center().y() - clamp(s[i] / range, -1.f, 1.f) * r.height() / 2);
        i ? path.lineTo(pt) : path.moveTo(pt);
    }
    return path;
}
QString dbText(float lin) {
    const float db = gainToDb(lin);
    return db <= -90.f ? QString("-inf") : QString::number(db, 'f', 1);
}
// Level meter law shared by the gain and meter displays: -60 .. +6 dBFS with more room near the top
float meterPos(float db) { return std::pow(clamp((db + 60.f) / 66.f, 0.f, 1.f), 1.5f); }
void meterBar(QPainter& p, const QRectF& r, float peak, float rms = -1.f) {   // rms < 0: the peak alone, solid
    p.fillRect(r, theme::bg());
    QLinearGradient g(r.bottomLeft(), r.topLeft());   // the colours of the mixer's meters, placed on this law
    g.setColorAt(0, theme::green());
    g.setColorAt(meterPos(-10), theme::green());
    g.setColorAt(meterPos(-3), theme::accent());
    g.setColorAt(meterPos(0), theme::red());
    g.setColorAt(1, theme::red());
    auto upTo = [&](float lin) {
        const double h = r.height() * meterPos(gainToDb(lin));
        return QRectF(r.left(), r.bottom() - h, r.width(), h);
    };
    if (rms >= 0) p.setOpacity(0.45);
    p.fillRect(upTo(peak), g);
    p.setOpacity(1);
    if (rms >= 0) p.fillRect(upTo(rms), g);
}

// ---------------------------------------------------------------- the display base: drag on it to set parameters
class Display : public QWidget {
public:
    Display(IProcessor* p, QWidget* parent, QSize hint)
        : QWidget(parent)
        , m_proc(p)
        , m_owner(ownerOf(p))
        , m_hint(hint) {
        setObjectName("display");
        setMouseTracking(true);
        setMinimumSize(hint * 3 / 4);
    }
    QSize sizeHint() const override { return m_hint; }
    virtual void tick() { update(); }   // 30 Hz: take the DSP's live data, repaint

protected:
    float norm(uint32_t id) const { return m_proc->params().getNormalized(id); }
    float value(uint32_t id) const { return m_proc->params().get(id); }
    void set(uint32_t id, float n, bool commit) {
        App::instance()->setProcessorParam(m_owner, id, clamp(n, 0.f, 1.f), commit);
        update();
    }
    // m_dragX / m_dragY follow the mouse sideways / up-down inside dragArea() (an undo step each on release); m_spanX / m_spanY
    // pixels cover the whole range, Shift drags finer, a double-click resets them
    uint32_t m_dragX = 0, m_dragY = 0;
    float m_spanX = 250, m_spanY = 250;
    virtual QRectF dragArea() const { return rect(); }
    virtual float dragSign(QPointF) const { return 1.f; }   // -1: a sideways drag starting here runs the other way
    virtual bool click(QPointF) { return false; }   // buttons painted on the display
    bool hot() const { return m_dragging || ((m_dragX || m_dragY) && dragArea().contains(m_mouse)); }
    // "Drive 45 %   Bias 0.12" in a corner of r while dragging or pointing at the drag area
    void drawReadout(QPainter& p, const QRectF& r, Qt::Alignment corner) const {
        if (!hot()) return;
        QStringList parts;
        for (uint32_t id : {m_dragY, m_dragX})
            if (const ParamDesc* d = id ? m_proc->params().descById(id) : nullptr)
                parts << QString::fromStdString(d->name) + " " + m_proc->formatValue(id);
        p.setFont(theme::smallFont());
        const QString s = parts.join("   ");
        const QSizeF sz = QFontMetricsF(p.font()).size(0, s) + QSizeF(14, 6);
        const QRectF in = r.adjusted(6, 6, -6, -6);
        const QRectF c(QPointF(corner & Qt::AlignRight ? in.right() - sz.width() : in.left(),
                           corner & Qt::AlignBottom ? in.bottom() - sz.height() : in.top()),
            sz);
        p.setPen(QPen(theme::border(), 1));
        p.setBrush(theme::withAlpha(theme::panel(), 235));
        p.drawRoundedRect(c, theme::radiusFor(c.height()), theme::radiusFor(c.height()));
        p.setPen(theme::text());
        p.drawText(c, Qt::AlignCenter, s);
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton || click(e->position())) return;
        if ((m_dragX || m_dragY) && dragArea().contains(e->position())) {
            m_dragging = true;
            m_movedX = m_movedY = false;
            m_last = e->position();
            m_sign = dragSign(m_last);
        }
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        const QPointF pos = e->position();
        m_mouse = pos;
        if (!m_dragging) {
            const bool in = (m_dragX || m_dragY) && dragArea().contains(pos);
            setCursor(!in ? Qt::ArrowCursor : !m_dragY ? Qt::SizeHorCursor : !m_dragX ? Qt::SizeVerCursor : Qt::SizeAllCursor);
            update();
            return;
        }
        const float fine = (e->modifiers() & Qt::ShiftModifier) ? 0.2f : 1.f;
        const QPointF d = pos - m_last;
        m_last = pos;
        if (m_dragX && d.x() != 0) {
            set(m_dragX, norm(m_dragX) + m_sign * fine * (float)d.x() / m_spanX, false);
            m_movedX = true;
        }
        if (m_dragY && d.y() != 0) {
            set(m_dragY, norm(m_dragY) - fine * (float)d.y() / m_spanY, false);
            m_movedY = true;
        }
    }
    void mouseReleaseEvent(QMouseEvent*) override {
        if (!m_dragging) return;
        m_dragging = false;
        if (m_movedX) set(m_dragX, norm(m_dragX), true);
        if (m_movedY) set(m_dragY, norm(m_dragY), true);
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton || click(e->position()) || !dragArea().contains(e->position())) return;
        for (uint32_t id : {m_dragX, m_dragY})
            if (const ParamDesc* d = id ? m_proc->params().descById(id) : nullptr) set(id, d->toNormalized(d->def), true);
    }
    void leaveEvent(QEvent*) override {
        m_mouse = {-1, -1};
        update();
    }
    IProcessor* m_proc;
    Id m_owner;
    QPointF m_mouse{-1, -1};

private:
    QSize m_hint;
    bool m_dragging = false, m_movedX = false, m_movedY = false;
    QPointF m_last;
    float m_sign = 1;
};

// The strip above every display: the effect's name and how to work the display
class Header : public QWidget {
public:
    Header(const QString& name, const QString& hint, QWidget* parent)
        : QWidget(parent)
        , m_name(name.toUpper())
        , m_hint(hint) {
        setFixedHeight(18);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        QFont f = theme::smallFont();
        f.setBold(true);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 1);
        p.setFont(f);
        p.setPen(theme::text());
        p.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter, m_name);
        label(p, rect(), Qt::AlignRight | Qt::AlignVCenter, m_hint, theme::textDim());
    }

private:
    QString m_name, m_hint;
};

// Header, display, a row of knobs (all the parameters unless 'knobs' filters them); one 30 Hz timer keeps them live
QWidget* frame(IProcessor* p, QWidget* parent, Display* d, const QString& hint, std::function<bool(const ParamDesc&)> knobs = nullptr) {
    auto* w = new QWidget(parent);
    auto* lay = new QVBoxLayout(w);
    lay->setContentsMargins(8, 6, 8, 8);
    lay->setSpacing(6);
    lay->addWidget(new Header(p->displayName(), hint, w));
    d->setParent(w);
    lay->addWidget(d, 1);
    const Id owner = ownerOf(p);
    auto* gp = new GenericParamPanel(*App::instance(), owner, p, knobs, 8, w);
    gp->setVisible(gp->layout()->count() > 0);
    lay->addWidget(gp);
    auto* t = new QTimer(w);
    QObject::connect(t, &QTimer::timeout, w, [p, d, gp, owner] {
        if (owner && App::instance()->processor(owner) != p) return;   // removed or replaced: the window rebuilds or closes
        d->tick();
        gp->sync();
    });
    t->start(33);
    return w;
}

// ---------------------------------------------------------------- distortion: the effect on the left, a scope on the right
// Left (the drag area, a square): what the effect does to a signal. Right: the live input and output, on the input's rising
// zero crossings so a steady tone stands still.
class ShaperDisplay : public Display {
public:
    ShaperDisplay(IProcessor* p, const StereoTap& tap, QWidget* parent)
        : Display(p, parent, {560, 240})
        , m_src(tap) {
        m_tap.read(tap);
    }
    void tick() override {
        m_tap.read(m_src);
        m_level = std::max(std::max(m_tap.peak(InL), m_tap.peak(InR)), m_level * 0.85f);
        update();
    }

protected:
    static constexpr double kRange = 1.1;   // both panels show +-1.1, so 0 dBFS sits just inside
    virtual void paintShape(QPainter& p, const QRectF& r) = 0;
    QRectF dragArea() const override { return QRectF(0, 0, height(), height()); }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF left = dragArea(), right(left.right() + 8, 0, width() - left.width() - 8, height());
        box(p, left);
        p.save();
        p.setClipRect(left.adjusted(1, 1, -1, -1));
        paintShape(p, left.adjusted(1, 1, -1, -1));
        p.restore();
        drawReadout(p, left, Qt::AlignBottom | Qt::AlignRight);
        box(p, right);
        const QRectF r = right.adjusted(1, 1, -1, -1);
        auto Y = [&](double v) { return r.center().y() - v / kRange * r.height() / 2; };
        p.setPen(QPen(theme::grid(), 1));
        for (int i = 1; i < 8; ++i)
            p.drawLine(QPointF(r.left() + r.width() * i / 8, r.top()), QPointF(r.left() + r.width() * i / 8, r.bottom()));
        for (double v : {-0.5, 0.5}) p.drawLine(QPointF(r.left(), Y(v)), QPointF(r.right(), Y(v)));
        p.setPen(QPen(theme::gridStrong(), 1, Qt::DashLine));
        for (double v : {-1.0, 1.0}) p.drawLine(QPointF(r.left(), Y(v)), QPointF(r.right(), Y(v)));
        p.setPen(QPen(theme::gridStrong(), 1));
        p.drawLine(QPointF(r.left(), Y(0)), QPointF(r.right(), Y(0)));
        const int n = 1024, from = m_tap.trigger(n);
        p.save();
        p.setClipRect(r);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(dryColor(), 1.2));
        p.drawPath(trace(m_tap.mid(InL, from, n).data(), n, r));
        p.setPen(QPen(wetColor(), 1.8));
        p.drawPath(trace(m_tap.mid(OutL, from, n).data(), n, r));
        p.restore();
        label(p, r.adjusted(8, 4, 0, 0), Qt::AlignLeft | Qt::AlignTop, "in", dryColor());
        label(p, r.adjusted(26, 4, 0, 0), Qt::AlignLeft | Qt::AlignTop, "out", wetColor());
    }
    TapCopy m_tap;
    const StereoTap& m_src;
    float m_level = 0;   // the input's recent peak: the part of the curve in use
};

// Saturation, Waveshaper, Distortion: the transfer curve (input across, output up), the stretch the input is using lit up
class CurveDisplay : public ShaperDisplay {
public:
    CurveDisplay(IProcessor* p, const StereoTap& tap, std::function<float(float)> curve, uint32_t dragY, uint32_t dragX)
        : ShaperDisplay(p, tap, nullptr)
        , m_curve(std::move(curve)) {
        m_dragY = dragY;
        m_dragX = dragX;
    }

protected:
    void paintShape(QPainter& p, const QRectF& r) override {
        auto X = [&](double v) { return r.center().x() + v / kRange * r.width() / 2; };
        auto Y = [&](double v) { return r.center().y() - v / kRange * r.height() / 2; };
        p.setPen(QPen(theme::grid(), 1));
        for (double v : {-0.5, 0.5}) {
            p.drawLine(QPointF(X(v), r.top()), QPointF(X(v), r.bottom()));
            p.drawLine(QPointF(r.left(), Y(v)), QPointF(r.right(), Y(v)));
        }
        p.setPen(QPen(theme::gridStrong(), 1));
        p.drawLine(QPointF(X(0), r.top()), QPointF(X(0), r.bottom()));
        p.drawLine(QPointF(r.left(), Y(0)), QPointF(r.right(), Y(0)));
        p.setPen(QPen(theme::gridStrong(), 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(QPointF(X(-1), Y(1)), QPointF(X(1), Y(-1))));   // 0 dBFS
        p.drawLine(QPointF(X(-1), Y(-1)), QPointF(X(1), Y(1)));   // unity: no effect
        const double lv = std::min<double>(m_level, kRange);
        if (lv > 0.002) p.fillRect(QRectF(QPointF(X(-lv), r.top()), QPointF(X(lv), r.bottom())), theme::withAlpha(wetColor(), 14));
        QPainterPath all, used;
        for (int i = 0; i <= (int)r.width(); ++i) {
            const double x = (2.0 * i / r.width() - 1) * kRange;
            const QPointF pt(r.left() + i, Y(clamp(m_curve((float)x), -1.3f, 1.3f)));
            i ? all.lineTo(pt) : all.moveTo(pt);
            if (std::fabs(x) <= lv) used.elementCount() ? used.lineTo(pt) : used.moveTo(pt);
        }
        p.setPen(QPen(theme::withAlpha(wetColor(), 60), 7, Qt::SolidLine, Qt::RoundCap));
        p.drawPath(used);
        p.setPen(QPen(wetColor(), 2));
        p.drawPath(all);
        label(p, QRectF(r.left(), Y(0) - 16, r.width() - 6, 14), Qt::AlignRight | Qt::AlignVCenter, "in", theme::textDim());
        label(p, QRectF(X(0) + 6, r.top() + 4, 40, 14), Qt::AlignLeft | Qt::AlignVCenter, "out", theme::textDim());
    }
    std::function<float(float)> m_curve;
};

// Bit Crusher: one sine cycle and what the crusher makes of it; bits are the vertical steps, the downsampling the horizontal
class CrushDisplay : public ShaperDisplay {
public:
    explicit CrushDisplay(BitCrusher* b)
        : ShaperDisplay(b, b->tap, nullptr) {
        m_dragY = BitCrusher::pBits;
        m_dragX = BitCrusher::pDownsample;
    }

protected:
    void paintShape(QPainter& p, const QRectF& r) override {
        const float bits = value(BitCrusher::pBits), levels = std::pow(2.f, bits) * 0.5f;   // as BitCrusher::process quantises
        const int hold = std::max(1, (int)std::ceil(value(BitCrusher::pDownsample) - 1e-4f)), n = 128;   // samples in the cycle
        auto X = [&](double i) { return r.left() + r.width() * i / n; };
        auto Y = [&](double v) { return r.center().y() - v / kRange * r.height() / 2; };
        p.setPen(QPen(theme::grid(), 1));
        if (r.height() / 2 / kRange / levels >= 4)   // the quantisation levels and hold steps while they are far enough apart to see
            for (int q = (int)std::ceil(-kRange * levels); q <= (int)std::floor(kRange * levels); ++q)
                p.drawLine(QPointF(r.left(), Y(q / levels)), QPointF(r.right(), Y(q / levels)));
        if (r.width() / n * hold >= 4)
            for (int i = hold; i < n; i += hold) p.drawLine(QPointF(X(i), r.top()), QPointF(X(i), r.bottom()));
        p.setPen(QPen(theme::gridStrong(), 1));
        p.drawLine(QPointF(r.left(), Y(0)), QPointF(r.right(), Y(0)));
        auto sine = [&](double i) { return 0.9 * std::sin(2 * M_PI * i / n); };
        QPainterPath in, out;
        for (int i = 0; i <= 256; ++i) i ? in.lineTo(X(i / 2.0), Y(sine(i / 2.0))) : in.moveTo(X(0), Y(0));
        for (int i = 0; i < n; i += hold) {
            const double v = std::round(sine(i) * levels) / levels;
            i ? out.lineTo(X(i), Y(v)) : out.moveTo(X(i), Y(v));
            out.lineTo(X(std::min(i + hold, n)), Y(v));
        }
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(dryColor(), 1.2));
        p.drawPath(in);
        p.setPen(QPen(wetColor(), 2));
        p.drawPath(out);
        label(p, r.adjusted(8, 4, 0, 0), Qt::AlignLeft | Qt::AlignTop,
            QString("%1 bit   1/%2 rate").arg(bits, 0, 'f', bits < 15.95f && std::fabs(bits - std::round(bits)) > 0.05f ? 1 : 0).arg(hold),
            theme::textDim());
    }
};

// ---------------------------------------------------------------- Gain: a gain line over a dB scale, input and output meters
class GainDisplay : public Display {
public:
    GainDisplay(GainUtility* g)
        : Display(g, nullptr, {560, 240})
        , m_src(g->tap) {
        m_dragY = GainUtility::pGain;
        m_dragX = GainUtility::pPan;
        m_tap.read(m_src);
    }
    void tick() override {
        m_tap.read(m_src);
        for (int c = 0; c < 4; ++c) m_lv[c] = std::max(m_tap.peak(c), m_lv[c] * 0.89f);
        update();
    }

protected:
    static constexpr double kMeters = 132, kScale = 34;   // meter panel width, dB label column
    QRectF dragArea() const override { return QRectF(0, 0, width() - kMeters - 8, height()); }
    QRectF lineSpan() const { return dragArea().adjusted(kScale, 14, -12, -14); }   // where the gain line and its handle move
    // the gain scale: +-24 dB gets 80 % of the height, -24 .. -60 the rest
    double yOf(float db) const {
        const QRectF r = lineSpan();
        const double pos = db >= -24 ? 0.2 + 0.8 * (db + 24) / 48 : 0.2 * (db + 60) / 36;
        return r.bottom() - pos * r.height();
    }
    void resizeEvent(QResizeEvent*) override {
        m_spanY = (float)(lineSpan().height() * 0.8 / 48 * 84);   // the handle stays under the mouse between -24 and +24 dB
        m_spanX = (float)(lineSpan().width() - 20);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF f = dragArea(), s = lineSpan();
        box(p, f);
        for (int db : {24, 12, 6, 0, -6, -12, -24, -60}) {
            const double y = yOf(db);
            p.setPen(QPen(db ? theme::grid() : theme::gridStrong(), 1));
            p.drawLine(QPointF(s.left(), y), QPointF(f.right() - 1, y));
            label(p, QRectF(f.left(), y - 7, kScale - 6, 14), Qt::AlignRight | Qt::AlignVCenter, (db > 0 ? "+" : "") + QString::number(db),
                theme::textDim());
        }
        p.setPen(QPen(theme::grid(), 1, Qt::DashLine));
        p.drawLine(QPointF(s.center().x(), f.top() + 1), QPointF(s.center().x(), f.bottom() - 1));   // pan centre
        const float gain = value(GainUtility::pGain), pan = value(GainUtility::pPan);
        const bool mute = m_proc->params().getBool(GainUtility::pMute);
        const double gy = yOf(gain), y0 = yOf(0), hx = s.center().x() + pan * (s.width() - 20) / 2;
        const QColor c = mute ? theme::red() : gain >= 0 ? wetColor() : theme::accent2();
        p.fillRect(QRectF(QPointF(s.left(), std::min(gy, y0)), QPointF(f.right() - 1, std::max(gy, y0))), theme::withAlpha(c, 30));
        p.setPen(QPen(c, 2, mute ? Qt::DashLine : Qt::SolidLine));
        p.drawLine(QPointF(s.left(), gy), QPointF(f.right() - 1, gy));
        p.setPen(QPen(theme::surface(), 2));
        p.setBrush(c);
        p.drawEllipse(QPointF(hx, gy), 7, 7);
        QFont big = theme::boldFont();
        big.setPointSizeF(15);
        p.setFont(big);
        p.setPen(mute ? theme::red() : theme::text());
        const QRectF text(s.left() + 10, f.top() + 8, s.width() - 20, 24);
        p.drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
            mute ? QString("Muted") : QString("%1%2 dB").arg(gain > 0.05f ? "+" : "").arg(gain, 0, 'f', 1));
        label(p, text.translated(0, 22), Qt::AlignLeft | Qt::AlignTop,
            "Pan " + m_proc->formatValue(GainUtility::pPan) + "    Balance " + m_proc->formatValue(GainUtility::pBalance),
            theme::textDim());
        // meters: the input and the output, both sides
        const QRectF m(f.right() + 8, 0, kMeters, height());
        box(p, m);
        const QRectF bars = m.adjusted(34, 12, -10, -24);
        for (int db : {0, -6, -12, -24, -40}) {
            const double y = bars.bottom() - meterPos(db) * bars.height();
            label(p, QRectF(m.left(), y - 7, 28, 14), Qt::AlignRight | Qt::AlignVCenter, QString::number(db), theme::textDim());
            p.setPen(QPen(theme::grid(), 1));
            p.drawLine(QPointF(m.left() + 30, y), QPointF(bars.left() - 2, y));
        }
        const double bw = (bars.width() - 14) / 4;
        for (int i = 0; i < 4; ++i)
            meterBar(p, QRectF(bars.left() + i * bw + (i >= 2 ? 14 : 0), bars.top(), bw - 2, bars.height()), m_lv[i]);
        label(p, QRectF(bars.left(), bars.bottom() + 4, 2 * bw, 16), Qt::AlignCenter, "in", theme::textDim());
        label(p, QRectF(bars.left() + 2 * bw + 14, bars.bottom() + 4, 2 * bw, 16), Qt::AlignCenter, "out", theme::textDim());
        drawReadout(p, f, Qt::AlignTop | Qt::AlignRight);
    }
    TapCopy m_tap;
    const StereoTap& m_src;
    float m_lv[4] = {};   // peak meters with their fall: in L, in R, out L, out R
};

// ---------------------------------------------------------------- Stereo Width: a polar vectorscope with the width as a fan
// Mono points straight up, a side panned hard left or right lands on the 45-degree lines, out-of-phase content lies flat. The fan
// marks where a hard-panned source ends up at the current width (and mid / side gains); drag its edges sideways.
class WidthDisplay : public Display {
public:
    explicit WidthDisplay(StereoWidth* s)
        : Display(s, nullptr, {560, 240})
        , m_src(s->tap) {
        m_dragX = StereoWidth::pWidth;
        m_tap.read(m_src);
    }
    void tick() override {
        m_tap.read(m_src);
        float pk = 0;
        for (int i = kTap - 2048; i < kTap; ++i) pk = std::max({pk, std::fabs(m_tap.ch[OutL][i]), std::fabs(m_tap.ch[OutR][i])});
        m_scale = pk > m_scale ? pk : std::max(m_scale * 0.97f, 0.03f);   // follows the level so quiet material still fills it
        m_corr += (m_tap.correlation(2048) - m_corr) * 0.3f;
        update();
    }

protected:
    QPointF centre() const { return {width() / 2.0, height() - 34.0}; }
    double radius() const { return std::min(centre().y() - 16, width() / 2.0 - 30); }
    float dragSign(QPointF pos) const override { return pos.x() < centre().x() ? -1.f : 1.f; }
    void resizeEvent(QResizeEvent*) override { m_spanX = (float)radius(); }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        box(p, rect());
        const QPointF c = centre();
        const double R = radius();
        const QRectF disc(c.x() - R, c.y() - R, 2 * R, 2 * R);
        auto at = [&](double a, double rr) { return QPointF(c.x() + rr * std::sin(a), c.y() - rr * std::cos(a)); };   // a: from straight up
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(theme::grid(), 1));
        for (double f : {0.5, 1.0}) p.drawArc(QRectF(c.x() - R * f, c.y() - R * f, 2 * R * f, 2 * R * f), 0, 180 * 16);
        for (double a : {-M_PI / 4, M_PI / 4}) p.drawLine(c, at(a, R));
        p.setPen(QPen(theme::gridStrong(), 1));
        p.drawLine(at(-M_PI / 2, R), at(M_PI / 2, R));
        p.drawLine(c, at(0, R));
        label(p, QRectF(at(-M_PI / 4, R + 12) - QPointF(10, 8), QSizeF(20, 16)), Qt::AlignCenter, "L", theme::textDim());
        label(p, QRectF(at(M_PI / 4, R + 12) - QPointF(10, 8), QSizeF(20, 16)), Qt::AlignCenter, "R", theme::textDim());
        label(p, QRectF(at(0, R + 8) - QPointF(10, 8), QSizeF(20, 16)), Qt::AlignCenter, "M", theme::textDim());
        // the fan: a hard-left source's side / mid ratio after the effect is width * side gain / mid gain
        const double ratio = value(StereoWidth::pWidth) * dbToGain(value(StereoWidth::pSideGain)) / dbToGain(value(StereoWidth::pMidGain));
        const double a = std::atan(ratio), deg = a * 180 / M_PI;
        QPainterPath fan;
        fan.moveTo(c);
        fan.arcTo(disc, 90 + deg, -2 * deg);
        fan.closeSubpath();
        p.fillPath(fan, theme::withAlpha(wetColor(), hot() ? 34 : 22));
        // the samples, the lower half folded up
        QVector<QPointF> pts;
        pts.reserve(2048);
        const double k = R / (2 * m_scale);
        for (int i = kTap - 2048; i < kTap; ++i) {
            const float l = m_tap.ch[OutL][i], r = m_tap.ch[OutR][i];
            const double y = (l + r) * k, x = (r - l) * k;
            pts << (y < 0 ? QPointF(c.x() - x, c.y() + y) : QPointF(c.x() + x, c.y() - y));
        }
        p.save();
        p.setClipPath([&] {
            QPainterPath clip;
            clip.moveTo(c);
            clip.arcTo(disc, 0, 180);
            return clip;
        }());
        p.setPen(QPen(theme::withAlpha(dotColor(), 120), 1.6));
        p.drawPoints(pts.data(), (int)pts.size());
        p.restore();
        p.setPen(QPen(wetColor(), 2));
        for (double s : {-1.0, 1.0}) p.drawLine(c, at(s * a, R));
        p.setPen(QPen(theme::surface(), 1.5));
        p.setBrush(wetColor());
        for (double s : {-1.0, 1.0}) p.drawEllipse(at(s * a, R), 5, 5);
        // correlation under the scope
        const QRectF bar(c.x() - R, c.y() + 14, 2 * R, 8);
        p.fillRect(bar, theme::bg());
        const double cx = c.x() + clamp(m_corr, -1.f, 1.f) * R;
        p.fillRect(QRectF(QPointF(std::min(c.x(), cx), bar.top()), QPointF(std::max(c.x(), cx), bar.bottom())),
            m_corr >= 0 ? theme::green() : theme::red());
        p.setPen(QPen(theme::gridStrong(), 1));
        p.drawLine(QPointF(c.x(), bar.top() - 2), QPointF(c.x(), bar.bottom() + 2));
        label(p, QRectF(bar.left() - 26, bar.center().y() - 7, 22, 14), Qt::AlignRight | Qt::AlignVCenter, "-1", theme::textDim());
        label(p, QRectF(bar.right() + 4, bar.center().y() - 7, 22, 14), Qt::AlignLeft | Qt::AlignVCenter, "+1", theme::textDim());
        // readouts
        QFont big = theme::boldFont();
        big.setPointSizeF(15);
        p.setFont(big);
        p.setPen(theme::text());
        p.drawText(QRectF(12, 8, 160, 24), Qt::AlignLeft | Qt::AlignVCenter,
            QString::number(std::lround(value(StereoWidth::pWidth) * 100)) + " %");
        label(p, QRectF(12, 32, 160, 14), Qt::AlignLeft | Qt::AlignVCenter, "width", theme::textDim());
        p.setFont(big);
        p.setPen(theme::text());
        p.drawText(QRectF(width() - 172, 8, 160, 24), Qt::AlignRight | Qt::AlignVCenter,
            QString("%1%2").arg(m_corr >= 0.005f ? "+" : "").arg(m_corr, 0, 'f', 2));
        label(p, QRectF(width() - 172, 32, 160, 14), Qt::AlignRight | Qt::AlignVCenter, "correlation", theme::textDim());
    }
    TapCopy m_tap;
    const StereoTap& m_src;
    float m_scale = 0.03f, m_corr = 0;
};

// ---------------------------------------------------------------- Phase Inversion, Mono Conversion: both sides, before and after
// A lane per side with the input (dim) under the output; the effect's switches are buttons down the left edge.
class LaneDisplay : public Display {
public:
    struct Button {
        QString text;
        std::function<bool()> on;
        std::function<void()> press;
    };
    LaneDisplay(IProcessor* p, const StereoTap& tap, std::vector<Button> buttons)
        : Display(p, nullptr, {560, 170})
        , m_src(tap)
        , m_buttons(std::move(buttons)) {
        m_tap.read(tap);
    }
    void tick() override {
        m_tap.read(m_src);
        m_corr += (m_tap.correlation(2048) - m_corr) * 0.3f;
        update();
    }

protected:
    static constexpr double kGutter = 76;
    QRectF buttonRect(int i) const {
        const double h = (double)height() / m_buttons.size();
        return QRectF(0, i * h, kGutter, h).adjusted(0.5, i ? 3 : 0.5, -0.5, i + 1 < (int)m_buttons.size() ? -3 : -0.5);
    }
    bool click(QPointF pos) override {
        for (int i = 0; i < (int)m_buttons.size(); ++i)
            if (buttonRect(i).contains(pos)) {
                m_buttons[i].press();
                update();
                return true;
            }
        return false;
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        for (int i = 0; i < (int)m_buttons.size(); ++i) {
            const QRectF r = buttonRect(i);
            const bool on = m_buttons[i].on();
            p.setPen(QPen(on ? theme::mix(theme::border(), wetColor(), 0.5) : theme::border(), 1));
            p.setBrush(on ? theme::activeBrush(r) : QBrush(theme::raised()));
            p.drawRoundedRect(r, theme::radiusFor(r.height()), theme::radiusFor(r.height()));
            QFont f = theme::smallFont();
            f.setBold(on);
            p.setFont(f);
            p.setPen(on ? theme::text() : theme::textDim());
            p.drawText(r, Qt::AlignCenter, m_buttons[i].text);
        }
        const QRectF area(kGutter + 8, 0, width() - kGutter - 8, height());
        const double h = (area.height() - 6) / 2;
        const int n = 1024, from = m_tap.trigger(n);
        for (int side = 0; side < 2; ++side) {
            const QRectF lane(area.left(), area.top() + side * (h + 6), area.width(), h);
            box(p, lane);
            const QRectF r = lane.adjusted(1, 1, -1, -1);
            p.setPen(QPen(theme::gridStrong(), 1));
            p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
            p.save();
            p.setClipRect(r);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(dryColor(), 1.2));
            p.drawPath(trace(m_tap.ch[InL + side].data() + from, n, r));
            p.setPen(QPen(wetColor(), 1.8));
            p.drawPath(trace(m_tap.ch[OutL + side].data() + from, n, r));
            p.restore();
            label(p, r.adjusted(8, 3, 0, 0), Qt::AlignLeft | Qt::AlignTop, side ? "R" : "L", theme::text());
        }
        label(p, area.adjusted(0, 3, -8, 0), Qt::AlignRight | Qt::AlignTop,
            QString("correlation %1%2").arg(m_corr >= 0.005f ? "+" : "").arg(m_corr, 0, 'f', 2), theme::textDim());
    }
    TapCopy m_tap;
    const StereoTap& m_src;
    std::vector<Button> m_buttons;
    float m_corr = 0;
};

// ---------------------------------------------------------------- Spectrum Analyzer
// Log frequency, smooth between the sparse low bins, the loudest of the dense high ones (Smoothing widens that band), a falling
// peak trace; point at it for frequency, note and level.
class SpectrumDisplay : public Display {
public:
    explicit SpectrumDisplay(SpectrumAnalyzer* sa)
        : Display(sa, nullptr, {560, 240})
        , m_sa(sa) {}
    void tick() override {
        m_sa->readSpectrum(m_mag);
        const int w = std::max(2, (int)plot().width()), bins = (int)m_mag.size();
        m_db.assign(w, -200.f);
        if (m_peak.size() != (size_t)w) m_peak.assign(w, -200.f);
        if (bins > 4) {
            const double binHz = App::instance()->engine().sampleRate() / (2.0 * bins);
            const double half = std::max(std::log2(1000.0) / w / 2, value(SpectrumAnalyzer::pSmooth) / 6.0);   // octaves each side
            auto at = [&](int k) { return m_mag[clamp(k, 0, bins - 1)]; };
            for (int x = 0; x < w; ++x) {
                const double hz = 20 * std::pow(1000.0, (x + 0.5) / w), b0 = hz * std::exp2(-half) / binHz,
                             b1 = hz * std::exp2(half) / binHz;
                float db = -200.f;
                if (b1 - b0 < 2) {   // Catmull-Rom between the bins
                    const double b = hz / binHz;
                    const int i = (int)b;
                    const float t = (float)(b - i), p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
                    db = p1 + 0.5f * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
                } else   // the loudest bin in the band: a tone keeps its level, noise draws its upper edge
                    for (int k = (int)std::ceil(b0); k <= (int)b1 && k < bins; ++k) db = std::max(db, m_mag[k]);
                m_db[x] = db;
                m_peak[x] = std::max(db, m_peak[x] - 0.4f);   // 12 dB/s fall
            }
        }
        update();
    }

protected:
    QRectF plot() const { return QRectF(36, 8, width() - 46, height() - 28); }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        box(p, rect());
        const QRectF r = plot();
        const float range = value(SpectrumAnalyzer::pRange);
        auto X = [&](double hz) { return r.left() + std::log(hz / 20) / std::log(1000.0) * r.width(); };
        auto Y = [&](double db) { return r.top() - clamp(db, (double)-range, 0.0) / range * r.height(); };
        for (double dec : {10.0, 100.0, 1000.0, 10000.0})
            for (int m = 1; m <= 9; ++m) {
                const double hz = dec * m;
                if (hz < 20 || hz > 20000) continue;
                const bool major = m == 1 || m == 2 || m == 5;
                p.setPen(QPen(major ? theme::grid() : theme::withAlpha(theme::grid(), 110), 1));
                p.drawLine(QPointF(X(hz), r.top()), QPointF(X(hz), r.bottom()));
                if (major && hz > 20)
                    label(p, QRectF(X(hz) - 20, r.bottom() + 3, 40, 14), Qt::AlignCenter,
                        hz >= 1000 ? QString::number(hz / 1000) + "k" : QString::number(hz), theme::textDim());
            }
        const int step = range > 60 ? 12 : 6;
        for (int db = 0; db >= -range; db -= step) {
            p.setPen(QPen(db ? theme::grid() : theme::gridStrong(), 1));
            p.drawLine(QPointF(r.left(), Y(db)), QPointF(r.right(), Y(db)));
            label(p, QRectF(0, Y(db) - 7, r.left() - 6, 14), Qt::AlignRight | Qt::AlignVCenter, QString::number(db), theme::textDim());
        }
        if (!m_db.empty()) {
            QPainterPath line, peak;
            for (int x = 0; x < (int)m_db.size(); ++x) {
                const QPointF a(r.left() + x + 0.5, Y(m_db[x])), b(r.left() + x + 0.5, Y(m_peak[x]));
                x ? line.lineTo(a) : line.moveTo(a);
                x ? peak.lineTo(b) : peak.moveTo(b);
            }
            QPainterPath area = line;
            area.lineTo(r.right(), r.bottom());
            area.lineTo(r.left(), r.bottom());
            QLinearGradient g(r.topLeft(), r.bottomLeft());
            g.setColorAt(0, theme::withAlpha(theme::accent2(), 130));
            g.setColorAt(1, theme::withAlpha(theme::accent2(), 12));
            p.save();
            p.setClipRect(r);
            p.fillPath(area, g);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(theme::withAlpha(theme::text(), 80), 1));
            p.drawPath(peak);
            p.setPen(QPen(dotColor(), 1.5));
            p.drawPath(line);
            p.restore();
        }
        if (r.contains(m_mouse) && !m_db.empty()) {   // what is under the mouse
            const int x = clamp((int)(m_mouse.x() - r.left()), 0, (int)m_db.size() - 1);
            const double hz = 20 * std::pow(1000.0, (x + 0.5) / m_db.size());
            p.setPen(QPen(theme::withAlpha(theme::text(), 70), 1));
            p.drawLine(QPointF(m_mouse.x(), r.top()), QPointF(m_mouse.x(), r.bottom()));
            const QString s = QString("%1   %2   %3 dB")
                                  .arg(hz >= 1000 ? QString::number(hz / 1000, 'f', 2) + " kHz" : QString::number(hz, 'f', 0) + " Hz")
                                  .arg(theme::noteName((int)std::lround(69 + 12 * std::log2(hz / 440))))
                                  .arg(std::max(m_db[x], -range), 0, 'f', 1);
            p.setFont(theme::smallFont());
            const QSizeF sz = QFontMetricsF(p.font()).size(0, s) + QSizeF(14, 6);
            const QRectF c(clamp(m_mouse.x() + 10, r.left(), r.right() - sz.width()), r.top() + 6, sz.width(), sz.height());
            p.setPen(QPen(theme::border(), 1));
            p.setBrush(theme::withAlpha(theme::panel(), 235));
            p.drawRoundedRect(c, theme::radiusFor(c.height()), theme::radiusFor(c.height()));
            p.setPen(theme::text());
            p.drawText(c, Qt::AlignCenter, s);
        }
    }
    SpectrumAnalyzer* m_sa;
    std::vector<float> m_mag, m_db, m_peak;   // the analyzer's bins; per pixel column: now, peak
};

// ---------------------------------------------------------------- Peak / RMS Meter
// Two bars (RMS solid, peak lighter above it, the held peak as a line, a clip light), a shared scale between them, and the
// recent history of both; click to clear the maximum and the clip lights.
class MeterDisplay : public Display {
public:
    explicit MeterDisplay(MeterEffect* m)
        : Display(m, nullptr, {560, 240})
        , m_m(m) {}
    void tick() override {
        const auto lv = m_m->levels();
        m_pk[0] = std::max(lv.peakL, m_pk[0] * 0.89f);
        m_pk[1] = std::max(lv.peakR, m_pk[1] * 0.89f);
        m_rms[0] = lv.rmsL, m_rms[1] = lv.rmsR, m_hold[0] = lv.holdL, m_hold[1] = lv.holdR;
        m_max[0] = std::max(m_max[0], lv.peakL), m_max[1] = std::max(m_max[1], lv.peakR);
        m_hist[m_head] = {std::max(lv.peakL, lv.peakR), std::max(lv.rmsL, lv.rmsR)};
        m_head = (m_head + 1) % kHist;
        update();
    }

protected:
    static constexpr int kHist = 600;   // 20 s at 30 Hz
    static constexpr double kMeters = 150;
    bool click(QPointF) override {
        m_max[0] = m_max[1] = 0;
        return true;
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const double top = 18, bottom = height() - 24;
        auto Y = [&](float db) { return bottom - meterPos(db) * (bottom - top); };
        const QRectF m(0, 0, kMeters, height()), h(kMeters + 8, 0, width() - kMeters - 8, height());
        box(p, m);
        box(p, h);
        const int ticks[] = {6, 0, -6, -12, -18, -24, -30, -40, -60};
        for (int db : ticks) {
            label(p, QRectF(m.center().x() - 20, Y(db) - 7, 40, 14), Qt::AlignCenter, (db > 0 ? "+" : "") + QString::number(db),
                db >= 0 ? theme::text() : theme::textDim());
            p.setPen(QPen(db ? theme::grid() : theme::gridStrong(), 1));
            p.drawLine(QPointF(h.left() + 1, Y(db)), QPointF(h.right() - 1, Y(db)));
        }
        for (int c = 0; c < 2; ++c) {
            const QRectF bar(c ? m.right() - 44 : m.left() + 14, top, 30, bottom - top);
            meterBar(p, bar, m_pk[c], m_rms[c]);
            p.setPen(QPen(theme::text(), 1.5));
            p.drawLine(QPointF(bar.left(), Y(gainToDb(m_hold[c]))), QPointF(bar.right(), Y(gainToDb(m_hold[c]))));
            p.fillRect(QRectF(bar.left(), 6, bar.width(), 6), m_max[c] >= 1.f ? theme::red() : theme::withAlpha(theme::red(), 40));
            label(p, QRectF(bar.left() - 10, bottom + 4, bar.width() + 20, 16), Qt::AlignCenter, dbText(m_hold[c]),
                m_max[c] >= 1.f ? theme::red() : theme::text());
        }
        // history: newest on the right
        const QRectF g = h.adjusted(1, 0, -1, 0);
        QPainterPath peak, rms;
        for (int i = 0; i < kHist; ++i) {
            const auto& s = m_hist[(m_head + i) % kHist];
            const double x = g.left() + g.width() * i / (kHist - 1);
            const QPointF a(x, Y(gainToDb(s[0]))), b(x, Y(gainToDb(s[1])));
            i ? peak.lineTo(a) : peak.moveTo(a);
            i ? rms.lineTo(b) : rms.moveTo(b);
        }
        rms.lineTo(g.right(), bottom);
        rms.lineTo(g.left(), bottom);
        p.save();
        p.setClipRect(QRectF(g.left(), top, g.width(), bottom - top));
        p.fillPath(rms, theme::withAlpha(theme::accent2(), 90));
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(dotColor(), 1.3));
        p.drawPath(peak);
        p.restore();
        const float rmsNow = std::max(m_rms[0], m_rms[1]), maxNow = std::max(m_max[0], m_max[1]);
        label(p, h.adjusted(10, 3, -10, 0), Qt::AlignLeft | Qt::AlignTop,
            QString("peak %1     rms %2     max %3 dB").arg(dbText(std::max(m_pk[0], m_pk[1])), dbText(rmsNow), dbText(maxNow)),
            maxNow >= 1.f ? theme::red() : theme::text());
        label(p, QRectF(m.left(), bottom + 4, 14, 16), Qt::AlignCenter, "L", theme::textDim());
        label(p, QRectF(m.right() - 14, bottom + 4, 14, 16), Qt::AlignCenter, "R", theme::textDim());
    }
    MeterEffect* m_m;
    float m_pk[2] = {}, m_rms[2] = {}, m_hold[2] = {}, m_max[2] = {};
    std::array<float, 2> m_hist[kHist] = {};   // peak, rms
    int m_head = 0;
};

// ---------------------------------------------------------------- registrations
template <class E> E* as(IProcessor* p) { return static_cast<E*>(p); }
const char* kReset = "  ·  Shift: fine  ·  double-click: reset";

NativeEditorRegistration s_saturation({"native.saturation"}, [](IProcessor* p, QWidget* parent) {
    auto* s = as<Saturation>(p);
    return frame(p, parent,
        new CurveDisplay(
            p, s->tap, [s](float x) { return s->transfer(x); }, Saturation::pDrive, Saturation::pBias),
        QString("drag the curve: ↕ drive  ↔ bias") + kReset);
});
NativeEditorRegistration s_waveshaper({"native.waveshaper"}, [](IProcessor* p, QWidget* parent) {
    auto* s = as<Waveshaper>(p);
    return frame(p, parent,
        new CurveDisplay(
            p, s->tap, [s](float x) { return s->transfer(x); }, Waveshaper::pAmount, Waveshaper::pSymmetry),
        QString("drag the curve: ↕ amount  ↔ asymmetry") + kReset);
});
NativeEditorRegistration s_distortion({"native.distortion"}, [](IProcessor* p, QWidget* parent) {
    auto* s = as<Distortion>(p);
    return frame(p, parent,
        new CurveDisplay(
            p, s->tap, [s](float x) { return s->transfer(x); }, Distortion::pDrive, 0),
        QString("drag the curve: ↕ drive") + kReset);
});
NativeEditorRegistration s_bitcrusher({"native.bitcrusher"}, [](IProcessor* p, QWidget* parent) {
    return frame(p, parent, new CrushDisplay(as<BitCrusher>(p)), QString("drag the steps: ↕ bits  ↔ downsample") + kReset);
});
NativeEditorRegistration s_gain({"native.gain"}, [](IProcessor* p, QWidget* parent) {
    return frame(p, parent, new GainDisplay(as<GainUtility>(p)), QString("drag: ↕ gain  ↔ pan") + kReset);
});
NativeEditorRegistration s_width({"native.stereowidth"}, [](IProcessor* p, QWidget* parent) {
    return frame(p, parent, new WidthDisplay(as<StereoWidth>(p)), QString("drag the fan out or in: ↔ width") + kReset);
});
NativeEditorRegistration s_phase({"native.phaseinvert"}, [](IProcessor* p, QWidget* parent) {
    auto button = [p](const char* text, uint32_t id) {
        return LaneDisplay::Button{text, [p, id] { return p->params().getBool(id); },
            [p, id] { App::instance()->setProcessorParam(ownerOf(p), id, p->params().getBool(id) ? 0.f : 1.f, true); }};
    };
    return frame(p, parent,
        new LaneDisplay(p, as<PhaseInvert>(p)->tap,
            {button("Ø  Left", PhaseInvert::pLeft), button("Swap L / R", PhaseInvert::pSwap), button("Ø  Right", PhaseInvert::pRight)}),
        "click to flip a side's polarity or swap the sides", [](const ParamDesc&) { return false; });
});
NativeEditorRegistration s_mono({"native.mono"}, [](IProcessor* p, QWidget* parent) {
    std::vector<LaneDisplay::Button> modes;
    const ParamDesc* d = p->params().descById(MonoConversion::pMode);
    for (int i = 0; i < (int)d->choices.size(); ++i)
        modes.push_back({QString::fromStdString(d->choices[i]), [p, i] { return p->params().getInt(MonoConversion::pMode) == i; },
            [p, d, i] { App::instance()->setProcessorParam(ownerOf(p), MonoConversion::pMode, d->toNormalized((float)i), true); }});
    return frame(p, parent, new LaneDisplay(p, as<MonoConversion>(p)->tap, std::move(modes)), "click a mode; Mix blends it with the input",
        [](const ParamDesc& pd) { return pd.id != MonoConversion::pMode; });
});
NativeEditorRegistration s_spectrum({"native.spectrum"}, [](IProcessor* p, QWidget* parent) {
    return frame(p, parent, new SpectrumDisplay(as<SpectrumAnalyzer>(p)), "point at it for frequency, note and level");
});
NativeEditorRegistration s_meter({"native.meter"}, [](IProcessor* p, QWidget* parent) {
    return frame(p, parent, new MeterDisplay(as<MeterEffect>(p)), "click to clear the maximum and the clip lights");
});

}   // namespace
}   // namespace mt
