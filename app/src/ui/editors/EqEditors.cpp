// Visual editors for the EQs and filters (Parametric EQ, Shelving EQ, high-pass / low-pass / band-pass / notch): the response
// curve over the live spectrum (input faint, output bright) with a handle per band to drag, and the values below it.
#include "ui/PluginWindow.h"
#include "ui/Theme.h"
#include "dsp/effects/Effects.h"
#include "dsp/Fft.h"
#include <QComboBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QTimer>
#include <QWheelEvent>
#include <complex>

namespace mt {
namespace {

Id ownerOf(IProcessor* p) {
    App* app = App::instance();
    for (auto& [id, proc] : app->engine().registry().processors())
        if (proc.get() == p) return id;
    return 0;
}

QColor bandColor(int i) {   // -1: the theme's accent (a single-band filter)
    static const QColor c[] = {
        QColor("#e2665c"), QColor("#e0a040"), QColor("#6dc26d"), QColor("#5aa9e6"), QColor("#c07be0"), QColor("#5ad1c9")};
    if (i < 0) return theme::accent();
    return theme::isDark() ? c[i % 6] : c[i % 6].darker(135);
}

std::complex<double> biquadAt(const dsp::Biquad& q, double hz, double sr) {
    const std::complex<double> z = std::polar(1.0, -2 * M_PI * hz / sr), z2 = z * z;
    return (double(q.b0) + double(q.b1) * z + double(q.b2) * z2) / (1.0 + double(q.a1) * z + double(q.a2) * z2);
}
double dbOf(std::complex<double> h) { return 20 * std::log10(std::abs(h) + 1e-9); }

// One live spectrum line: the latest kFft samples of a tap in dB per pixel column, falling slowly like an analyser
struct Spectrum {
    static constexpr int kFft = 4096;   // ~11 Hz bins at 44.1 kHz: enough to tell bass notes apart
    static constexpr float kFloor = -120.f;
    std::vector<float> col;
    uint32_t seen = 0;
    template <class HzAt> void update(const AnalysisTap& tap, int width, HzAt hzAt) {
        static dsp::Fft fft(kFft);
        static const std::vector<float> window = [] {
            std::vector<float> w(kFft);
            for (int i = 0; i < kFft; ++i) w[i] = 0.5f * (1.f - std::cos(2 * (float)M_PI * i / (kFft - 1)));
            return w;
        }();
        if ((int)col.size() != width) col.assign(width, kFloor);
        std::vector<float> db(width, kFloor);
        const uint32_t end = tap.written.load(std::memory_order_acquire);
        if (end != seen) {   // no new audio (engine stopped, effect bypassed): the line falls away
            seen = end;
            std::vector<float> in(kFft);
            std::vector<std::complex<float>> out(kFft / 2 + 1);
            for (int i = 0; i < kFft; ++i)
                in[i] = tap.ring[(end - kFft + i) & (AnalysisTap::kSize - 1)].load(std::memory_order_relaxed) * window[i];
            fft.forwardReal(in.data(), out.data());
            const double binHz = tap.sampleRate.load(std::memory_order_relaxed) / kFft;
            auto mag = [&](int k) { return std::abs(out[std::clamp(k, 0, kFft / 2)]); };
            for (int x = 0; x < width; ++x) {
                const double k0 = hzAt(x - 0.5) / binHz, k1 = hzAt(x + 0.5) / binHz;
                float m = 0;
                if (k1 - k0 < 1) {   // several columns per bin (bass): interpolate between bins
                    const double k = (k0 + k1) / 2, f = k - std::floor(k);
                    m = (float)((1 - f) * mag((int)k) + f * mag((int)k + 1));
                } else
                    for (int k = (int)std::ceil(k0); k <= (int)k1; ++k) m = std::max(m, mag(k));
                // 4/N: a full-scale sine reads 0 dB through the Hann window; +3 dB/oct tilt so a mix reads about flat
                db[x] = 20.f * std::log10(m * 4.f / kFft + 1e-9f) + 3.f * (float)std::log2(hzAt(x) / 1000.0);
            }
        }
        for (int x = 0; x < width; ++x) col[x] = std::max(db[x], col[x] - 1.2f);
    }
};

// The response curve and the live spectrum under it, with a draggable handle per band
class ResponseView : public QWidget {
public:
    struct Band {
        QString name, tag;   // tag: the letter / number inside the handle
        int color;   // bandColor() index
        uint32_t freq, gain = 0, q = 0, on = 0, type = 0;   // parameter ids; 0 = this band has none
    };
    ResponseView(IProcessor* proc, Id owner, std::vector<Band> bands, double dbTop, double dbBottom, const AnalysisTap* in,
        const AnalysisTap* out, QWidget* parent)
        : QWidget(parent)
        , m_proc(proc)
        , m_owner(owner)
        , m_bands(std::move(bands))
        , m_top(dbTop)
        , m_bottom(dbBottom)
        , m_in(in)
        , m_out(out) {
        setObjectName("eqResponse");
        setMinimumSize(560, 240);
        setMouseTracking(true);
        theme::roundedFrame(this);
    }
    std::function<std::complex<double>(int band, double hz, double sr)> bandAt;   // one band's own filter
    std::function<double(double hz)> totalDb = [this](double hz) { return sumDb(hz); };   // what the whole effect does

    double sampleRate() const { return m_out->sampleRate.load(std::memory_order_relaxed); }
    bool enabled(int b) const { return !m_bands[b].on || ps().getBool(m_bands[b].on); }
    double bandDb(int b, double hz) const { return dbOf(bandAt(b, hz, sampleRate())); }
    double sumDb(double hz) const {
        double s = 0;
        for (int b = 0; b < (int)m_bands.size(); ++b)
            if (enabled(b)) s += bandDb(b, hz);
        return s;
    }
    QPointF handlePos(int b) const {
        const Band& d = m_bands[b];
        const double hz = ps().get(d.freq), db = usesGain(b) ? ps().get(d.gain) : std::max(0.0, bandDb(b, hz));
        return {hzToX(hz), std::min(std::max(dbToY(db), 8.0), height() - 8.0)};   // kept inside, also on a tiny view
    }
    void tick() {
        if (!isVisible()) return;
        auto hzAt = [this](double x) { return xToHz(x); };
        m_pre.update(*m_in, width(), hzAt);
        m_post.update(*m_out, width(), hzAt);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const double w = width(), h = height(), zeroY = dbToY(0);
        p.fillRect(rect(), theme::surface());
        p.setFont(theme::smallFont());
        for (double dec : {10., 100., 1000., 10000.})   // a line per 1..9 of each decade, the decades stronger
            for (int i = 1; i <= 9; ++i) {
                const double x = std::round(hzToX(dec * i)) + 0.5;
                if (x < 1 || x > w - 1) continue;
                p.setPen(i == 1 ? theme::gridStrong() : theme::grid());
                p.drawLine(QPointF(x, 0), QPointF(x, h));
            }
        const double step = m_top - m_bottom > 48 ? 12 : 6;
        for (double db = std::ceil(m_bottom / step) * step; db <= m_top; db += step) {
            const double y = std::round(dbToY(db)) + 0.5;
            p.setPen(db == 0 ? theme::gridStrong() : theme::grid());
            p.drawLine(QPointF(0, y), QPointF(w, y));
            if (y > 10 && y < h - 16) {
                p.setPen(theme::textDim());
                p.drawText(QPointF(5, y - 3), db > 0 ? QString("+%1").arg(db) : QString::number(db));
            }
        }
        p.setPen(theme::textDim());
        for (double hz : {50., 100., 200., 500., 1000., 2000., 5000., 10000.})
            p.drawText(QPointF(hzToX(hz) + 3, h - 5), hz >= 1000 ? QString("%1k").arg(hz / 1000) : QString::number(hz));

        auto spectrum = [&](const Spectrum& s, const QColor& fill, const QColor& line) {
            if ((int)s.col.size() != width()) return;
            QPolygonF poly;
            for (int x = 0; x < width(); ++x) poly << QPointF(x, std::clamp((6.0 - s.col[x]) / 102.0 * h, 0.0, h));
            QPainterPath area;
            area.addPolygon(poly);
            area.lineTo(w, h);
            area.lineTo(0, h);
            p.fillPath(area, fill);
            if (line.alpha()) {
                p.setPen(QPen(line, 1));
                p.drawPolyline(poly);
            }
        };
        // input as a faint shadow, output as a line over a light fill: told apart by shape, whatever the palette's colours
        spectrum(m_pre, theme::withAlpha(theme::textDim(), 32), Qt::transparent);
        spectrum(m_post, theme::withAlpha(theme::accent2(), 38), theme::withAlpha(theme::accent2(), 190));

        auto curve = [&](auto dbAt) {
            QPolygonF poly;
            for (double x = 0; x <= w + 1; x += 2) poly << QPointF(x, std::clamp(dbToY(dbAt(xToHz(x))), -2.0, h + 2));
            return poly;
        };
        auto fillToZero = [&](const QPolygonF& poly, const QColor& c) {
            QPainterPath area;
            area.addPolygon(poly);
            area.lineTo(w + 1, zeroY);
            area.lineTo(0, zeroY);
            p.fillPath(area, c);
        };
        if (m_bands.size() > 1)   // each band's own shape, so overlapping bands stay readable
            for (int b = 0; b < (int)m_bands.size(); ++b)
                if (enabled(b)) {
                    const QPolygonF poly = curve([&](double hz) { return bandDb(b, hz); });
                    fillToZero(poly, theme::withAlpha(bandColor(m_bands[b].color), 34));
                    p.setPen(QPen(theme::withAlpha(bandColor(m_bands[b].color), 120), 1));
                    p.drawPolyline(poly);
                }
        const QPolygonF total = curve(totalDb);
        fillToZero(total, theme::withAlpha(theme::accent(), 22));
        p.setPen(QPen(theme::accent(), 2));
        p.drawPolyline(total);

        const int hot = m_drag >= 0 ? m_drag : m_hover;
        for (int b = 0; b < (int)m_bands.size(); ++b) {
            const QPointF c = handlePos(b);
            const QColor col = bandColor(m_bands[b].color);
            const double r = b == hot ? 8 : 6.5;
            if (b == hot) {
                p.setPen(Qt::NoPen);
                p.setBrush(theme::withAlpha(col, 60));
                p.drawEllipse(c, r + 5, r + 5);
            }
            p.setPen(QPen(enabled(b) ? col.darker(140) : col, 1.5));
            p.setBrush(enabled(b) ? col : theme::surface());
            p.drawEllipse(c, r, r);
            p.setPen(enabled(b) ? QColor(0x10, 0x12, 0x16) : col);
            p.drawText(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r), Qt::AlignCenter, m_bands[b].tag);
        }
        if (hot >= 0) {   // the values of the handle under the mouse, next to it
            const Band& d = m_bands[hot];
            QString t = d.name + "   " + m_proc->formatValue(d.freq);
            if (usesGain(hot)) t += "   " + m_proc->formatValue(d.gain);
            if (d.q) t += "   Q " + m_proc->formatValue(d.q);
            if (!enabled(hot)) t += "   (off)";
            const QPointF c = handlePos(hot);
            QRectF box(0, 0, p.fontMetrics().horizontalAdvance(t) + 14, p.fontMetrics().height() + 6);
            box.moveCenter(c + QPointF(0, c.y() > 40 ? -26 : 26));
            box.moveLeft(std::clamp(box.left(), 4.0, w - box.width() - 4));
            p.setPen(theme::border());
            p.setBrush(theme::withAlpha(theme::panel(), 235));
            p.drawRoundedRect(box, theme::radiusFor(box.height()), theme::radiusFor(box.height()));
            p.setPen(theme::text());
            p.drawText(box, Qt::AlignCenter, t);
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        m_drag = e->button() == Qt::LeftButton ? handleAt(e->position()) : -1;
        if (m_drag < 0) return;
        m_grab = handlePos(m_drag) - e->position();   // the handle keeps its offset to the mouse: no jump on press
        m_pressY = e->position().y();
        m_startQ = m_bands[m_drag].q ? ps().getNormalized(m_bands[m_drag].q) : 0;
        m_moved = false;
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (m_drag < 0) {
            const int b = handleAt(e->position());
            if (b != m_hover) {
                m_hover = b;
                setCursor(b >= 0 ? Qt::SizeAllCursor : Qt::ArrowCursor);
                update();
            }
            return;
        }
        const Band& d = m_bands[m_drag];
        const QPointF at = e->position() + m_grab;
        if (!m_moved && d.on && !ps().getBool(d.on)) set(d.on, 1.f, true);   // dragging a band that is off switches it on
        m_moved = true;
        setPlain(d.freq, xToHz(at.x()));
        if (usesGain(m_drag)) setPlain(d.gain, yToDb(at.y()));
        else if (d.q) set(d.q, (float)(m_startQ + (m_pressY - e->position().y()) / (height() * 0.75)), false);   // up = sharper
        update();
    }
    void mouseReleaseEvent(QMouseEvent*) override {
        if (m_drag >= 0 && m_moved) {   // one undo step per value the drag changed
            const Band& d = m_bands[m_drag];
            set(d.freq, ps().getNormalized(d.freq), true);
            if (usesGain(m_drag)) set(d.gain, ps().getNormalized(d.gain), true);
            else if (d.q) set(d.q, ps().getNormalized(d.q), true);
        }
        m_drag = -1;
        update();
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        const int b = handleAt(e->position());
        if (b >= 0 && m_bands[b].on) set(m_bands[b].on, enabled(b) ? 0.f : 1.f, true);
    }
    void wheelEvent(QWheelEvent* e) override {
        const int b = handleAt(e->position());
        if (b < 0 || !m_bands[b].q || !e->angleDelta().y()) return e->ignore();
        setPlain(m_bands[b].q, ps().get(m_bands[b].q) * (e->angleDelta().y() > 0 ? 1.12 : 1 / 1.12), true);
        update();
    }
    void leaveEvent(QEvent*) override {
        m_hover = -1;
        update();
    }

private:
    ParameterSet& ps() const { return m_proc->params(); }
    void set(uint32_t id, float normalized, bool commit) { App::instance()->setProcessorParam(m_owner, id, normalized, commit); }
    void setPlain(uint32_t id, double v, bool commit = false) { set(id, ps().descById(id)->toNormalized((float)v), commit); }
    // shelves and peaks move up and down in gain; cut filters have none, so there the handle rides the curve and sets Q
    bool usesGain(int b) const {
        const Band& d = m_bands[b];
        return d.gain && (!d.type || ps().getInt(d.type) >= dsp::Biquad::Peak);
    }
    int handleAt(QPointF at) const {
        for (int b = (int)m_bands.size() - 1; b >= 0; --b)   // the one painted on top wins
            if (QLineF(handlePos(b), at).length() <= 10) return b;
        return -1;
    }
    double xToHz(double x) const { return 20 * std::pow(1000.0, x / width()); }
    double hzToX(double hz) const { return std::log(hz / 20) / std::log(1000.0) * width(); }
    double dbToY(double db) const { return (m_top - db) / (m_top - m_bottom) * height(); }
    double yToDb(double y) const { return m_top - y / height() * (m_top - m_bottom); }

    IProcessor* m_proc;
    Id m_owner;
    std::vector<Band> m_bands;
    double m_top, m_bottom;
    const AnalysisTap *m_in, *m_out;
    Spectrum m_pre, m_post;
    int m_hover = -1, m_drag = -1;
    QPointF m_grab;
    double m_pressY = 0, m_startQ = 0;
    bool m_moved = false;
};

// A parameter's value in a small box: drag up / down or scroll to change it, double-click to type one
class ValueBox : public QLabel {
public:
    ValueBox(IProcessor* proc, Id owner, uint32_t id, QString prefix, QWidget* parent)
        : QLabel(parent)
        , m_proc(proc)
        , m_owner(owner)
        , m_id(id)
        , m_prefix(std::move(prefix)) {
        setFixedHeight(20);
        setCursor(Qt::SizeVerCursor);
        setToolTip(QString::fromStdString(desc().name) + ": drag or scroll to change, double-click to type a value");
        makeValueEditable(this, [this](double v) {
            if (text().endsWith(" kHz")) v *= 1000;   // typed in the unit on display
            set(desc().toNormalized((float)v), true);
            sync();
        });
        sync();
    }
    void sync() {
        const QString t = m_prefix + m_proc->formatValue(m_id);
        if (t != text()) setText(t);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(isEnabled() && (underMouse() || m_dragging) ? theme::textDim() : theme::border());
        p.setBrush(theme::surface());
        p.drawRoundedRect(r, theme::radiusFor(r.height()), theme::radiusFor(r.height()));
        p.setPen(isEnabled() ? theme::text() : theme::withAlpha(theme::textDim(), 120));
        p.setFont(theme::smallFont());
        p.drawText(r, Qt::AlignCenter, text());
    }
    void mousePressEvent(QMouseEvent* e) override {
        m_dragging = e->button() == Qt::LeftButton;
        m_lastY = e->position().y();
        m_value = m_proc->params().getNormalized(m_id);
        m_moved = false;
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!m_dragging) return;
        const double perPx = e->modifiers() & Qt::ShiftModifier ? 0.0004 : 0.004;   // Shift: fine
        m_value = std::clamp(m_value + (m_lastY - e->position().y()) * perPx, 0.0, 1.0);
        m_lastY = e->position().y();
        m_moved = true;
        set((float)m_value, false);
        sync();
    }
    void mouseReleaseEvent(QMouseEvent*) override {
        if (m_dragging && m_moved) set((float)m_value, true);
        m_dragging = false;
        update();
    }
    void wheelEvent(QWheelEvent* e) override {
        if (!e->angleDelta().y()) return e->ignore();
        set(std::clamp(m_proc->params().getNormalized(m_id) + (e->angleDelta().y() > 0 ? 0.01f : -0.01f), 0.f, 1.f), true);
        sync();
    }
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }

private:
    const ParamDesc& desc() const { return *m_proc->params().descById(m_id); }
    void set(float normalized, bool commit) { App::instance()->setProcessorParam(m_owner, m_id, normalized, commit); }
    IProcessor* m_proc;
    Id m_owner;
    uint32_t m_id;
    QString m_prefix;
    bool m_dragging = false, m_moved = false;
    double m_lastY = 0, m_value = 0;
};

// A band's colour and name above its values; a click switches the band on or off when it can be
class BandChip : public QWidget {
public:
    BandChip(IProcessor* proc, Id owner, const ResponseView::Band& band, QWidget* parent)
        : QWidget(parent)
        , m_proc(proc)
        , m_owner(owner)
        , m_band(band) {
        setFixedHeight(20);
        if (band.on) {
            setCursor(Qt::PointingHandCursor);
            setToolTip(band.name + " on / off");
        }
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const bool on = !m_band.on || m_proc->params().getBool(m_band.on);
        const QColor col = bandColor(m_band.color);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(on ? theme::withAlpha(col, 150) : theme::border());
        p.setBrush(on ? theme::withAlpha(col, 40) : Qt::transparent);
        p.drawRoundedRect(r, theme::radiusFor(r.height()), theme::radiusFor(r.height()));
        p.setPen(QPen(col, 1.5));
        p.setBrush(on ? col : Qt::transparent);
        p.drawEllipse(QPointF(r.left() + 10, r.center().y()), 3.5, 3.5);
        p.setPen(on ? theme::text() : theme::textDim());
        p.setFont(theme::smallFont());
        p.drawText(r.adjusted(18, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, m_band.name);
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (!m_band.on || e->button() != Qt::LeftButton) return;
        App::instance()->setProcessorParam(m_owner, m_band.on, m_proc->params().getBool(m_band.on) ? 0.f : 1.f, true);
        update();
    }

private:
    IProcessor* m_proc;
    Id m_owner;
    ResponseView::Band m_band;
};

// One band's column under the display: chip, type (when it has a choice), then its values
QWidget* bandColumn(
    IProcessor* proc, Id owner, const ResponseView::Band& b, int width, std::vector<std::function<void()>>& syncs, QWidget* parent) {
    auto* col = new QWidget(parent);
    col->setFixedWidth(width);
    auto* v = new QVBoxLayout(col);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(3);
    auto* chip = new BandChip(proc, owner, b, col);
    v->addWidget(chip);
    syncs.push_back([chip] { chip->update(); });
    auto& ps = proc->params();
    if (b.type) {
        auto* combo = new QComboBox(col);
        combo->setFixedHeight(20);
        theme::styled(combo, "font-size:8pt;");
        for (auto& c : ps.descById(b.type)->choices) combo->addItem(QString::fromStdString(c));
        combo->setCurrentIndex(ps.getInt(b.type));
        QObject::connect(combo, &QComboBox::currentIndexChanged, col, [proc, owner, id = b.type](int i) {
            App::instance()->setProcessorParam(owner, id, proc->params().descById(id)->toNormalized((float)i), true);
        });
        v->addWidget(combo);
        syncs.push_back([combo, &ps, id = b.type] {
            if (combo->currentIndex() != ps.getInt(id)) {
                QSignalBlocker block(combo);
                combo->setCurrentIndex(ps.getInt(id));
            }
        });
    }
    for (auto [id, prefix] : {std::pair<uint32_t, QString>{b.freq, ""}, {b.gain, ""}, {b.q, "Q "}}) {
        if (!id) continue;
        auto* box = new ValueBox(proc, owner, id, prefix, col);
        v->addWidget(box);
        if (id == b.gain && b.type)   // a cut filter has no gain: its box greys out
            syncs.push_back([box, &ps, type = b.type] {
                box->setEnabled(ps.getInt(type) >= dsp::Biquad::Peak);
                box->sync();
            });
        else syncs.push_back([box] { box->sync(); });
    }
    v->addStretch(1);
    return col;
}

// The shared frame: a header strip (name, how to use the display) above the display; returns the layout for the controls
QVBoxLayout* frame(QWidget* w, IProcessor* proc, ResponseView* view, const QString& hint) {
    auto* lay = new QVBoxLayout(w);
    lay->setContentsMargins(8, 6, 8, 8);
    lay->setSpacing(6);
    auto* head = new QHBoxLayout();
    auto* name = theme::dimLabel(proc->displayName(), w, "font-size:8pt; font-weight:bold;");
    name->setFont(theme::smallFont());
    head->addWidget(name);
    head->addStretch(1);
    head->addWidget(theme::dimLabel(hint, w, "font-size:8pt;"));
    lay->addLayout(head);
    lay->addWidget(view, 1);
    return lay;
}
void startRefresh(QWidget* w, ResponseView* view, std::vector<std::function<void()>> syncs) {
    auto* t = new QTimer(w);
    QObject::connect(t, &QTimer::timeout, w, [w, view, syncs, rev = theme::revision()]() mutable {
        view->tick();
        for (auto& s : syncs) s();
        if (rev != theme::revision()) {   // painted controls pick up a new theme
            rev = theme::revision();
            w->update();
        }
    });
    t->start(33);
}

QWidget* makeParametricEq(IProcessor* p, QWidget* parent) {
    auto* eq = static_cast<ParametricEQ*>(p);
    const Id owner = ownerOf(p);
    using E = ParametricEQ;
    std::vector<ResponseView::Band> bands;
    for (int b = 0; b < E::kBands; ++b)
        bands.push_back(
            {QString("Band %1").arg(b + 1), QString::number(b + 1), b, E::bp(b, 2), E::bp(b, 3), E::bp(b, 4), E::bp(b, 0), E::bp(b, 1)});
    auto* w = new QWidget(parent);
    auto* view = new ResponseView(eq, owner, bands, 24, -24, &eq->tapIn, &eq->tapOut, w);
    view->setMinimumSize(680, 270);
    view->bandAt = [eq](int b, double hz, double sr) {
        auto& ps = eq->params();
        dsp::Biquad f;
        f.set((dsp::Biquad::Type)ps.getInt(E::bp(b, 1)), sr, ps.get(E::bp(b, 2)), ps.get(E::bp(b, 4)), ps.get(E::bp(b, 3)));
        return biquadAt(f, hz, sr);
    };
    view->totalDb = [view, eq](double hz) { return view->sumDb(hz) + eq->params().get(E::pOutput); };
    auto* lay = frame(w, eq, view, "drag a handle: freq / gain     wheel: Q     double-click: band on / off");
    std::vector<std::function<void()>> syncs;
    auto* strip = new QHBoxLayout();
    strip->setSpacing(8);
    for (auto& b : bands) strip->addWidget(bandColumn(eq, owner, b, 100, syncs, w));
    strip->addStretch(1);
    auto* out = new GenericParamPanel(*App::instance(), owner, eq, [](const ParamDesc& d) { return d.id == E::pOutput; }, 1, w);
    out->layout()->setContentsMargins(0, 0, 0, 0);
    strip->addWidget(out, 0, Qt::AlignTop);
    syncs.push_back([out] { out->sync(); });
    lay->addLayout(strip);
    startRefresh(w, view, syncs);
    return w;
}

QWidget* makeShelvingEq(IProcessor* p, QWidget* parent) {
    auto* eq = static_cast<ShelvingEQ*>(p);
    const Id owner = ownerOf(p);
    using E = ShelvingEQ;
    const std::vector<ResponseView::Band> bands = {{"Low shelf", "L", 0, E::pLowFreq, E::pLowGain},
        {"Mid", "M", 2, E::pMidFreq, E::pMidGain, E::pMidQ}, {"High shelf", "H", 3, E::pHighFreq, E::pHighGain}};
    auto* w = new QWidget(parent);
    auto* view = new ResponseView(eq, owner, bands, 18, -18, &eq->tapIn, &eq->tapOut, w);
    view->bandAt = [eq](int b, double hz, double sr) {   // as ShelvingEQ::process builds them
        auto& ps = eq->params();
        dsp::Biquad f;
        if (b == 0) f.set(dsp::Biquad::LowShelf, sr, ps.get(E::pLowFreq), 0.7f, ps.get(E::pLowGain));
        else if (b == 1) f.set(dsp::Biquad::Peak, sr, ps.get(E::pMidFreq), ps.get(E::pMidQ), ps.get(E::pMidGain));
        else f.set(dsp::Biquad::HighShelf, sr, ps.get(E::pHighFreq), 0.7f, ps.get(E::pHighGain));
        return biquadAt(f, hz, sr);
    };
    auto* lay = frame(w, eq, view, "drag a handle: freq / gain     wheel on M: Q");
    std::vector<std::function<void()>> syncs;
    auto* strip = new QHBoxLayout();
    strip->setSpacing(12);
    strip->addStretch(1);
    for (auto& b : bands) strip->addWidget(bandColumn(eq, owner, b, 120, syncs, w));
    strip->addStretch(1);
    lay->addLayout(strip);
    startRefresh(w, view, syncs);
    return w;
}

QWidget* makeFilter(IProcessor* p, QWidget* parent) {
    auto* f = static_cast<FilterEffect*>(p);
    const Id owner = ownerOf(p);
    using E = FilterEffect;
    auto* w = new QWidget(parent);
    auto* view = new ResponseView(f, owner, {{"Cutoff", "", -1, E::pCutoff, 0, E::pResonance}}, 24, -48, &f->tapIn, &f->tapOut, w);
    view->bandAt = [f](int, double hz, double sr) {   // the slope is that many biquads in a row
        auto& ps = f->params();
        dsp::Biquad q;
        q.set(f->type(), sr, ps.get(E::pCutoff), ps.get(E::pResonance));
        return std::pow(biquadAt(q, hz, sr), ps.getInt(E::pSlope) + 1);
    };
    view->totalDb = [view, f](double hz) {
        const double mix = f->params().get(E::pMix);
        return dbOf(1 - mix + mix * view->bandAt(0, hz, view->sampleRate()));
    };
    auto* lay = frame(w, f, view, "drag the handle: cutoff / resonance     wheel: resonance");
    auto* knobs = new GenericParamPanel(*App::instance(), owner, f, nullptr, 4, w);
    auto* row = new QHBoxLayout();
    row->addStretch(1);
    row->addWidget(knobs);
    row->addStretch(1);
    lay->addLayout(row);
    startRefresh(w, view, {[knobs] { knobs->sync(); }});
    return w;
}

NativeEditorRegistration s_parametric({"native.parametriceq"}, &makeParametricEq);
NativeEditorRegistration s_shelving({"native.shelvingeq"}, &makeShelvingEq);
NativeEditorRegistration s_filters({"native.highpass", "native.lowpass", "native.bandpass", "native.notch"}, &makeFilter);

}   // namespace

// Self-test: drags band 2's handle on a Parametric EQ editor up and to the right, as a mouse would. True when the band
// followed (higher frequency, positive gain) and switched itself on. 'eq' must be in the engine (App edits it by owner id).
bool eqHandleDragSelfTest(IProcessor* eq) {
    std::unique_ptr<QWidget> ed(eq->createEditor(nullptr));
    auto* view = ed ? static_cast<ResponseView*>(ed->findChild<QWidget*>("eqResponse")) : nullptr;
    if (!view) return false;
    ed->resize(ed->sizeHint());
    ed->layout()->activate();
    auto& ps = eq->params();
    const uint32_t on = ParametricEQ::bp(1, 0), freq = ParametricEQ::bp(1, 2), gain = ParametricEQ::bp(1, 3);
    const float f0 = ps.get(freq);
    const QPointF from = view->handlePos(1), to = from + QPointF(80, -50);
    auto send = [view](QEvent::Type t, QPointF at, Qt::MouseButton button, Qt::MouseButtons held) {
        QMouseEvent e(t, at, view->mapToGlobal(at), button, held, Qt::NoModifier);
        QCoreApplication::sendEvent(view, &e);
    };
    send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    send(QEvent::MouseMove, (from + to) / 2, Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    return ps.getBool(on) && ps.get(freq) > f0 * 1.5f && ps.get(gain) > 3.f;
}

}   // namespace mt
