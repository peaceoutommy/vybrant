// Editors for the delay, reverb and modulation effects, FL Studio style: one large display that you drag on for the main
// parameters and that shows what the effect is doing right now, the other parameters as knobs below it.
#include "ui/PluginWindow.h"
#include "ui/Theme.h"
#include "dsp/effects/Effects.h"
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <complex>
#include <set>

namespace mt {
namespace {

using cd = std::complex<double>;

Id ownerOf(IProcessor* p) {
    for (auto& [id, proc] : App::instance()->engine().registry().processors())
        if (proc.get() == p) return id;
    return 0;
}
double bpmNow() { return App::instance()->engine().transport().bpm; }
double sampleRate() { return App::instance()->engine().sampleRate(); }
double frac(double x) { return x - std::floor(x); }
// the tempo-synced division (1 = 1/32 ... 14 = 2 bars) closest to a length in seconds
int nearestSync(double seconds, double bpm) {
    int best = 1;
    for (int c = 2; c < (int)kSyncChoices.size(); ++c)
        if (std::abs(std::log(syncDivisionSeconds(c, bpm) / seconds)) < std::abs(std::log(syncDivisionSeconds(best, bpm) / seconds)))
            best = c;
    return best;
}
QString timeText(double s) { return s < 1 ? QString::number(s * 1000, 'f', s < 0.01 ? 1 : 0) + " ms" : QString::number(s, 'g', 3) + " s"; }
QString percent(float v) { return QString::number(v * 100, 'f', 0) + " %"; }

void handle(QPainter& p, QPointF c, const QColor& col, bool hot) {
    p.setPen(QPen(col, 1.5));
    p.setBrush(hot ? col : theme::surface());
    p.drawEllipse(c, hot ? 5.5 : 4.5, hot ? 5.5 : 4.5);
}
void tag(QPainter& p, QPointF at, const QString& s) {   // the value beside a handle being dragged
    QRectF r(at.x() + 9, at.y() - 22, QFontMetricsF(p.font()).horizontalAdvance(s) + 12, 17);
    if (r.right() > p.window().right() - 4) r.moveRight(at.x() - 9);
    if (r.top() < 2) r.moveTop(at.y() + 9);
    theme::drawPanel(p, r, theme::withAlpha(theme::panelLight(), 235), 3);
    p.setPen(theme::text());
    p.drawText(r, Qt::AlignCenter, s);
}

// The display on top of every editor. A drag edits parameters live; releasing the mouse commits each parameter the gesture
// changed as one undo step.
class Display : public QWidget {
public:
    Display(IProcessor* p, QWidget* parent)
        : QWidget(parent)
        , m_proc(p)
        , m_owner(ownerOf(p)) {
        setObjectName("timeDisplay");   // the self-test drags on it
        setMinimumSize(440, 200);
        setMouseTracking(true);
        theme::roundedFrame(this);
    }
    QSize sizeHint() const override { return {560, 240}; }

protected:
    float get(uint32_t id) const { return m_proc->params().get(id); }
    int getInt(uint32_t id) const { return m_proc->params().getInt(id); }
    const ParamDesc& desc(uint32_t id) const { return *m_proc->params().descById(id); }
    void set(uint32_t id, float plain) {
        const float n = desc(id).toNormalized(plain);
        if (std::abs(n - m_proc->params().getNormalized(id)) < 1e-6f) return;   // no undo step for a value that did not move
        App::instance()->setProcessorParam(m_owner, id, n, false);
        m_touched.insert(id);
        update();
    }
    void mouseReleaseEvent(QMouseEvent*) override {
        for (uint32_t id : m_touched) App::instance()->setProcessorParam(m_owner, id, m_proc->params().getNormalized(id), true);
        m_touched.clear();
        m_dragging = false;
        update();
    }
    QRectF plot() const { return QRectF(rect()).adjusted(24, 26, -30, -20); }
    void begin(QPainter& p) const {
        p.fillRect(rect(), theme::surface());
        p.setRenderHint(QPainter::Antialiasing);
        p.setFont(theme::smallFont());
    }
    // "Name value   Name value" along the top edge: names dim, values bright
    void readout(QPainter& p, std::initializer_list<std::pair<QString, QString>> items) const {
        QFontMetricsF fm(p.font());
        double x = 10;
        for (auto& [name, value] : items) {
            p.setPen(theme::textDim());
            p.drawText(QPointF(x, 16), name);
            x += fm.horizontalAdvance(name + "  ");
            p.setPen(theme::text());
            p.drawText(QPointF(x, 16), value);
            x += fm.horizontalAdvance(value) + 16;
        }
    }
    void hint(QPainter& p, const QString& s) const {
        p.setPen(theme::textDim());
        p.drawText(QRectF(rect()).adjusted(0, 5, -10, 0), Qt::AlignRight | Qt::AlignTop, s);
    }
    IProcessor* m_proc;
    Id m_owner;
    std::set<uint32_t> m_touched;
    bool m_dragging = false;
};

// ---------------------------------------------------------------- delays: the echoes as bars along a time axis
// Left channel above the centre line, right below; a bar's height is that echo's level in dB, the faint bar at 0 the dry hit.
// Drag an echo: sideways sets the delay time (through echo n, so any echo can be grabbed; on the sync grid when synced), up
// or down its level - the first echo sets the mix, a later one the feedback that makes it that loud.
class TapView : public Display {
public:
    TapView(IProcessor* p, QWidget* parent)
        : Display(p, parent)
        , m_kind(p->key() == "native.pingpong" ? PingPong
                  : p->key() == "native.echo"  ? Multi
                                               : Mono)
        , m_mix(m_kind == Multi ? (uint32_t)Echo::pMix : (uint32_t)Delay::pMix) {
        setToolTip("Drag an echo sideways to set the delay time, up or down to set its level\n(the first echo sets the mix, a later "
                   "one the feedback)");
    }

private:
    static_assert(Delay::pTime == PingPongDelay::pTime && Delay::pTime == Echo::pTime && Delay::pSync == PingPongDelay::pSync &&
        Delay::pSync == Echo::pSync && Delay::pFeedback == PingPongDelay::pFeedback && Delay::pFeedback == Echo::pFeedback &&
        Delay::pMix == PingPongDelay::pMix);
    enum Kind { Mono, PingPong, Multi };
    struct Tap {
        double t;   // seconds after the dry hit
        float level;   // linear, wet
        int lane, n;   // 0 = left / 1 = right, echo number
        double off;   // the lane's extra loop time (Delay's stereo offset): echo n sits at n * (time + off)
        bool ghost;   // ping-pong crosstalk below 100 % width: drawn, not grabbed
    };
    struct Grab {
        int n = -1, lane = 0;
        double off = 0;
    };
    double period() const {   // the delay time the DSP runs at
        const double t = syncDivisionSeconds(getInt(Delay::pSync), bpmNow());
        return std::min(t > 0 ? t : get(Delay::pTime) * 0.001, m_kind == Multi ? 1.0 : 4.0);
    }
    // the impulse response at the echo times, as each effect's process() builds it (the cut filters left out)
    std::vector<Tap> model(double T, float fb, float mix, double until) const {
        std::vector<Tap> out;
        if (m_kind == Mono) {
            const double off = get(Delay::pStereoOffset) * 0.001;
            for (int lane = 0; lane < 2; ++lane) {
                const double o = std::max(0.0, lane ? off : -off);
                float g = mix;
                for (int n = 1; n < 64 && n * (T + o) < until && g > 1e-3f; ++n, g *= fb)
                    out.push_back({n * (T + o), g, lane, n, o, false});
            }
        } else if (m_kind == PingPong) {   // odd echoes leave on the left, even ones on the right
            const float w = get(PingPongDelay::pWidth);
            float g = mix;
            for (int n = 1; n < 64 && n * T < until && g > 1e-3f; ++n, g *= fb) {
                const int lane = (n - 1) % 2;
                out.push_back({n * T, g * (0.5f + w * 0.5f), lane, n, 0, false});
                if (w < 0.98f) out.push_back({n * T, g * (0.5f - w * 0.5f), 1 - lane, n, 0, true});
            }
        } else {   // Echo: taps at k * time that feed back together: y[n] = sum c_k buf[n - k], buf = impulse + fb * y
            const int taps = getInt(Echo::pTaps);
            const float spread = get(Echo::pSpread), shape = get(Echo::pDecayShape);
            for (int lane = 0; lane < 2; ++lane) {
                float c[9] = {}, buf[64] = {1};
                for (int k = 1; k <= taps; ++k) {
                    const float pan = ((k % 2) ? -1.f : 1.f) * spread * (float)k / taps;
                    c[k] = std::pow(1.f - shape, (float)(k - 1)) * (lane ? 0.5f + pan * 0.5f : 0.5f - pan * 0.5f);
                }
                for (int n = 1; n < 64 && n * T < until; ++n) {
                    float y = 0;
                    for (int k = 1; k <= std::min(taps, n); ++k) y += c[k] * buf[n - k];
                    buf[n] = fb * y;
                    if (mix * y > 1e-3f) out.push_back({n * T, mix * y, lane, n, 0, false});
                }
            }
        }
        return out;
    }
    std::vector<Tap> current() const { return model(period(), get(Delay::pFeedback), get(m_mix), m_window); }
    float levelOf(int n, int lane, float fb, float mix) const {
        for (auto& tp : model(1, fb, mix, 1e9))
            if (tp.n == n && tp.lane == lane && !tp.ghost) return tp.level;
        return 0;
    }
    void fitWindow(double T) {   // a time axis that shows the first few echoes; kept still while dragging
        const double off = m_kind == Mono ? std::abs(get(Delay::pStereoOffset)) * 0.001 : 0;
        const double span = (m_kind == Multi ? getInt(Echo::pTaps) + 2 : 6) * (T + off);
        for (double w : {0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0})
            if ((m_window = w) >= span) break;
    }
    double X(double t) const { return plot().left() + t / m_window * plot().width(); }
    double laneH() const { return plot().height() / 2; }
    double heightOf(float level) const {   // 0 dB at 85 % of the lane, -48 dB on the centre line
        return std::clamp((20 * std::log10(std::max(level, 1e-6f)) + 48) / 48 * 0.85, 0.0, 1.0) * laneH();
    }
    float levelAt(double h) const { return h < 2 ? 0.f : std::pow(10.f, (float)((h / laneH() / 0.85 * 48 - 48) / 20)); }
    const Tap* hit(const std::vector<Tap>& taps, QPointF pos) const {
        const int side = pos.y() > plot().center().y();
        const Tap* best = nullptr;
        double bestScore = 1e9;
        for (auto& tp : taps) {
            const double dx = std::abs(X(tp.t) - pos.x()), score = dx + (tp.lane != side ? 12 : 0);
            if (!tp.ghost && dx <= 10 && score < bestScore) best = &tp, bestScore = score;
        }
        return best;
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        begin(p);
        const double T = period();
        const int sync = getInt(Delay::pSync);
        if (!m_dragging) fitWindow(T);
        const QRectF r = plot();
        const double mid = r.center().y();
        if (sync > 0) {   // the beat grid: sixteenths, beats, bars
            const TransportView tv = App::instance()->engine().transport();
            const double sixteenth = 15 / tv.bpm, px = sixteenth / m_window * r.width();
            const int perBar = std::max(1, tv.tsNum * 16 / std::max(1, tv.tsDen));
            for (int k = 1; k * sixteenth < m_window; ++k) {
                const bool bar = k % perBar == 0, beat = k % 4 == 0;
                if (!beat && px < 6) continue;
                const double x = X(k * sixteenth);
                p.setPen(bar ? theme::mix(theme::gridStrong(), theme::textDim(), 0.35) : beat ? theme::gridStrong() : theme::grid());
                p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
                if (bar || (beat && px * 4 >= 40)) {
                    p.setPen(theme::textDim());
                    const int bars = k / perBar;
                    p.drawText(QPointF(x + 3, r.bottom() + 14),
                        !bar            ? QString("%1/4").arg(k / 4)
                            : bars == 1 ? QString("1 bar")
                                        : QString("%1 bars").arg(bars));
                }
            }
        } else {   // round times
            double step = 2;
            for (double s : {0.01, 0.02, 0.05, 0.1, 0.2, 0.25, 0.5, 1.0, 2.0})
                if (s / m_window * r.width() >= 56) {
                    step = s;
                    break;
                }
            for (double t = step; t < m_window - 1e-9; t += step) {
                p.setPen(theme::grid());
                p.drawLine(QPointF(X(t), r.top()), QPointF(X(t), r.bottom()));
                p.setPen(theme::textDim());
                p.drawText(QPointF(X(t) + 3, r.bottom() + 14), timeText(t));
            }
        }
        for (int db : {0, -12, -24, -36}) {
            const double h = heightOf(std::pow(10.f, db / 20.f));
            p.setPen(db == 0 ? QPen(theme::gridStrong(), 1, Qt::DashLine) : QPen(theme::grid()));
            p.drawLine(QPointF(r.left(), mid - h), QPointF(r.right(), mid - h));
            p.drawLine(QPointF(r.left(), mid + h), QPointF(r.right(), mid + h));
            p.setPen(theme::textDim());
            p.drawText(QPointF(r.right() + 5, mid - h + 4), QString::number(db));
            p.drawText(QPointF(r.right() + 5, mid + h + 4), QString::number(db));
        }
        p.setPen(theme::gridStrong());
        p.drawLine(QPointF(r.left(), mid), QPointF(r.right(), mid));
        p.setPen(theme::textDim());
        p.drawText(QPointF(r.left() - 15, mid - laneH() / 2 + 4), "L");
        p.drawText(QPointF(r.left() - 15, mid + laneH() / 2 + 4), "R");
        const double h0 = heightOf(1.f);
        theme::drawPanel(p, QRectF(r.left() - 1.5, mid - h0, 3, 2 * h0), theme::withAlpha(theme::textDim(), 110), 1.5);

        // the echoes; the ones the cursor just passed light up
        const float since = sinceHit();
        const auto taps = current();
        for (auto& tp : taps) {
            const double h = heightOf(tp.level), x = X(tp.t);
            if (h < 0.5 || x > r.right()) continue;
            const double glow = since >= tp.t ? std::max(0.0, 1 - (since - tp.t) / 0.25) : 0;
            QColor c = theme::mix(tp.lane ? theme::accent2() : theme::accent(), theme::playhead(), glow * 0.85);
            c.setAlpha(tp.ghost ? 110 : 245 - std::min(110, (tp.n - 1) * 9));
            theme::drawPanel(p, tp.lane ? QRectF(x - 3.5, mid + 1, 7, h) : QRectF(x - 3.5, mid - 1 - h, 7, h), c, 2.5);
        }
        if (since < m_window) {
            p.setPen(QPen(theme::withAlpha(theme::playhead(), 160), 1));
            p.drawLine(QPointF(X(since), r.top()), QPointF(X(since), r.bottom()));
        }
        const Grab& on = m_dragging ? m_grab : m_hover;
        for (auto& tp : taps) {   // handles on the first two echoes, and on the one under the mouse
            const bool hot = tp.n == on.n && tp.lane == on.lane;
            if (tp.ghost || (tp.n > 2 && !hot) || X(tp.t) > r.right()) continue;
            const QPointF top(X(tp.t), tp.lane ? mid + 1 + heightOf(tp.level) : mid - 1 - heightOf(tp.level));
            handle(p, top, tp.lane ? theme::accent2() : theme::accent(), hot);
            if (hot && m_dragging)
                tag(p, top,
                    timeText(tp.t) + "   " +
                        (tp.n == 1 ? "mix " + m_proc->formatValue(m_mix) : "feedback " + m_proc->formatValue(Delay::pFeedback)));
        }
        readout(p,
            {{"Time", sync > 0 ? QString::fromStdString(kSyncChoices[sync]) + "  (" + timeText(T) + ")" : timeText(T)},
                {"Feedback", m_proc->formatValue(Delay::pFeedback)}, {"Mix", m_proc->formatValue(m_mix)}});
        hint(p, "drag an echo:  ↔ time   ↕ level");
    }
    float sinceHit() const {
        switch (m_kind) {
        case Mono: return static_cast<Delay*>(m_proc)->sinceHit();
        case PingPong: return static_cast<PingPongDelay*>(m_proc)->sinceHit();
        default: return static_cast<Echo*>(m_proc)->sinceHit();
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        const auto taps = current();
        if (const Tap* tp = hit(taps, e->position())) {
            m_grab = {tp->n, tp->lane, tp->off};
            m_dragging = true;
        }
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!m_dragging) {
            const auto taps = current();
            const Tap* tp = hit(taps, e->position());
            const Grab h = tp ? Grab{tp->n, tp->lane, tp->off} : Grab{};
            if (h.n != m_hover.n || h.lane != m_hover.lane) {
                m_hover = h;
                setCursor(tp ? Qt::SizeAllCursor : Qt::ArrowCursor);
                update();
            }
            return;
        }
        const QRectF r = plot();
        const double T = std::max(0.001, (e->position().x() - r.left()) / r.width() * m_window / m_grab.n - m_grab.off);
        if (getInt(Delay::pSync) > 0) set(Delay::pSync, (float)nearestSync(T, bpmNow()));
        else set(Delay::pTime, (float)(T * 1000));
        const double h = m_grab.lane ? e->position().y() - r.center().y() : r.center().y() - e->position().y();
        const float target = levelAt(std::max(0.0, h)), mix = get(m_mix);
        if (m_grab.n == 1) {
            const float unit = levelOf(1, m_grab.lane, get(Delay::pFeedback), 1.f);
            if (unit > 0) set(m_mix, std::min(1.f, target / unit));
        } else if (mix > 0) {   // the feedback that puts echo n at the pointer (its level only grows with feedback)
            float lo = 0, hi = desc(Delay::pFeedback).max;
            for (int i = 0; i < 24; ++i) {
                const float m = (lo + hi) / 2;
                (levelOf(m_grab.n, m_grab.lane, m, mix) < target ? lo : hi) = m;
            }
            set(Delay::pFeedback, (lo + hi) / 2);
        }
    }
    void leaveEvent(QEvent*) override {
        m_hover = {};
        update();
    }

    const Kind m_kind;
    const uint32_t m_mix;
    double m_window = 2;
    Grab m_grab, m_hover;
};

// ---------------------------------------------------------------- reverb: the tail's energy decay
// Level in dB over a square-root time axis, so a 10 ms pre-delay and a 10 s tail both read. The second curve is the band
// around 5 kHz, which damping shortens and the high cut lowers; Size moves the onset (the combs' first echoes). Handles: the
// pre-delay line, the tail's end at -60 dB (Decay) and the highs at -30 dB (Damping).
class ReverbView : public Display {
public:
    ReverbView(IProcessor* p, QWidget* parent)
        : Display(p, parent) {
        setToolTip("Drag the pre-delay line, the end of the tail (decay time) or the highs' handle (damping)");
    }

private:
    using R = Reverb;
    static constexpr double kHighs = 5000;
    static constexpr double kFirstComb = 1116 / 44100.0, kLastComb = 1617 / 44100.0;   // Reverb::onPrepare's shortest and longest
    enum Handle { None = -1, Pre, Decay, Highs };
    double lenScale() const { return 0.5 + get(R::pSize); }
    double onset() const { return get(R::pPreDelay) * 0.001 + kFirstComb * lenScale(); }
    double build() const { return (kLastComb - kFirstComb) * lenScale(); }   // until every comb has answered
    double highsDecay(float damping) const {   // RT60 near 5 kHz: each comb's one-pole damping takes some dB on every loop
        const double d = damping * 0.6, w = 2 * M_PI * kHighs / sampleRate();
        const double perLoop = 20 * std::log10((1 - d) / std::abs(cd(1 - d * std::cos(w), d * std::sin(w))));
        return 60 / (60 / get(R::pDecay) - perLoop / (1378 / 44100.0 * lenScale()));   // 1378 = the average comb
    }
    double highsOffset() const { return -10 * std::log10(1 + std::pow(kHighs / get(R::pHighCut), 4)); }   // the high cut at 5 kHz
    double levelDb(double t, double rt, double offsetDb) const {
        const double t0 = onset(), b = build();
        if (t < t0) return -200;
        if (t < t0 + b) return offsetDb + 20 * std::log10(std::max(1e-4, (t - t0) / b));
        return offsetDb - 60 * (t - t0 - b) / rt;
    }
    double X(double t) const { return plot().left() + std::sqrt(std::max(0.0, t) / m_window) * plot().width(); }
    double tAt(double x) const {
        const double u = std::clamp((x - plot().left()) / plot().width(), 0.0, 1.0);
        return u * u * m_window;
    }
    double Y(double db) const { return plot().top() + std::clamp(-db / 72, 0.0, 1.0) * plot().height(); }
    QPointF handlePos(int h) const {
        const double end = onset() + build();
        if (h == Pre) return {X(get(R::pPreDelay) * 0.001), plot().top() + 6};
        if (h == Decay) return {X(end + get(R::pDecay)), Y(-60)};
        return {X(end + highsDecay(get(R::pDamping)) * std::max(0.0, 30 + highsOffset()) / 60), Y(-30)};
    }
    int hit(QPointF pos) const {
        int best = None;
        double bestD = 12;
        for (int h : {Decay, Highs})
            if (double d = QLineF(pos, handlePos(h)).length(); d < bestD) best = h, bestD = d;
        if (best == None && std::abs(pos.x() - handlePos(Pre).x()) <= 6) best = Pre;
        return best;
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        begin(p);
        if (!m_dragging) {
            const double span = (onset() + build() + get(R::pDecay)) * 1.1;
            for (double w : {0.5, 1.0, 2.0, 3.0, 5.0, 10.0, 20.0, 30.0})
                if ((m_window = w) >= span) break;
        }
        const QRectF r = plot();
        for (int db = 0; db >= -72; db -= 12) {
            p.setPen(db == -60 ? QPen(theme::gridStrong(), 1, Qt::DashLine) : QPen(theme::grid()));
            p.drawLine(QPointF(r.left(), Y(db)), QPointF(r.right(), Y(db)));
            p.setPen(theme::textDim());
            p.drawText(QPointF(r.right() + 5, Y(db) + 4), QString::number(db));
        }
        double lastX = -1e9;
        for (double t : {0.01, 0.02, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0}) {
            if (t > m_window || X(t) - lastX < 40) continue;
            lastX = X(t);
            p.setPen(theme::grid());
            p.drawLine(QPointF(lastX, r.top()), QPointF(lastX, r.bottom()));
            p.setPen(theme::textDim());
            p.drawText(QPointF(lastX + 3, r.bottom() + 14), timeText(t));
        }
        const double pre = get(R::pPreDelay) * 0.001, rt = get(R::pDecay), rtHigh = highsDecay(get(R::pDamping));
        p.fillRect(QRectF(r.left(), r.top(), X(pre) - r.left(), r.height()), theme::withAlpha(theme::gridStrong(), 70));

        auto tail = [&](double decay, double offsetDb) {
            QPolygonF top;
            for (double x = X(onset()); x <= r.right() + 0.5; x += 1.5) {
                const double db = levelDb(tAt(x), decay, offsetDb);
                top << QPointF(x, Y(db));
                if (db < -72 && x > X(onset() + build())) break;   // gone below the floor
            }
            return top;
        };
        auto draw = [&](const QPolygonF& top, const QColor& c, int alpha) {
            if (top.isEmpty()) return;
            QPolygonF area = top;
            area << QPointF(top.last().x(), r.bottom()) << QPointF(top.first().x(), r.bottom());
            QLinearGradient g(0, r.top(), 0, r.bottom());
            g.setColorAt(0, theme::withAlpha(c, alpha));
            g.setColorAt(1, theme::withAlpha(c, alpha / 5));
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawPolygon(area);
            p.setPen(QPen(c, 1.6));
            p.setBrush(Qt::NoBrush);
            p.drawPolyline(top);
        };
        draw(tail(rt, 0), theme::accent(), 80);
        draw(tail(rtHigh, highsOffset()), theme::accent2(), 100);

        const float since = static_cast<R*>(m_proc)->sinceHit();
        if (since < m_window) {   // the cursor runs along the tail from the last hit
            p.setPen(QPen(theme::withAlpha(theme::playhead(), 160), 1));
            p.drawLine(QPointF(X(since), r.top()), QPointF(X(since), r.bottom()));
            if (const double db = levelDb(since, rt, 0); db > -72) {
                p.setPen(Qt::NoPen);
                p.setBrush(theme::playhead());
                p.drawEllipse(QPointF(X(since), Y(db)), 3.5, 3.5);
            }
        }
        const int on = m_dragging ? m_grab : m_hover;
        p.setPen(QPen(theme::textDim(), 1, Qt::DashLine));
        p.drawLine(QPointF(X(pre), r.top()), QPointF(X(pre), r.bottom()));
        handle(p, handlePos(Pre), theme::text(), on == Pre);
        handle(p, handlePos(Decay), theme::accent(), on == Decay);
        handle(p, handlePos(Highs), theme::accent2(), on == Highs);
        if (m_dragging)
            tag(p, handlePos(m_grab),
                m_grab == Pre         ? "pre-delay " + m_proc->formatValue(R::pPreDelay)
                    : m_grab == Decay ? "decay " + m_proc->formatValue(R::pDecay)
                                      : "damping " + m_proc->formatValue(R::pDamping) + "  (highs " + timeText(rtHigh) + ")");
        readout(p,
            {{"Pre-delay", m_proc->formatValue(R::pPreDelay)}, {"Decay", m_proc->formatValue(R::pDecay)},
                {"Damping", m_proc->formatValue(R::pDamping)}});
        QFontMetricsF fm(p.font());   // legend
        double x = width() - 10 - fm.horizontalAdvance("full band     highs (5 kHz)") - 22;
        for (auto [name, c] : {std::pair{QString("full band"), theme::accent()}, std::pair{QString("highs (5 kHz)"), theme::accent2()}}) {
            theme::drawPanel(p, QRectF(x, 9, 8, 8), c, 2);
            p.setPen(theme::textDim());
            p.drawText(QPointF(x + 12, 16), name);
            x += 12 + fm.horizontalAdvance(name) + 14;
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        m_grab = hit(e->position());
        m_dragging = m_grab != None;
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!m_dragging) {
            if (const int h = hit(e->position()); h != m_hover) {
                m_hover = h;
                setCursor(h == Pre ? Qt::SizeHorCursor : h != None ? Qt::SizeAllCursor : Qt::ArrowCursor);
                update();
            }
            return;
        }
        const double t = tAt(e->position().x()), end = onset() + build();
        if (m_grab == Pre) set(R::pPreDelay, (float)(t * 1000));
        else if (m_grab == Decay) set(R::pDecay, (float)(t - end));
        else if (const double share = (30 + highsOffset()) / 60; share > 0.02) {   // the damping that ends the highs there
            const double want = std::max(0.0, t - end) / share;
            float lo = 0, hi = 1;
            for (int i = 0; i < 24; ++i) {
                const float m = (lo + hi) / 2;
                (highsDecay(m) > want ? lo : hi) = m;
            }
            set(R::pDamping, (lo + hi) / 2);
        }
    }
    void leaveEvent(QEvent*) override {
        m_hover = None;
        update();
    }

    double m_window = 5;
    int m_grab = None, m_hover = None;
};

// ---------------------------------------------------------------- modulation: the LFO, and what it moves
// Left: the LFO over time, one cycle bright, the live phase as a cursor (dot = left channel, ring = right). Drag the wave:
// sideways stretches the cycle (the rate; steps through the divisions when synced), up or down sets the depth.
// Right, live: the chorus voices' delay times, the flanger's comb, the phaser's notches, the tremolo's level, the vibrato's pitch.
uint32_t lfoSyncParam(const QString& key) {
    static_assert(Tremolo::pSync == Vibrato::pSync);
    return key == "native.flanger" ? Flanger::pSync : key == "native.phaser" ? Phaser::pSync : key == "native.chorus" ? 0 : Tremolo::pSync;
}
class LfoView : public Display {
public:
    LfoView(IProcessor* p, QWidget* parent)
        : Display(p, parent)
        , m_sync(lfoSyncParam(p->key())) {
        const QString k = p->key();
        m_kind = k == "native.chorus" ? kChorus
            : k == "native.flanger"   ? kFlanger
            : k == "native.phaser"    ? kPhaser
            : k == "native.tremolo"   ? kTremolo
                                      : kVibrato;
        m_shape = m_kind == kTremolo ? Tremolo::pShape : m_kind == kVibrato ? Vibrato::pShape : 0;
        setToolTip("Drag the wave: sideways sets the rate, up or down the depth");
    }

private:
    static constexpr uint32_t kRate = 1, kDepth = 2;
    static_assert(Chorus::pRate == kRate && Flanger::pRate == kRate && Phaser::pRate == kRate && Tremolo::pRate == kRate &&
        Vibrato::pRate == kRate && Chorus::pDepth == kDepth && Flanger::pDepth == kDepth && Phaser::pDepth == kDepth &&
        Tremolo::pDepth == kDepth && Vibrato::pDepth == kDepth);
    enum Kind { kChorus, kFlanger, kPhaser, kTremolo, kVibrato };
    double rate() const {
        const double s = m_sync ? syncDivisionSeconds(getInt(m_sync), bpmNow()) : 0;
        return s > 0 ? 1 / s : get(kRate);
    }
    int shape() const { return m_shape ? getInt(m_shape) : 0; }
    double phase(int voice = 0) const {
        switch (m_kind) {
        case kChorus: return static_cast<Chorus*>(m_proc)->lfoPhase(voice);
        case kFlanger: return static_cast<Flanger*>(m_proc)->lfoPhase();
        case kPhaser: return static_cast<Phaser*>(m_proc)->lfoPhase();
        case kTremolo: return static_cast<Tremolo*>(m_proc)->lfoPhase();
        default: return static_cast<Vibrato*>(m_proc)->lfoPhase();
        }
    }
    float held() const {
        return m_kind == kTremolo ? static_cast<Tremolo*>(m_proc)->lfoHeld()
            : m_kind == kVibrato  ? static_cast<Vibrato*>(m_proc)->lfoHeld()
                                  : 0.f;
    }
    double stereoOffset() const {   // how far the right channel's LFO runs ahead, in cycles
        switch (m_kind) {
        case kFlanger: return get(Flanger::pPhase) / 360;
        case kPhaser: return get(Phaser::pSpread) * 0.5;
        case kTremolo: return get(Tremolo::pStereoPhase) / 360;
        case kVibrato: return get(Vibrato::pStereoPhase) / 360;
        default: return 0;
        }
    }
    // the LFO at a phase, as dsp::LFO computes it; S&H draws a fixed pseudo-random step per cycle
    static float wave(int shape, double ph, int cycle = 0) {
        switch (shape) {
        case 1: return (float)(ph < 0.5 ? ph * 4 - 1 : 3 - ph * 4);
        case 2: return (float)(ph * 2 - 1);
        case 3: return ph < 0.5 ? 1.f : -1.f;
        case 4: return (float)(((uint32_t)(cycle + 7) * 2654435761u >> 16) & 0xffff) / 32767.5f - 1;
        default: return (float)std::sin(ph * 2 * M_PI);
        }
    }
    float leftValue(double ph) const { return shape() == 4 ? held() : wave(shape(), ph); }
    float rightValue(double ph) const {   // each effect derives its right channel differently
        if (m_kind == kChorus) return -wave(0, ph);
        if (m_kind == kTremolo) return shape() == 4 ? held() : wave(shape(), frac(ph + stereoOffset()));
        return (float)std::sin(2 * M_PI * (ph + stereoOffset()));
    }
    QRectF lfoRect() const {
        const QRectF r = plot();
        return {r.left(), r.top(), r.width() * 0.6 - 10, r.height()};
    }
    QRectF targetRect() const {
        const QRectF r = plot();
        const double x = r.left() + r.width() * 0.6 + 10;
        return {x, r.top(), r.right() - x, r.height()};
    }
    double cycleWidth(double hz) const {   // log: the slowest rate spans the panel, the fastest 10 px
        return 10 * std::pow(lfoRect().width() / 10, 1 - desc(kRate).toNormalized((float)hz));
    }
    double rateFor(double cycleW) const {
        return desc(kRate).fromNormalized((float)(1 - std::log(std::max(cycleW, 10.0) / 10) / std::log(lfoRect().width() / 10)));
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        begin(p);
        const QRectF a = lfoRect(), b = targetRect();
        const double mid = a.center().y(), half = a.height() / 2 - 6, cw = cycleWidth(rate()), depth = get(kDepth);
        const int shp = shape();
        p.setPen(theme::grid());
        for (double x = a.left() + cw; x < a.right() && cw >= 8; x += cw) p.drawLine(QPointF(x, a.top()), QPointF(x, a.bottom() - 12));
        for (double y : {mid - half, mid + half}) p.drawLine(QPointF(a.left(), y), QPointF(a.right(), y));
        p.setPen(theme::gridStrong());
        p.drawLine(QPointF(a.left(), mid), QPointF(a.right(), mid));
        p.setPen(QPen(theme::withAlpha(theme::accent2(), 170), 1, Qt::DashLine));
        for (double y : {mid - depth * half, mid + depth * half}) p.drawLine(QPointF(a.left(), y), QPointF(a.right(), y));

        QPolygonF all, first;
        for (double x = a.left(); x <= a.right(); x += 1) {
            const double c = (x - a.left()) / cw;
            const QPointF pt(x, mid - wave(shp, c - std::floor(c), (int)c) * depth * half);
            all << pt;
            if (c <= 1) first << pt;
        }
        QPolygonF area = all;
        area << QPointF(a.right(), mid) << QPointF(a.left(), mid);
        p.setPen(Qt::NoPen);
        p.setBrush(theme::withAlpha(theme::accent(), 28));
        p.drawPolygon(area);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(theme::withAlpha(theme::accent(), 120), 1.3));
        p.drawPolyline(all);
        p.setPen(QPen(theme::accent(), 2));
        p.drawPolyline(first);

        // one cycle, bracketed along the bottom with the rate
        const bool synced = m_sync && getInt(m_sync) > 0;
        const QString rateText = synced ? QString::fromStdString(kSyncChoices[getInt(m_sync)]) : m_proc->formatValue(kRate);
        const double by = a.bottom() - 4, bx = a.left() + std::min(cw, a.width());
        p.setPen(theme::textDim());
        p.drawLine(QPointF(a.left(), by), QPointF(bx, by));
        p.drawLine(QPointF(a.left(), by - 3), QPointF(a.left(), by + 3));
        p.drawLine(QPointF(bx, by - 3), QPointF(bx, by + 3));
        const double tw = QFontMetricsF(p.font()).horizontalAdvance(rateText);
        p.drawText(QPointF(bx - a.left() > tw + 12 ? (a.left() + bx - tw) / 2 : bx + 5, by - 4), rateText);

        const double ph = phase();
        const QPointF dot(a.left() + ph * std::min(cw, a.width()), mid - leftValue(ph) * depth * half);
        const QPointF ring(
            a.left() + (m_kind == kChorus ? ph : frac(ph + stereoOffset())) * std::min(cw, a.width()), mid - rightValue(ph) * depth * half);
        p.setPen(QPen(theme::withAlpha(theme::playhead(), 110), 1));
        p.drawLine(QPointF(dot.x(), a.top()), QPointF(dot.x(), a.bottom() - 12));
        p.setPen(QPen(theme::playhead(), 1.5));
        p.setBrush(theme::surface());
        p.drawEllipse(ring, 3.5, 3.5);
        p.setPen(Qt::NoPen);
        p.setBrush(theme::playhead());
        p.drawEllipse(dot, 4, 4);

        p.setPen(theme::gridStrong());
        p.drawLine(QPointF(a.right() + 10, a.top()), QPointF(a.right() + 10, a.bottom()));
        switch (m_kind) {
        case kChorus: chorusVoices(p, b); break;
        case kFlanger:
        case kPhaser: response(p, b, ph); break;
        case kTremolo: level(p, b, ph); break;
        case kVibrato: pitch(p, b, ph); break;
        }
        readout(p, {{synced ? "Sync" : "Rate", rateText}, {"Depth", percent((float)depth)}});
        if (m_dragging) tag(p, m_last, rateText + "   depth " + percent((float)depth));
    }
    void title(QPainter& p, const QRectF& b, const QString& s) const {
        p.setPen(theme::textDim());
        p.drawText(QPointF(b.left(), 16), s);
    }
    void liveDots(QPainter& p, QPointF left, QPointF right) const {
        p.setPen(QPen(theme::playhead(), 1.5));
        p.setBrush(theme::surface());
        p.drawEllipse(right, 3.5, 3.5);
        p.setPen(Qt::NoPen);
        p.setBrush(theme::playhead());
        p.drawEllipse(left, 4, 4);
    }
    // chorus: each voice sweeps its delay between Delay and Delay + 8 ms x Depth (right channel mirrored)
    void chorusVoices(QPainter& p, const QRectF& b) const {
        title(p, b, "Delay per voice (ms)");
        const int voices = getInt(Chorus::pVoices);
        const double base = get(Chorus::pDelay), dep = get(Chorus::pDepth) * 4;
        double range = 50;
        for (double r : {10.0, 20.0, 30.0})
            if (base + 2 * dep + 2 <= r) {
                range = r;
                break;
            }
        auto X = [&](double ms) { return b.left() + ms / range * b.width(); };
        for (int ms = 0; ms < range; ms += range > 20 ? 10 : 5) {
            p.setPen(theme::grid());
            p.drawLine(QPointF(X(ms), b.top()), QPointF(X(ms), b.bottom()));
            p.setPen(theme::textDim());
            p.drawText(QPointF(X(ms) + 3, b.bottom() + 14), QString::number(ms));
        }
        const double rowH = b.height() / 4;
        for (int v = 0; v < voices; ++v) {
            const double y = b.center().y() + (v - (voices - 1) / 2.0) * rowH, m = std::sin(2 * M_PI * phase(v));
            theme::drawPanel(p, QRectF(X(base) - 4, y - 5, X(base + 2 * dep) - X(base) + 8, 10), theme::withAlpha(theme::accent2(), 70), 5);
            liveDots(p, QPointF(X(base + (m + 1) * dep), y), QPointF(X(base + (1 - m) * dep), y));
        }
    }
    // flanger / phaser: the frequency response at the LFO's current position, left bright, right in the second colour
    void response(QPainter& p, const QRectF& b, double ph) const {
        const bool fl = m_kind == kFlanger;
        const double sr = sampleRate(), depth = get(kDepth);
        const double delayMs = fl ? get(Flanger::pDelay) + (std::sin(2 * M_PI * ph) + 1) * depth * 5 : 0;
        title(p, b, fl ? QString("Comb filter, now (%1 ms)").arg(delayMs, 0, 'f', 1) : QString("Notches, now"));
        // the flanger's comb on a linear 0 - 3 kHz axis (evenly spaced teeth), the phaser's few notches on a log one
        auto F = [&](double x) { return fl ? 3000 * (x - b.left()) / b.width() : 20 * std::pow(1000.0, (x - b.left()) / b.width()); };
        auto X = [&](double hz) {
            return fl ? b.left() + hz / 3000 * b.width() : b.left() + std::log(hz / 20) / std::log(1000.0) * b.width();
        };
        const double top = fl ? 12 : 6, span = fl ? 30 : 24;
        auto Y = [&](double db) { return b.top() + std::clamp((top - db) / span, 0.0, 1.0) * b.height(); };
        for (double hz : fl ? std::initializer_list<double>{1000, 2000} : std::initializer_list<double>{100, 1000, 10000}) {
            p.setPen(theme::grid());
            p.drawLine(QPointF(X(hz), b.top()), QPointF(X(hz), b.bottom()));
            p.setPen(theme::textDim());
            p.drawText(QPointF(X(hz) + 3, b.bottom() + 14), hz >= 1000 ? QString::number(hz / 1000) + "k" : QString::number(hz));
        }
        for (int db = 0; db > top - span; db -= fl ? 12 : 6) {
            p.setPen(db ? theme::grid() : theme::gridStrong());
            p.drawLine(QPointF(b.left(), Y(db)), QPointF(b.right(), Y(db)));
        }
        const double fb = get(fl ? (uint32_t)Flanger::pFeedback : (uint32_t)Phaser::pFeedback);
        const double mix = get(fl ? (uint32_t)Flanger::pMix : (uint32_t)Phaser::pMix);
        auto curve = [&](double m) {
            dsp::Biquad st[12];
            const int stages = fl ? 0 : getInt(Phaser::pStages);
            const double fc = fl ? 0 : get(Phaser::pCenter) * std::pow(2.0, m * depth * 2);
            for (int s = 0; s < stages; ++s) st[s].set(dsp::Biquad::AllPass, sr, (float)(fc * (1 + s * 0.3)), 0.5f);
            const double D = fl ? get(Flanger::pDelay) * 0.001 * sr + 1 + (m + 1) * depth * 0.005 * sr : 0;   // samples
            QPolygonF poly;
            for (double x = b.left(); x <= b.right(); x += 1) {
                const double hz = std::max(1.0, F(x)), w = 2 * M_PI * hz / sr;
                if (hz >= sr / 2) break;
                const cd z1 = std::polar(1.0, -w);
                cd h;
                if (fl) {   // out = dry (1 - mix/2) + mix z^-D / (1 - fb z^-D)
                    const cd zd = std::polar(1.0, -w * D);
                    h = (1 - mix / 2) + mix * zd / (1.0 - fb * zd);
                } else {   // the all-pass chain A with one sample of feedback: dry (1 - mix/2) + mix/2 A / (1 - fb z^-1 A)
                    cd ap = 1;
                    for (int s = 0; s < stages; ++s)
                        ap *= ((double)st[s].b0 + (double)st[s].b1 * z1 + (double)st[s].b2 * z1 * z1) /
                            (1.0 + (double)st[s].a1 * z1 + (double)st[s].a2 * z1 * z1);
                    h = (1 - mix / 2) + mix / 2 * ap / (1.0 - fb * z1 * ap);
                }
                poly << QPointF(x, Y(20 * std::log10(std::abs(h) + 1e-9)));
            }
            return poly;
        };
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(theme::withAlpha(theme::accent2(), 200), 1.2));
        p.drawPolyline(curve(std::sin(2 * M_PI * (ph + stereoOffset()))));
        QPolygonF left = curve(std::sin(2 * M_PI * ph)), area = left;
        area << QPointF(left.last().x(), b.bottom()) << QPointF(b.left(), b.bottom());
        p.setPen(Qt::NoPen);
        p.setBrush(theme::withAlpha(theme::accent(), 28));
        p.drawPolygon(area);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(theme::accent(), 1.7));
        p.drawPolyline(left);
    }
    // tremolo: each channel's gain right now, inside the band the depth lets it swing through
    void level(QPainter& p, const QRectF& b, double ph) const {
        title(p, b, "Level, now");
        const double depth = get(kDepth), w = std::min(34.0, b.width() / 5);
        for (int ch = 0; ch < 2; ++ch) {
            const double gain = 1 - depth * (0.5 - (ch ? rightValue(ph) : leftValue(ph)) * 0.5);
            const QRectF col(b.center().x() + (ch ? 0.5 : -1.5) * w, b.top() + 4, w, b.height() - 8);
            theme::drawPanel(p, col, theme::withAlpha(theme::gridStrong(), 110), 3);
            theme::drawPanel(p, QRectF(col.left(), col.bottom() - gain * col.height(), w, gain * col.height()),
                ch ? theme::accent2() : theme::accent(), 3);
            p.setPen(theme::textDim());
            p.drawText(QRectF(col.left(), col.bottom() + 2, w, 14), Qt::AlignCenter, ch ? "R" : "L");
        }
        const double floorY = b.top() + 4 + depth * (b.height() - 8);   // the lowest the gain swings to
        p.setPen(QPen(theme::withAlpha(theme::accent2(), 170), 1, Qt::DashLine));
        p.drawLine(QPointF(b.center().x() - 1.5 * w - 6, floorY), QPointF(b.center().x() + 1.5 * w + 6, floorY));
        p.setPen(theme::textDim());
        p.drawText(QPointF(b.center().x() + 1.5 * w + 10, floorY + 4),
            QString::number(20 * std::log10(std::max(1e-3, 1 - depth)), 'f', 1) + " dB");
    }
    // vibrato: the pitch the delay sweep causes over one cycle (slope of the delay time), with the live position
    void pitch(QPainter& p, const QRectF& b, double ph) const {
        title(p, b, "Pitch over one cycle");
        const double k = get(kDepth) * 0.01 * rate();   // the delay swing (Depth x 10 ms) per second of LFO
        auto cents = [&](int shp, double q) {
            const double slope = shp == 1 ? (q < 0.5 ? 4 : -4) : shp == 2 ? 2 : shp >= 3 ? 0 : 2 * M_PI * std::cos(2 * M_PI * q);
            return 1200 * std::log2(std::max(0.01, 1 - k * slope));
        };
        const int shp = shape();
        double peak = 1;
        for (int i = 0; i < 64; ++i) peak = std::max({peak, std::abs(cents(shp, i / 64.0)), std::abs(cents(0, i / 64.0))});
        double scale = 8000;
        for (double s : {10.0, 25.0, 50.0, 100.0, 200.0, 400.0, 800.0, 1600.0})
            if (s >= peak) {
                scale = s;
                break;
            }
        auto Y = [&](double c) { return b.center().y() - c / scale * (b.height() / 2 - 6); };
        p.setPen(theme::grid());
        for (double c : {scale, -scale}) p.drawLine(QPointF(b.left(), Y(c)), QPointF(b.right(), Y(c)));
        p.setPen(theme::gridStrong());
        p.drawLine(QPointF(b.left(), Y(0)), QPointF(b.right(), Y(0)));
        p.setPen(theme::textDim());
        p.drawText(QRectF(b.left(), Y(scale) + 2, b.width() - 2, 14), Qt::AlignRight, QString("+%1 ct").arg(scale));
        p.drawText(QRectF(b.left(), Y(-scale) - 16, b.width() - 2, 14), Qt::AlignRight, QString("-%1 ct").arg(scale));
        QPolygonF poly;
        for (double x = b.left(); x <= b.right(); x += 1) poly << QPointF(x, Y(cents(shp, (x - b.left()) / b.width())));
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(theme::accent(), 1.7));
        p.drawPolyline(poly);
        const double qR = frac(ph + stereoOffset());
        p.setPen(QPen(theme::withAlpha(theme::playhead(), 110), 1));
        p.drawLine(QPointF(b.left() + ph * b.width(), b.top()), QPointF(b.left() + ph * b.width(), b.bottom()));
        liveDots(p, QPointF(b.left() + ph * b.width(), Y(cents(shp, ph))), QPointF(b.left() + qR * b.width(), Y(cents(0, qR))));
    }

    void mousePressEvent(QMouseEvent* e) override {
        const QRectF a = lfoRect();
        if (!a.contains(e->position())) return;
        m_dragging = true;
        m_press = m_last = e->position();
        m_pressCycles = std::max(0.5, (m_press.x() - a.left()) / cycleWidth(rate()));
        m_pressDepth = get(kDepth);
        setCursor(Qt::ClosedHandCursor);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        const QRectF a = lfoRect();
        if (!m_dragging) {
            setCursor(a.contains(e->position()) ? Qt::OpenHandCursor : Qt::ArrowCursor);
            return;
        }
        m_last = e->position();
        const double hz = rateFor((m_last.x() - a.left()) / m_pressCycles);   // the grabbed point of the wave stays under the pointer
        if (m_sync && getInt(m_sync) > 0) set(m_sync, (float)nearestSync(1 / hz, bpmNow()));
        else set(kRate, (float)hz);
        const double mid = a.center().y(), half = a.height() / 2 - 6;
        set(kDepth, std::clamp(m_pressDepth + (float)((std::abs(m_last.y() - mid) - std::abs(m_press.y() - mid)) / half), 0.f, 1.f));
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        Display::mouseReleaseEvent(e);
        setCursor(lfoRect().contains(e->position()) ? Qt::OpenHandCursor : Qt::ArrowCursor);
    }

    Kind m_kind;
    const uint32_t m_sync;
    uint32_t m_shape = 0;
    QPointF m_press, m_last;
    double m_pressCycles = 1;
    float m_pressDepth = 0;
};

// Header (name, and the Sync choice when the effect has one), the display, then the knobs for everything the display does
// not edit. 'onDisplay' lists the parameters the display edits.
template <class View> QWidget* editor(IProcessor* p, QWidget* parent, std::vector<uint32_t> onDisplay, uint32_t syncId = 0) {
    App& app = *App::instance();
    const Id owner = ownerOf(p);
    auto* w = new QWidget(parent);
    auto* lay = new QVBoxLayout(w);
    lay->setContentsMargins(6, 4, 6, 6);
    lay->setSpacing(4);
    auto* head = new QHBoxLayout();
    auto* name = new QLabel(p->displayName(), w);
    name->setFont(theme::smallFont());
    head->addWidget(name);
    head->addStretch(1);
    QComboBox* sync = nullptr;
    if (syncId) {
        head->addWidget(theme::dimLabel("Sync", w, "font-size:8pt;"));
        sync = new QComboBox(w);
        sync->setFixedHeight(20);
        sync->setToolTip("Tempo sync: off uses the time / rate set on the display");
        for (auto& c : p->params().descById(syncId)->choices) sync->addItem(QString::fromStdString(c));
        sync->setCurrentIndex(p->params().getInt(syncId));
        QObject::connect(sync, &QComboBox::currentIndexChanged, w, [&app, owner, p, syncId](int i) {
            app.setProcessorParam(owner, syncId, p->params().descById(syncId)->toNormalized((float)i), true);
        });
        head->addWidget(sync);
    }
    lay->addLayout(head);
    auto* view = new View(p, w);
    lay->addWidget(view, 1);
    onDisplay.push_back(syncId);
    auto* knobs = new GenericParamPanel(
        app, owner, p, [onDisplay](const ParamDesc& d) { return std::find(onDisplay.begin(), onDisplay.end(), d.id) == onDisplay.end(); },
        6, w, GenericParamPanel::kCellW);
    auto* row = new QHBoxLayout();
    row->addStretch(1);
    row->addWidget(knobs);
    row->addStretch(1);
    lay->addLayout(row);
    auto* t = new QTimer(w);
    QObject::connect(t, &QTimer::timeout, w, [view, knobs, sync, p, syncId] {
        view->update();
        knobs->sync();
        if (sync && sync->currentIndex() != p->params().getInt(syncId)) {   // automation, undo, presets
            QSignalBlocker block(sync);
            sync->setCurrentIndex(p->params().getInt(syncId));
        }
    });
    t->start(33);
    return w;
}

NativeEditorRegistration s_delays({"native.delay", "native.pingpong", "native.echo"}, [](IProcessor* p, QWidget* parent) {
    return editor<TapView>(p, parent,
        {Delay::pTime, Delay::pFeedback, p->key() == "native.echo" ? (uint32_t)Echo::pMix : (uint32_t)Delay::pMix}, Delay::pSync);
});
NativeEditorRegistration s_reverb({"native.reverb"},
    [](IProcessor* p, QWidget* parent) { return editor<ReverbView>(p, parent, {Reverb::pPreDelay, Reverb::pDecay, Reverb::pDamping}); });
NativeEditorRegistration s_modulation({"native.chorus", "native.flanger", "native.phaser", "native.tremolo", "native.vibrato"},
    [](IProcessor* p, QWidget* parent) { return editor<LfoView>(p, parent, {1, 2}, lfoSyncParam(p->key())); });

}   // namespace
}   // namespace mt
