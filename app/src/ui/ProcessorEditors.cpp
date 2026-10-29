// Custom editors for native processors (installed into the DSP layer through nativeEditorFactory()).
#include "ui/PluginWindow.h"
#include "ui/Theme.h"
#include "ui/widgets/WaveView.h"
#include <array>
#include <QGraphicsOpacityEffect>
#include <QSignalBlocker>
#include <QToolButton>
#include <QSettings>
#include <QInputDialog>
#include <algorithm>
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

static Id ownerOf(IProcessor* p) {
    App* app = App::instance();
    for (auto& [id, proc] : app->engine().registry().processors())
        if (proc.get() == p) return id;
    return 0;
}

// ADSR shape over the sample's duration for the waveform preview (note held for the whole sample)
static float previewEnvelope(double frac, double seconds, float a, float d, float sus, float level) {
    double t = frac * seconds;
    float e;
    if (t < a) e = (float)(t / std::max(a, 0.0005f));
    else {
        double td = t - a;
        e = sus + (1.f - sus) * (float)std::exp(-4.0 * td / std::max(d, 0.0005f));
    }
    return e * level;
}

// ---------------------------------------------------------------- sampler editor
static QWidget* makeSamplerEditor(Sampler* s, QWidget* parent) {
    App& app = *App::instance();
    Id owner = ownerOf(s);
    auto* w = new QWidget(parent);
    auto* lay = new QVBoxLayout(w);
    lay->setContentsMargins(4, 4, 4, 4);
    auto* top = new QHBoxLayout();
    auto* play = new QPushButton("\u25B6 Play", w);
    play->setToolTip("Play the sample at its root note through the channel's mixer track (click again to stop)");
    play->setFocusPolicy(Qt::NoFocus);
    top->addWidget(play);
    auto* load = new QPushButton("Load sample...", w);
    top->addWidget(load);
    auto* name = new QLabel(s->samplePath().isEmpty() ? "(no sample)" : QFileInfo(s->samplePath()).fileName(), w);
    top->addWidget(name, 1);
    lay->addLayout(top);
    {   // FL-style one-shot audition: one click plays the whole sample, a second click stops it
        auto* stopTimer = new QTimer(w);
        stopTimer->setSingleShot(true);
        auto* playing = new bool(false);
        QObject::connect(w, &QObject::destroyed, [playing] { delete playing; });
        auto stop = [&app, s, play, playing, stopTimer] {
            if (*playing) {
                app.noteOff(s->params().getInt(Sampler::pRoot));
                *playing = false;
                play->setText("\u25B6 Play");
                stopTimer->stop();
            }
        };
        QObject::connect(stopTimer, &QTimer::timeout, w, stop);
        QObject::connect(play, &QPushButton::clicked, w, [&app, owner, s, play, playing, stopTimer, stop] {
            if (*playing) {
                stop();
                return;
            }
            app.selectChannel(owner);
            int note = s->params().getInt(Sampler::pRoot);
            app.noteOn(note, 1.f);
            *playing = true;
            play->setText("\u25A0 Stop");
            auto a = s->sample();
            double secs = a
                ? a->seconds() / std::pow(2.0, (s->params().get(Sampler::pTune) + s->params().get(Sampler::pFine) / 100.0) / 12.0)
                : 1.0;
            bool loops = s->params().getInt(Sampler::pLoopMode) != 0;
            if (!loops) stopTimer->start((int)(secs * 1000) + 100);
        });
    }
    auto* wave = new WaveView(w);
    wave->asset = [s] { return s->sample(); };
    wave->startFrac = [s] { return s->params().get(Sampler::pStart); };
    wave->setStart = [&app, owner](float f) { app.setProcessorParam(owner, Sampler::pStart, f, true); };
    wave->reversed = [s] { return s->params().getBool(Sampler::pReverse); };
    wave->loop = [s] {
        auto& ps = s->params();
        if (ps.getInt(Sampler::pLoopMode) == 0) return std::make_pair(0.f, 0.f);
        return std::make_pair(ps.get(Sampler::pLoopStart), ps.get(Sampler::pLoopEnd));
    };
    wave->envelope = [s](double frac) {
        auto& ps = s->params();
        auto a = s->sample();
        double secs = a ? a->seconds() : 1.0;
        return previewEnvelope(
            frac, secs, ps.get(Sampler::pAttack), ps.get(Sampler::pDecay), ps.get(Sampler::pSustain), ps.get(Sampler::pLevel));
    };
    wave->caption = [s] {
        auto& ps = s->params();
        float semis = ps.get(Sampler::pTune) + ps.get(Sampler::pFine) / 100.f;
        QString c = QString("level %1 dB").arg(gainToDb(ps.get(Sampler::pLevel)), 0, 'f', 1);
        if (std::fabs(semis) > 0.01f) c += QString("  |  tune %1 st").arg(semis, 0, 'f', 2);
        if (ps.get(Sampler::pCutoff) < 19000.f)
            c += QString("  |  %1 %2 Hz")
                     .arg(ps.getInt(Sampler::pFilterType) == 0      ? "LP"
                             : ps.getInt(Sampler::pFilterType) == 1 ? "HP"
                                                                    : "BP")
                     .arg((int)ps.get(Sampler::pCutoff));
        return c;
    };
    wave->onDrop = [&app, owner, name](const QString& p) {
        app.setChannelSample(owner, p);
        name->setText(QFileInfo(p).fileName());
    };
    QObject::connect(load, &QPushButton::clicked, w, [&app, owner, name, w] {
        QString p = QFileDialog::getOpenFileName(w, "Load sample", QString(), "Audio (*.wav *.aif *.aiff *.flac *.ogg *.mp3)");
        if (!p.isEmpty()) {
            app.setChannelSample(owner, p);
            name->setText(QFileInfo(p).fileName());
        }
    });
    lay->addWidget(wave);
    // knobs grouped by what they do: sample / envelope / filter / output
    struct Group {
        const char* title;
        std::vector<uint32_t> ids;
        int cols;
    };
    QToolButton* envBtn = nullptr;
    static const std::vector<Group> groups = {{"Envelope", {Sampler::pAttack, Sampler::pDecay, Sampler::pSustain, Sampler::pRelease}, 4},
        {"Sample", {Sampler::pStart, Sampler::pReverse, Sampler::pLoopMode, Sampler::pLoopStart, Sampler::pLoopEnd, Sampler::pDeclick}, 8},
        {"Pitch", {Sampler::pRoot, Sampler::pTune, Sampler::pFine}, 3},
        {"Filter", {Sampler::pFilterType, Sampler::pCutoff, Sampler::pReso}, 4},
        {"Output", {Sampler::pLevel, Sampler::pPan, Sampler::pVelSens, Sampler::pPitchBendRange, Sampler::pGlide}, 5},
        {"Time", {Sampler::pStretchMode, Sampler::pTime, Sampler::pFitTempo, Sampler::pSourceBpm}, 5}};
    // three rows of groups so the window stays compact: Sample + Pitch / Envelope + Filter / Output
    static const int rowOf[] = {0, 0, 1, 1, 2, 2}; /* Envelope + Sample / Pitch + Filter / Output + Time */
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(4);
    std::vector<GenericParamPanel*> panels;
    int col[3] = {0, 0, 0};
    for (size_t i = 0; i < groups.size(); ++i) {
        auto& g = groups[i];
        auto* box = new QVBoxLayout();
        box->setSpacing(2);
        auto* title = theme::dimLabel(g.title, w, "font-weight:bold; font-size:8pt;");
        auto* th = new QHBoxLayout();
        th->setSpacing(6);
        th->addWidget(title);
        if (i ==
            0) {   // envelope on/off: off by default so a new sound plays as it is; the knobs grey out until it is switched on (or touched)
            auto* onBtn = new QToolButton(w);
            onBtn->setText("Off");
            onBtn->setCheckable(true);
            onBtn->setToolTip(
                "Envelope on/off. Off: the sample plays through as it is. Moving an envelope knob or loading a preset switches it on.");
            onBtn->setStyleSheet(
                "QToolButton { padding: 0 8px; font-size: 8pt; font-weight: bold; } QToolButton:checked { color: #f2a93b; }");
            th->addWidget(onBtn);
            envBtn = onBtn;
            QObject::connect(onBtn, &QToolButton::toggled, w, [&app, owner, onBtn](bool on) {
                onBtn->setText(on ? "On" : "Off");
                if (App::instance()->processor(owner)) app.setProcessorParam(owner, Sampler::pEnvOn, on ? 1.f : 0.f, true);
            });
            auto* pb = new QToolButton(w);
            pb->setText("Presets \u25BE");
            pb->setPopupMode(QToolButton::InstantPopup);
            pb->setToolTip("Envelope presets: apply a built-in shape or save the current A/D/S/R as your own");
            pb->setStyleSheet("QToolButton { padding: 0 6px; font-size: 8pt; }");
            th->addWidget(pb);
            auto* pm = new QMenu(pb);
            pb->setMenu(pm);
            struct EnvPreset {
                QString name;
                float a, d, s, r;
            };
            auto applyEnv = [&app, owner, s](const EnvPreset& e) {
                auto& ps = s->params();
                auto set = [&](uint32_t id, float v) { app.setProcessorParam(owner, id, ps.desc(ps.indexOf(id)).toNormalized(v), true); };
                set(Sampler::pAttack, e.a);
                set(Sampler::pDecay, e.d);
                set(Sampler::pSustain, e.s);
                set(Sampler::pRelease, e.r);
            };
            auto userPresets = [] {
                std::vector<EnvPreset> v;
                for (auto& line : QSettings().value("sampler/envPresets").toStringList()) {
                    auto f = line.split('|');
                    if (f.size() == 5) v.push_back({f[0], f[1].toFloat(), f[2].toFloat(), f[3].toFloat(), f[4].toFloat()});
                }
                return v;
            };
            auto saveUser = [](const std::vector<EnvPreset>& v) {
                QStringList l;
                for (auto& e : v) l << QString("%1|%2|%3|%4|%5").arg(e.name).arg(e.a).arg(e.d).arg(e.s).arg(e.r);
                QSettings().setValue("sampler/envPresets", l);
            };
            QObject::connect(pm, &QMenu::aboutToShow, pm, [pm, applyEnv, userPresets, saveUser, s, w] {
                pm->clear();
                static const std::vector<EnvPreset> builtIn = {{"808 / long bass", 0.f, 1.5f, 1.f, 0.25f},
                    {"One-shot (whole sample)", 0.f, 0.1f, 1.f, 0.05f}, {"Punchy", 0.f, 0.35f, 0.f, 0.06f},
                    {"Pluck", 0.f, 0.15f, 0.f, 0.1f}, {"Soft attack", 0.05f, 0.3f, 0.8f, 0.3f}, {"Pad", 0.4f, 0.5f, 0.85f, 0.9f},
                    {"Gate (no tail)", 0.f, 0.1f, 1.f, 0.f}};
                for (auto& e : builtIn) pm->addAction(e.name, [applyEnv, e] { applyEnv(e); });
                auto user = userPresets();
                if (!user.empty()) {
                    pm->addSeparator();
                    for (auto& e : user) pm->addAction(e.name, [applyEnv, e] { applyEnv(e); });
                }
                pm->addSeparator();
                pm->addAction("Save current as...", [s, userPresets, saveUser, w] {
                    auto n = askName(w, "Save envelope preset", "My 808");
                    if (!n) return;
                    auto v = userPresets();
                    auto& ps = s->params();
                    EnvPreset e{
                        *n, ps.get(Sampler::pAttack), ps.get(Sampler::pDecay), ps.get(Sampler::pSustain), ps.get(Sampler::pRelease)};
                    bool replaced = false;
                    for (auto& x : v)
                        if (x.name == e.name) {
                            x = e;
                            replaced = true;
                        }
                    if (!replaced) v.push_back(e);
                    saveUser(v);
                });
                if (!user.empty()) {
                    auto* del = pm->addMenu("Delete preset");
                    for (auto& e : user)
                        del->addAction(e.name, [userPresets, saveUser, name = e.name] {
                            auto v = userPresets();
                            std::erase_if(v, [&](const EnvPreset& x) { return x.name == name; });
                            saveUser(v);
                        });
                }
            });
        }
        th->addStretch(1);
        box->addLayout(th);
        auto ids = g.ids;
        auto* gp = new GenericParamPanel(
            app, owner, s, [ids](const ParamDesc& d) { return std::find(ids.begin(), ids.end(), d.id) != ids.end(); }, g.cols, w,
            GenericParamPanel::kCellW);   // fixed cells: knobs line up across the rows of groups
        box->addWidget(gp);
        int r = rowOf[i];
        grid->addLayout(box, r, col[r]++, Qt::AlignLeft | Qt::AlignTop);
        panels.push_back(gp);
        QObject::connect(gp, &GenericParamPanel::edited, wave, [wave] { wave->update(); });
    }   // the waveform redraws with every knob movement, not on the next timer tick
    grid->setColumnStretch(2, 1);   // groups pack left; groups sharing a column start at the same x
    lay->addLayout(grid);
    auto* envFade = new QGraphicsOpacityEffect(panels[0]);
    envFade->setOpacity(1.0);
    panels[0]->setGraphicsEffect(envFade);
    // the envelope overlay appears once the envelope has been touched (now, or earlier in this project)
    auto* lastEnv = new std::array<float, 4>{};
    {
        auto& ps = s->params();
        *lastEnv = {ps.get(Sampler::pAttack), ps.get(Sampler::pDecay), ps.get(Sampler::pSustain), ps.get(Sampler::pRelease)};
    }
    QObject::connect(w, &QObject::destroyed, [lastEnv] { delete lastEnv; });
    wave->showEnvelope = [s] { return s->params().getBool(Sampler::pEnvOn); };
    auto syncEnvUi = [envBtn, envFade, s] {
        bool on = s->params().getBool(Sampler::pEnvOn);
        if (envBtn->isChecked() != on) {
            QSignalBlocker b(envBtn);
            envBtn->setChecked(on);
            envBtn->setText(on ? "On" : "Off");
        }
        envFade->setOpacity(on ? 1.0 : 0.38);
    };
    syncEnvUi();
    auto* t = new QTimer(w);
    QObject::connect(t, &QTimer::timeout, w, [&app, owner, wave, panels, s, lastEnv, syncEnvUi] {
        auto& ps = s->params();
        std::array<float, 4> cur = {
            ps.get(Sampler::pAttack), ps.get(Sampler::pDecay), ps.get(Sampler::pSustain), ps.get(Sampler::pRelease)};
        if (cur != *lastEnv) {
            *lastEnv = cur;
            if (!ps.getBool(Sampler::pEnvOn)) app.setProcessorParam(owner, Sampler::pEnvOn, 1.f, true);
        }
        syncEnvUi();
        wave->update();
        for (auto* gp : panels) gp->sync();
    });
    t->start(100);
    return w;
}

// ---------------------------------------------------------------- drum sampler editor
class PadGrid : public QWidget {
public:
    PadGrid(App& app, Id owner, DrumSampler* ds, QWidget* parent)
        : QWidget(parent)
        , m_app(app)
        , m_owner(owner)
        , m_ds(ds) {
        setMinimumSize(4 * 70, 4 * 56);
        setAcceptDrops(true);
    }
    int selected = 0;
    std::function<void(int)> onSelect;
    QRect padRect(int i) const {
        int c = i % 4, r = 3 - i / 4;
        int pw = width() / 4, ph = height() / 4;
        return QRect(c * pw + 2, r * ph + 2, pw - 4, ph - 4);
    }
    int padAt(const QPoint& p) const {
        for (int i = 0; i < 16; ++i)
            if (padRect(i).contains(p)) return i;
        return -1;
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setFont(theme::smallFont());
        for (int i = 0; i < 16; ++i) {
            QRect r = padRect(i);
            bool has = m_ds->padSample(i) != nullptr;
            p.setPen(QPen(i == selected ? theme::accent() : theme::border(), i == selected ? 2 : 1));
            p.setBrush(has ? QColor(0x3a, 0x4c, 0x60) : theme::panelLight());
            p.drawRoundedRect(r, 4, 4);
            p.setPen(theme::text());
            QString path = m_ds->padPath(i);
            QString nm = path.startsWith("builtin:") ? path.mid(8) : QFileInfo(path).completeBaseName();
            p.drawText(r.adjusted(4, 2, -4, -2), Qt::AlignTop | Qt::AlignLeft, QString("%1 %2").arg(i + 1).arg(theme::noteName(36 + i)));
            p.drawText(
                r.adjusted(4, 2, -4, -2), Qt::AlignBottom | Qt::AlignLeft, p.fontMetrics().elidedText(nm, Qt::ElideRight, r.width() - 8));
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        int i = padAt(e->pos());
        if (i < 0) return;
        selected = i;
        if (onSelect) onSelect(i);
        update();
        if (e->button() == Qt::LeftButton) {
            m_app.selectChannel(m_owner);
            m_app.noteOn(36 + i);
        } else {
            QMenu m;
            m.addAction("Load sample...", [this, i] {
                QString p = QFileDialog::getOpenFileName(this, "Load sample", QString(), "Audio (*.wav *.aif *.aiff *.flac *.ogg *.mp3)");
                if (!p.isEmpty()) assign(i, p);
            });
            auto* bi = m.addMenu("Built-in sound");
            for (const char* n : {"Kick", "Snare", "Clap", "Hat"})
                bi->addAction(n, [this, i, n] {
                    m_ds->setPadSample(i, DrumSampler::synthesizeDrum(n, m_app.engine().sampleRate()), QString("builtin:") + n);
                    m_app.model().setDirty(true);
                    update();
                });
            m.addAction("Clear", [this, i] {
                m_ds->setPadSample(i, nullptr, QString());
                update();
            });
            m.exec(e->globalPosition().toPoint());
        }
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        int i = padAt(e->pos());
        if (i >= 0) m_app.noteOff(36 + i);
    }
    void assign(int i, const QString& p) {
        auto a = m_app.cache().getSync(p);
        if (a) {
            m_ds->setPadSample(i, a, p);
            m_app.model().setDirty(true);
            update();
        }
    }
    void dragEnterEvent(QDragEnterEvent* e) override {
        if (e->mimeData()->hasUrls()) e->acceptProposedAction();
    }
    void dropEvent(QDropEvent* e) override {
        int i = padAt(e->position().toPoint());
        if (i < 0) return;
        for (auto& u : e->mimeData()->urls()) {
            QString p = u.toLocalFile();
            if (AudioFile::isAudioFile(p)) {
                assign(i, p);
                break;
            }
        }
    }

private:
    App& m_app;
    Id m_owner;
    DrumSampler* m_ds;
};
static QWidget* makeDrumEditor(DrumSampler* ds, QWidget* parent) {
    App& app = *App::instance();
    Id owner = ownerOf(ds);
    auto* w = new QWidget(parent);
    auto* lay = new QHBoxLayout(w);
    lay->setContentsMargins(4, 4, 4, 4);
    auto* left = new QVBoxLayout();
    auto* hint = theme::dimLabel(
        "Pads: click to audition (C1 = pad 1), right-click / drop files to assign. Steps trigger pad 1 at the root note.", w);
    hint->setWordWrap(true);
    left->addWidget(hint);
    auto* grid = new PadGrid(app, owner, ds, w);
    left->addWidget(grid, 1);
    auto* padWave = new WaveView(w);
    padWave->setMinimumHeight(90);
    left->addWidget(padWave);
    auto* playRow = new QHBoxLayout();
    auto* play = new QPushButton("\u25B6 Play pad", w);
    play->setToolTip("Play the selected pad (pads are one-shot)");
    play->setFocusPolicy(Qt::NoFocus);
    playRow->addWidget(play);
    playRow->addStretch(1);
    left->addLayout(playRow);
    lay->addLayout(left, 1);
    QObject::connect(play, &QPushButton::clicked, w, [&app, owner, grid] {
        app.selectChannel(owner);
        app.noteOn(36 + grid->selected, 1.f);
        QTimer::singleShot(50, [&app, n = 36 + grid->selected] { app.noteOff(n); });
    });
    padWave->asset = [ds, grid] { return ds->padSample(grid->selected); };
    padWave->onDrop = [grid](const QString& p) { grid->assign(grid->selected, p); };
    padWave->startFrac = [ds, grid] { return ds->params().get(DrumSampler::padParam(grid->selected, 6)); };
    padWave->setStart = [&app, owner, grid](float f) { app.setProcessorParam(owner, DrumSampler::padParam(grid->selected, 6), f, true); };
    padWave->reversed = [ds, grid] { return ds->params().getBool(DrumSampler::padParam(grid->selected, 5)); };
    padWave->envelope = [ds, grid](double frac) {
        auto& ps = ds->params();
        int pad = grid->selected;
        auto a = ds->padSample(pad);
        double secs = a ? a->seconds() : 1.0;
        float decay = std::min(ps.get(DrumSampler::pDecay), ps.get(DrumSampler::padParam(pad, 3)));
        return previewEnvelope(
            frac, secs, ps.get(DrumSampler::pAttack), decay, 0.f, ps.get(DrumSampler::pLevel) * ps.get(DrumSampler::padParam(pad, 0)));
    };
    padWave->caption = [ds, grid] {
        auto& ps = ds->params();
        int pad = grid->selected;
        float semis = ps.get(DrumSampler::pPitch) + ps.get(DrumSampler::padParam(pad, 2)) + ps.get(DrumSampler::padParam(pad, 7)) / 100.f;
        QString c = QString("pad %1").arg(pad + 1);
        if (std::fabs(semis) > 0.01f) c += QString("  |  pitch %1 st").arg(semis, 0, 'f', 2);
        return c;
    };
    auto* right = new QVBoxLayout();
    auto* padLabel = new QLabel("Pad 1", w);
    padLabel->setStyleSheet("font-weight:bold; color:#f2a93b;");
    right->addWidget(padLabel);
    auto* padHost = new QWidget(w);
    auto* padLay = new QVBoxLayout(padHost);
    padLay->setContentsMargins(0, 0, 0, 0);
    right->addWidget(padHost);
    GenericParamPanel** padPanel = new GenericParamPanel*(nullptr);
    auto rebuildPad = [&app, owner, ds, padHost, padLay, padPanel, padLabel](int pad) {
        if (*padPanel) delete *padPanel;
        padLabel->setText(QString("Pad %1 (%2)").arg(pad + 1).arg(theme::noteName(36 + pad)));
        *padPanel = new GenericParamPanel(
            app, owner, ds,
            [pad](const ParamDesc& d) { return d.id >= DrumSampler::padParam(pad, 0) && d.id < DrumSampler::padParam(pad, 8); }, 4,
            padHost);
        padLay->addWidget(*padPanel);
    };
    rebuildPad(0);
    grid->onSelect = rebuildPad;
    auto* global = new QLabel("Global", w);
    global->setStyleSheet("font-weight:bold; color:#f2a93b;");
    right->addWidget(global);
    auto* gp = new GenericParamPanel(app, owner, ds, [](const ParamDesc& d) { return d.id < DrumSampler::pPadBase; }, 5, w);
    right->addWidget(gp);
    QObject::connect(gp, &GenericParamPanel::edited, padWave, [padWave] { padWave->update(); });
    right->addStretch(1);
    lay->addLayout(right, 1);
    auto* t = new QTimer(w);
    QObject::connect(t, &QTimer::timeout, w, [gp, padPanel, padWave] {
        gp->sync();
        if (*padPanel) (*padPanel)->sync();
        padWave->update();
    });
    t->start(100);
    QObject::connect(w, &QObject::destroyed, [padPanel] { delete padPanel; });
    return w;
}

static NativeEditorRegistration s_sampler(
    {"native.sampler"}, [](IProcessor* p, QWidget* parent) { return makeSamplerEditor(static_cast<Sampler*>(p), parent); });
static NativeEditorRegistration s_drums(
    {"native.drumsampler"}, [](IProcessor* p, QWidget* parent) { return makeDrumEditor(static_cast<DrumSampler*>(p), parent); });

}   // namespace mt
