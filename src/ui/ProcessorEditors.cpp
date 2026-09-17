// Custom editors for native processors (installed into the DSP layer through nativeEditorFactory()).
#include "ui/PluginWindow.h"
#include "ui/Theme.h"
#include "dsp/instruments/Instruments.h"
#include "dsp/effects/Effects.h"
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QPushButton>
#include <QFileDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QTimer>
#include <QLabel>
#include <QMenu>

namespace mt {

static Id ownerOf(IProcessor* p) { App* app = App::instance(); for (auto& [id, proc] : app->engine().registry().processors()) if (proc.get() == p) return id; return 0; }

// ---------------------------------------------------------------- waveform view
class WaveView : public QWidget {
public:
    explicit WaveView(QWidget* parent = nullptr) : QWidget(parent) { setMinimumHeight(120); setAcceptDrops(true); setMouseTracking(true); }
    std::function<AudioAssetPtr()> asset; std::function<void(const QString&)> onDrop; std::function<float()> startFrac; std::function<void(float)> setStart;
    // Optional processing view: the drawn waveform reflects the instrument's current settings
    std::function<float(double)> envelope;            // gain multiplier at fraction 0..1 of the (already reversed) sample
    std::function<bool()> reversed;                   // play backwards
    std::function<std::pair<float, float>()> loop;    // loop region (fractions), enabled when first < second
    std::function<QString()> caption;                 // extra info line
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.fillRect(rect(), theme::surface()); auto a = asset ? asset() : nullptr;
        if (!a || a->frames == 0) { p.setPen(theme::textDim()); p.drawText(rect(), Qt::AlignCenter, "Drop an audio file here or use Load"); return; }
        bool rev = reversed && reversed(); float st = startFrac ? startFrac() : 0.f; std::pair<float, float> lp = loop ? loop() : std::make_pair(0.f, 0.f); bool hasLoop = lp.first < lp.second;
        QRect wave = rect().adjusted(0, 16, 0, 0); double spp = (double)a->frames / wave.width(); const auto& lv = a->levelFor(spp); int mid = wave.center().y(); int hh = wave.height() / 2 - 2;
        // background regions: before the start point (skipped) and the loop region
        if (st > 0) p.fillRect(QRect(wave.left(), wave.top(), (int)(st * wave.width()), wave.height()), QColor(0x1a, 0x1c, 0x21));
        if (hasLoop) p.fillRect(QRect(wave.left() + (int)(lp.first * wave.width()), wave.top(), (int)((lp.second - lp.first) * wave.width()), wave.height()), QColor(0x22, 0x2c, 0x38));
        p.setPen(theme::grid()); p.drawLine(wave.left(), mid, wave.right(), mid);
        for (int x = 0; x < wave.width(); ++x) {
            double frac = (double)x / wave.width(); double srcFrac = rev ? 1.0 - frac : frac; int64_t b0 = (int64_t)(srcFrac * a->frames / lv.samplesPerBin), b1 = std::max<int64_t>(b0 + 1, (int64_t)((srcFrac + 1.0 / wave.width()) * a->frames / lv.samplesPerBin));
            if (rev) std::swap(b0, b1), b0 = std::max<int64_t>(0, b1 - std::max<int64_t>(1, (int64_t)(spp / lv.samplesPerBin)));
            float mx = -1, mn = 1; for (int64_t b = b0; b < b1 && b < (int64_t)lv.maxL.size(); ++b) { if (b < 0) continue; mx = std::max(mx, std::max(lv.maxL[b], lv.maxR[b])); mn = std::min(mn, std::min(lv.minL[b], lv.minR[b])); }
            if (mx < mn) continue; float g = envelope ? envelope(frac) : 1.f; bool active = frac >= st; QColor c = active ? QColor("#5ad1c9") : QColor(0x3a, 0x45, 0x50); if (g > 1.f && active) c = c.lighter(110 + (int)std::min(60.f, (g - 1) * 40));
            p.setPen(c); p.drawLine(x, mid - (int)(clamp(mx * g, -1.f, 1.f) * hh), x, mid - (int)(clamp(mn * g, -1.f, 1.f) * hh));
        }
        if (envelope) { QPolygonF env; for (int x = 0; x < wave.width(); x += 2) env << QPointF(x, mid - clamp(envelope((double)x / wave.width()), 0.f, 1.f) * hh); p.setPen(QPen(theme::withAlpha(theme::accent(), 150), 1)); p.drawPolyline(env); }
        int sx = (int)(st * wave.width()); p.setPen(QPen(theme::accent(), 2)); p.drawLine(sx, wave.top(), sx, wave.bottom());
        if (hasLoop) { p.setPen(QPen(theme::accent2(), 1, Qt::DashLine)); int lx1 = (int)(lp.first * wave.width()), lx2 = (int)(lp.second * wave.width()); p.drawLine(lx1, wave.top(), lx1, wave.bottom()); p.drawLine(lx2, wave.top(), lx2, wave.bottom()); }
        p.setPen(theme::textDim()); p.setFont(theme::smallFont()); QString info = QString("%1  |  %2 s  |  %3 Hz  |  %4%5").arg(QFileInfo(a->path).fileName()).arg(a->seconds(), 0, 'f', 2).arg((int)a->originalSampleRate).arg(a->channels == 1 ? "mono" : "stereo").arg(rev ? "  |  reversed" : ""); if (caption) info += "  |  " + caption(); p.drawText(4, 12, info);
    }
    void mousePressEvent(QMouseEvent* e) override { if (setStart && e->button() == Qt::LeftButton) { setStart(clamp((float)e->pos().x() / width(), 0.f, 1.f)); update(); } }
    void mouseMoveEvent(QMouseEvent* e) override { if (setStart && (e->buttons() & Qt::LeftButton)) { setStart(clamp((float)e->pos().x() / width(), 0.f, 1.f)); update(); } }
    void dragEnterEvent(QDragEnterEvent* e) override { if (e->mimeData()->hasUrls()) e->acceptProposedAction(); }
    void dropEvent(QDropEvent* e) override { for (auto& u : e->mimeData()->urls()) { QString p = u.toLocalFile(); if (AudioFile::isAudioFile(p) && onDrop) { onDrop(p); break; } } }
};

// ADSR shape over the sample's duration for the waveform preview (note held for the whole sample)
static float previewEnvelope(double frac, double seconds, float a, float d, float sus, float level) {
    double t = frac * seconds; float e; if (t < a) e = (float)(t / std::max(a, 0.0005f)); else { double td = t - a; e = sus + (1.f - sus) * (float)std::exp(-4.0 * td / std::max(d, 0.0005f)); } return e * level;
}

// ---------------------------------------------------------------- sampler editor
static QWidget* makeSamplerEditor(Sampler* s, QWidget* parent) {
    App& app = *App::instance(); Id owner = ownerOf(s); auto* w = new QWidget(parent); auto* lay = new QVBoxLayout(w); lay->setContentsMargins(4, 4, 4, 4);
    auto* top = new QHBoxLayout(); auto* play = new QPushButton("\u25B6 Play", w); play->setToolTip("Play the sample at its root note through the channel's mixer track (click again to stop)"); play->setFocusPolicy(Qt::NoFocus); top->addWidget(play); auto* load = new QPushButton("Load sample...", w); top->addWidget(load); auto* name = new QLabel(s->samplePath().isEmpty() ? "(no sample)" : QFileInfo(s->samplePath()).fileName(), w); top->addWidget(name, 1); lay->addLayout(top);
    { // FL-style one-shot audition: one click plays the whole sample, a second click stops it
        auto* stopTimer = new QTimer(w); stopTimer->setSingleShot(true); auto* playing = new bool(false); QObject::connect(w, &QObject::destroyed, [playing] { delete playing; });
        auto stop = [&app, s, play, playing, stopTimer] { if (*playing) { app.noteOff(s->params().getInt(Sampler::pRoot)); *playing = false; play->setText("\u25B6 Play"); stopTimer->stop(); } };
        QObject::connect(stopTimer, &QTimer::timeout, w, stop);
        QObject::connect(play, &QPushButton::clicked, w, [&app, owner, s, play, playing, stopTimer, stop] { if (*playing) { stop(); return; } app.selectChannel(owner); int note = s->params().getInt(Sampler::pRoot); app.noteOn(note, 1.f); *playing = true; play->setText("\u25A0 Stop"); auto a = s->sample(); double secs = a ? a->seconds() / std::pow(2.0, (s->params().get(Sampler::pTune) + s->params().get(Sampler::pFine) / 100.0) / 12.0) : 1.0; bool loops = s->params().getInt(Sampler::pLoopMode) != 0; if (!loops) stopTimer->start((int)(secs * 1000) + 100); });
    }
    auto* wave = new WaveView(w); wave->asset = [s] { return s->sample(); }; wave->startFrac = [s] { return s->params().get(Sampler::pStart); }; wave->setStart = [&app, owner](float f) { app.setProcessorParam(owner, Sampler::pStart, f, true); };
    wave->reversed = [s] { return s->params().getBool(Sampler::pReverse); };
    wave->loop = [s] { auto& ps = s->params(); if (ps.getInt(Sampler::pLoopMode) == 0) return std::make_pair(0.f, 0.f); return std::make_pair(ps.get(Sampler::pLoopStart), ps.get(Sampler::pLoopEnd)); };
    wave->envelope = [s](double frac) { auto& ps = s->params(); auto a = s->sample(); double secs = a ? a->seconds() : 1.0; return previewEnvelope(frac, secs, ps.get(Sampler::pAttack), ps.get(Sampler::pDecay), ps.get(Sampler::pSustain), ps.get(Sampler::pLevel)); };
    wave->caption = [s] { auto& ps = s->params(); float semis = ps.get(Sampler::pTune) + ps.get(Sampler::pFine) / 100.f; QString c = QString("level %1 dB").arg(gainToDb(ps.get(Sampler::pLevel)), 0, 'f', 1); if (std::fabs(semis) > 0.01f) c += QString("  |  tune %1 st").arg(semis, 0, 'f', 2); if (ps.get(Sampler::pCutoff) < 19000.f) c += QString("  |  %1 %2 Hz").arg(ps.getInt(Sampler::pFilterType) == 0 ? "LP" : ps.getInt(Sampler::pFilterType) == 1 ? "HP" : "BP").arg((int)ps.get(Sampler::pCutoff)); return c; };
    wave->onDrop = [&app, owner, name](const QString& p) { app.setChannelSample(owner, p); name->setText(QFileInfo(p).fileName()); };
    QObject::connect(load, &QPushButton::clicked, w, [&app, owner, name, w] { QString p = QFileDialog::getOpenFileName(w, "Load sample", QString(), "Audio (*.wav *.aif *.aiff *.flac *.ogg *.mp3)"); if (!p.isEmpty()) { app.setChannelSample(owner, p); name->setText(QFileInfo(p).fileName()); } });
    lay->addWidget(wave); auto* params = new GenericParamPanel(app, owner, s, nullptr, 7, w); lay->addWidget(params);
    auto* t = new QTimer(w); QObject::connect(t, &QTimer::timeout, w, [wave, params] { wave->update(); params->sync(); }); t->start(100);
    return w;
}

// ---------------------------------------------------------------- drum sampler editor
class PadGrid : public QWidget {
public:
    PadGrid(App& app, Id owner, DrumSampler* ds, QWidget* parent) : QWidget(parent), m_app(app), m_owner(owner), m_ds(ds) { setMinimumSize(4 * 70, 4 * 56); setAcceptDrops(true); }
    int selected = 0; std::function<void(int)> onSelect;
    QRect padRect(int i) const { int c = i % 4, r = 3 - i / 4; int pw = width() / 4, ph = height() / 4; return QRect(c * pw + 2, r * ph + 2, pw - 4, ph - 4); }
    int padAt(const QPoint& p) const { for (int i = 0; i < 16; ++i) if (padRect(i).contains(p)) return i; return -1; }
    void paintEvent(QPaintEvent*) override { QPainter p(this); p.setRenderHint(QPainter::Antialiasing); p.setFont(theme::smallFont()); for (int i = 0; i < 16; ++i) { QRect r = padRect(i); bool has = m_ds->padSample(i) != nullptr; p.setPen(QPen(i == selected ? theme::accent() : theme::border(), i == selected ? 2 : 1)); p.setBrush(has ? QColor(0x3a, 0x4c, 0x60) : theme::panelLight()); p.drawRoundedRect(r, 4, 4); p.setPen(theme::text()); QString path = m_ds->padPath(i); QString nm = path.startsWith("builtin:") ? path.mid(8) : QFileInfo(path).completeBaseName(); p.drawText(r.adjusted(4, 2, -4, -2), Qt::AlignTop | Qt::AlignLeft, QString("%1 %2").arg(i + 1).arg(theme::noteName(36 + i))); p.drawText(r.adjusted(4, 2, -4, -2), Qt::AlignBottom | Qt::AlignLeft, p.fontMetrics().elidedText(nm, Qt::ElideRight, r.width() - 8)); } }
    void mousePressEvent(QMouseEvent* e) override { int i = padAt(e->pos()); if (i < 0) return; selected = i; if (onSelect) onSelect(i); update(); if (e->button() == Qt::LeftButton) { m_app.selectChannel(m_owner); m_app.noteOn(36 + i); } else { QMenu m; m.addAction("Load sample...", [this, i] { QString p = QFileDialog::getOpenFileName(this, "Load sample", QString(), "Audio (*.wav *.aif *.aiff *.flac *.ogg *.mp3)"); if (!p.isEmpty()) assign(i, p); }); auto* bi = m.addMenu("Built-in sound"); for (const char* n : {"Kick", "Snare", "Clap", "Hat"}) bi->addAction(n, [this, i, n] { m_ds->setPadSample(i, DrumSampler::synthesizeDrum(n, m_app.engine().sampleRate()), QString("builtin:") + n); m_app.model().setDirty(true); update(); }); m.addAction("Clear", [this, i] { m_ds->setPadSample(i, nullptr, QString()); update(); }); m.exec(e->globalPosition().toPoint()); } }
    void mouseReleaseEvent(QMouseEvent* e) override { int i = padAt(e->pos()); if (i >= 0) m_app.noteOff(36 + i); }
    void assign(int i, const QString& p) { auto a = m_app.cache().getSync(p); if (a) { m_ds->setPadSample(i, a, p); m_app.model().setDirty(true); update(); } }
    void dragEnterEvent(QDragEnterEvent* e) override { if (e->mimeData()->hasUrls()) e->acceptProposedAction(); }
    void dropEvent(QDropEvent* e) override { int i = padAt(e->position().toPoint()); if (i < 0) return; for (auto& u : e->mimeData()->urls()) { QString p = u.toLocalFile(); if (AudioFile::isAudioFile(p)) { assign(i, p); break; } } }
private: App& m_app; Id m_owner; DrumSampler* m_ds;
};
static QWidget* makeDrumEditor(DrumSampler* ds, QWidget* parent) {
    App& app = *App::instance(); Id owner = ownerOf(ds); auto* w = new QWidget(parent); auto* lay = new QHBoxLayout(w); lay->setContentsMargins(4, 4, 4, 4);
    auto* left = new QVBoxLayout(); auto* hint = new QLabel("Pads: click to audition (C1 = pad 1), right-click / drop files to assign. Steps trigger pad 1 at the root note.", w); hint->setWordWrap(true); theme::styled(hint, "color:@textDim@;"); left->addWidget(hint);
    auto* grid = new PadGrid(app, owner, ds, w); left->addWidget(grid, 1);
    auto* padWave = new WaveView(w); padWave->setMinimumHeight(90); left->addWidget(padWave);
    auto* playRow = new QHBoxLayout(); auto* play = new QPushButton("\u25B6 Play pad", w); play->setToolTip("Play the selected pad (pads are one-shot)"); play->setFocusPolicy(Qt::NoFocus); playRow->addWidget(play); playRow->addStretch(1); left->addLayout(playRow); lay->addLayout(left, 1);
    QObject::connect(play, &QPushButton::clicked, w, [&app, owner, grid] { app.selectChannel(owner); app.noteOn(36 + grid->selected, 1.f); QTimer::singleShot(50, [&app, n = 36 + grid->selected] { app.noteOff(n); }); });
    padWave->asset = [ds, grid] { return ds->padSample(grid->selected); };
    padWave->onDrop = [grid](const QString& p) { grid->assign(grid->selected, p); };
    padWave->startFrac = [ds, grid] { return ds->params().get(DrumSampler::padParam(grid->selected, 6)); }; padWave->setStart = [&app, owner, grid](float f) { app.setProcessorParam(owner, DrumSampler::padParam(grid->selected, 6), f, true); };
    padWave->reversed = [ds, grid] { return ds->params().getBool(DrumSampler::padParam(grid->selected, 5)); };
    padWave->envelope = [ds, grid](double frac) { auto& ps = ds->params(); int pad = grid->selected; auto a = ds->padSample(pad); double secs = a ? a->seconds() : 1.0; float decay = std::min(ps.get(DrumSampler::pDecay), ps.get(DrumSampler::padParam(pad, 3))); return previewEnvelope(frac, secs, ps.get(DrumSampler::pAttack), decay, 0.f, ps.get(DrumSampler::pLevel) * ps.get(DrumSampler::padParam(pad, 0))); };
    padWave->caption = [ds, grid] { auto& ps = ds->params(); int pad = grid->selected; float semis = ps.get(DrumSampler::pPitch) + ps.get(DrumSampler::padParam(pad, 2)) + ps.get(DrumSampler::padParam(pad, 7)) / 100.f; QString c = QString("pad %1").arg(pad + 1); if (std::fabs(semis) > 0.01f) c += QString("  |  pitch %1 st").arg(semis, 0, 'f', 2); return c; };
    auto* right = new QVBoxLayout(); auto* padLabel = new QLabel("Pad 1", w); padLabel->setStyleSheet("font-weight:bold; color:#f2a93b;"); right->addWidget(padLabel);
    auto* padHost = new QWidget(w); auto* padLay = new QVBoxLayout(padHost); padLay->setContentsMargins(0, 0, 0, 0); right->addWidget(padHost);
    GenericParamPanel** padPanel = new GenericParamPanel*(nullptr);
    auto rebuildPad = [&app, owner, ds, padHost, padLay, padPanel, padLabel](int pad) { if (*padPanel) delete *padPanel; padLabel->setText(QString("Pad %1 (%2)").arg(pad + 1).arg(theme::noteName(36 + pad))); *padPanel = new GenericParamPanel(app, owner, ds, [pad](const ParamDesc& d) { return d.id >= DrumSampler::padParam(pad, 0) && d.id < DrumSampler::padParam(pad, 8); }, 4, padHost); padLay->addWidget(*padPanel); };
    rebuildPad(0); grid->onSelect = rebuildPad;
    auto* global = new QLabel("Global", w); global->setStyleSheet("font-weight:bold; color:#f2a93b;"); right->addWidget(global);
    auto* gp = new GenericParamPanel(app, owner, ds, [](const ParamDesc& d) { return d.id < DrumSampler::pPadBase; }, 5, w); right->addWidget(gp); right->addStretch(1); lay->addLayout(right, 1);
    auto* t = new QTimer(w); QObject::connect(t, &QTimer::timeout, w, [gp, padPanel, padWave] { gp->sync(); if (*padPanel) (*padPanel)->sync(); padWave->update(); }); t->start(100);
    QObject::connect(w, &QObject::destroyed, [padPanel] { delete padPanel; });
    return w;
}

// ---------------------------------------------------------------- parametric EQ editor
class EqCurve : public QWidget {
public:
    EqCurve(App& app, Id owner, ParametricEQ* eq, QWidget* parent) : QWidget(parent), m_app(app), m_owner(owner), m_eq(eq) { setMinimumHeight(220); setMouseTracking(true); }
    static double xToHz(int x, int w) { return 20.0 * std::pow(1000.0, (double)x / w); } static int hzToX(double hz, int w) { return (int)(std::log(hz / 20.0) / std::log(1000.0) * w); }
    int yToDb(int y) const { return (int)std::lround((height() / 2.0 - y) / (height() / 2.0) * 24); } int dbToY(double db) const { return (int)(height() / 2.0 - db / 24.0 * height() / 2.0); }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing); p.fillRect(rect(), theme::surface()); p.setPen(theme::grid()); for (double hz : {50., 100., 200., 500., 1000., 2000., 5000., 10000.}) { int x = hzToX(hz, width()); p.drawLine(x, 0, x, height()); p.setPen(theme::textDim()); p.setFont(theme::smallFont()); p.drawText(x + 2, height() - 4, hz >= 1000 ? QString::number(hz / 1000) + "k" : QString::number(hz)); p.setPen(theme::grid()); }
        for (int db = -18; db <= 18; db += 6) { int y = dbToY(db); p.setPen(db == 0 ? theme::gridStrong() : theme::grid()); p.drawLine(0, y, width(), y); }
        QPolygonF poly; for (int x = 0; x < width(); ++x) poly << QPointF(x, dbToY(clamp(m_eq->magnitudeDb((float)xToHz(x, width())), -24.f, 24.f))); p.setPen(QPen(theme::accent(), 2)); p.drawPolyline(poly);
        static const QColor cols[] = {QColor("#e2665c"), QColor("#e0a040"), QColor("#6dc26d"), QColor("#5aa9e6"), QColor("#c07be0"), QColor("#5ad1c9")};
        for (int b = 0; b < ParametricEQ::kBands; ++b) { auto& ps = m_eq->params(); bool on = ps.getBool(ParametricEQ::bp(b, 0)); int x = hzToX(ps.get(ParametricEQ::bp(b, 2)), width()); int y = dbToY(ps.get(ParametricEQ::bp(b, 3))); p.setPen(QPen(cols[b], on ? 2 : 1)); p.setBrush(on ? cols[b] : theme::panel()); p.drawEllipse(QPoint(x, y), 6, 6); p.setPen(theme::bg()); p.drawText(QRect(x - 6, y - 6, 12, 12), Qt::AlignCenter, QString::number(b + 1)); }
        p.setPen(theme::textDim()); p.drawText(4, 12, "Drag band handles (freq/gain), wheel = Q, double-click toggles band");
    }
    int bandAt(const QPoint& pt) const { for (int b = ParametricEQ::kBands - 1; b >= 0; --b) { auto& ps = m_eq->params(); QPoint c(hzToX(ps.get(ParametricEQ::bp(b, 2)), width()), dbToY(ps.get(ParametricEQ::bp(b, 3)))); if ((c - pt).manhattanLength() <= 10) return b; } return -1; }
    void mousePressEvent(QMouseEvent* e) override { m_band = bandAt(e->pos()); }
    void mouseMoveEvent(QMouseEvent* e) override { if (m_band < 0 || !(e->buttons() & Qt::LeftButton)) return; auto& ps = m_eq->params(); double hz = clamp(xToHz(e->pos().x(), width()), 20.0, 20000.0); double db = clamp((height() / 2.0 - e->pos().y()) / (height() / 2.0) * 24, -24.0, 24.0); const ParamDesc* fd = ps.descById(ParametricEQ::bp(m_band, 2)); const ParamDesc* gd = ps.descById(ParametricEQ::bp(m_band, 3)); m_app.setProcessorParam(m_owner, ParametricEQ::bp(m_band, 2), fd->toNormalized((float)hz), false); m_app.setProcessorParam(m_owner, ParametricEQ::bp(m_band, 3), gd->toNormalized((float)db), false); if (!ps.getBool(ParametricEQ::bp(m_band, 0))) m_app.setProcessorParam(m_owner, ParametricEQ::bp(m_band, 0), 1.f, true); update(); }
    void mouseReleaseEvent(QMouseEvent*) override { if (m_band >= 0) { auto& ps = m_eq->params(); m_app.setProcessorParam(m_owner, ParametricEQ::bp(m_band, 2), ps.getNormalized(ParametricEQ::bp(m_band, 2)), true); m_app.setProcessorParam(m_owner, ParametricEQ::bp(m_band, 3), ps.getNormalized(ParametricEQ::bp(m_band, 3)), true); } m_band = -1; }
    void mouseDoubleClickEvent(QMouseEvent* e) override { int b = bandAt(e->pos()); if (b >= 0) m_app.setProcessorParam(m_owner, ParametricEQ::bp(b, 0), m_eq->params().getBool(ParametricEQ::bp(b, 0)) ? 0.f : 1.f, true); }
    void wheelEvent(QWheelEvent* e) override { int b = bandAt(e->position().toPoint()); if (b < 0) return; auto& ps = m_eq->params(); float q = ps.get(ParametricEQ::bp(b, 4)) * (e->angleDelta().y() > 0 ? 1.15f : 1 / 1.15f); m_app.setProcessorParam(m_owner, ParametricEQ::bp(b, 4), ps.descById(ParametricEQ::bp(b, 4))->toNormalized(q), true); update(); }
private: App& m_app; Id m_owner; ParametricEQ* m_eq; int m_band = -1;
};
static QWidget* makeEqEditor(ParametricEQ* eq, QWidget* parent) { App& app = *App::instance(); Id owner = ownerOf(eq); auto* w = new QWidget(parent); auto* lay = new QVBoxLayout(w); lay->setContentsMargins(4, 4, 4, 4); auto* curve = new EqCurve(app, owner, eq, w); lay->addWidget(curve); auto* gp = new GenericParamPanel(app, owner, eq, nullptr, 5, w); auto* sc = new QScrollArea(w); sc->setWidgetResizable(true); sc->setWidget(gp); sc->setMinimumHeight(220); lay->addWidget(sc); auto* t = new QTimer(w); QObject::connect(t, &QTimer::timeout, w, [curve, gp] { curve->update(); gp->sync(); }); t->start(60); return w; }

// ---------------------------------------------------------------- spectrum & meter editors
class SpectrumView : public QWidget {
public:
    SpectrumView(SpectrumAnalyzer* sa, QWidget* parent) : QWidget(parent), m_sa(sa) { setMinimumSize(400, 200); auto* t = new QTimer(this); connect(t, &QTimer::timeout, this, [this] { update(); }); t->start(33); }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.fillRect(rect(), theme::surface()); m_sa->readSpectrum(m_mag); if (m_mag.empty()) return; double sr = App::instance()->engine().sampleRate(); float range = m_sa->params().get(SpectrumAnalyzer::pRange);
        p.setPen(theme::grid()); for (double hz : {50., 100., 200., 500., 1000., 2000., 5000., 10000.}) { int x = (int)(std::log(hz / 20.0) / std::log(1000.0) * width()); p.drawLine(x, 0, x, height()); p.setPen(theme::textDim()); p.setFont(theme::smallFont()); p.drawText(x + 2, height() - 4, hz >= 1000 ? QString::number(hz / 1000) + "k" : QString::number(hz)); p.setPen(theme::grid()); }
        QPolygonF poly; poly << QPointF(0, height()); int bins = (int)m_mag.size();
        for (int x = 0; x < width(); ++x) { double hz = 20.0 * std::pow(1000.0, (double)x / width()); int bin = clamp((int)(hz / (sr / 2) * bins), 0, bins - 1); float db = m_mag[bin]; poly << QPointF(x, height() - clamp((db + range) / range, 0.f, 1.f) * height()); }
        poly << QPointF(width(), height()); p.setPen(QPen(theme::accent2(), 1.5)); p.setBrush(theme::withAlpha(theme::accent2(), 60)); p.drawPolygon(poly);
    }
private: SpectrumAnalyzer* m_sa; std::vector<float> m_mag;
};
class MeterView : public QWidget {
public:
    MeterView(MeterEffect* m, QWidget* parent) : QWidget(parent), m_m(m) { setMinimumSize(220, 200); auto* t = new QTimer(this); connect(t, &QTimer::timeout, this, [this] { update(); }); t->start(33); }
    void paintEvent(QPaintEvent*) override { QPainter p(this); p.fillRect(rect(), theme::surface()); auto lv = m_m->levels(); auto bar = [&](int x, float pk, float rms, float hold, const QString& name) { QRect r(x, 20, 40, height() - 50); p.fillRect(r, theme::bg()); auto pos = [&](float v) { return clamp((gainToDb(v) + 60.f) / 66.f, 0.f, 1.f); }; QLinearGradient g(0, r.bottom(), 0, r.top()); g.setColorAt(0, theme::green()); g.setColorAt(0.8, theme::accent()); g.setColorAt(1, theme::red()); p.fillRect(QRect(r.left(), r.bottom() - (int)(r.height() * pos(pk)), r.width(), (int)(r.height() * pos(pk))), g); p.fillRect(QRect(r.left() + 12, r.bottom() - (int)(r.height() * pos(rms)), 16, (int)(r.height() * pos(rms))), theme::withAlpha(Qt::white, 90)); int hy = r.bottom() - (int)(r.height() * pos(hold)); p.setPen(Qt::white); p.drawLine(r.left(), hy, r.right(), hy); p.setPen(theme::text()); p.setFont(theme::smallFont()); p.drawText(QRect(x - 10, height() - 28, 60, 12), Qt::AlignCenter, name); p.drawText(QRect(x - 10, height() - 16, 60, 12), Qt::AlignCenter, QString("%1 / %2").arg(gainToDb(pk), 0, 'f', 1).arg(gainToDb(rms), 0, 'f', 1)); }; bar(40, lv.peakL, lv.rmsL, lv.holdL, "L"); bar(120, lv.peakR, lv.rmsR, lv.holdR, "R"); p.setPen(theme::textDim()); p.drawText(4, 12, "Peak (bar) / RMS (inner) in dBFS, hold line"); }
private: MeterEffect* m_m;
};

static QWidget* factory(IProcessor* p, QWidget* parent) {
    if (auto* s = dynamic_cast<Sampler*>(p)) return makeSamplerEditor(s, parent);
    if (auto* d = dynamic_cast<DrumSampler*>(p)) return makeDrumEditor(d, parent);
    if (auto* e = dynamic_cast<ParametricEQ*>(p)) return makeEqEditor(e, parent);
    if (auto* s = dynamic_cast<SpectrumAnalyzer*>(p)) { auto* w = new QWidget(parent); auto* l = new QVBoxLayout(w); l->addWidget(new SpectrumView(s, w)); l->addWidget(new GenericParamPanel(*App::instance(), ownerOf(p), p, nullptr, 4, w)); return w; }
    if (auto* m = dynamic_cast<MeterEffect*>(p)) { auto* w = new QWidget(parent); auto* l = new QVBoxLayout(w); l->addWidget(new MeterView(m, w)); l->addWidget(new GenericParamPanel(*App::instance(), ownerOf(p), p, nullptr, 4, w)); return w; }
    return nullptr;
}
struct EditorFactoryInstaller { EditorFactoryInstaller() { nativeEditorFactory() = &factory; } } s_installer;

QWidget* Sampler::createEditor(QWidget* parent) { return nativeEditorFactory() ? nativeEditorFactory()(this, parent) : nullptr; }
QWidget* DrumSampler::createEditor(QWidget* parent) { return nativeEditorFactory() ? nativeEditorFactory()(this, parent) : nullptr; }
QWidget* ParametricEQ::createEditor(QWidget* parent) { return nativeEditorFactory() ? nativeEditorFactory()(this, parent) : nullptr; }
QWidget* SpectrumAnalyzer::createEditor(QWidget* parent) { return nativeEditorFactory() ? nativeEditorFactory()(this, parent) : nullptr; }
QWidget* MeterEffect::createEditor(QWidget* parent) { return nativeEditorFactory() ? nativeEditorFactory()(this, parent) : nullptr; }

} // namespace mt
