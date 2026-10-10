#include "ui/PluginWindow.h"
#include "ui/Theme.h"
#include "ui/widgets/WaveView.h"
#include "plugins/Vst3Plugin.h"
#include "plugins/Vst2Plugin.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QComboBox>
#include <QCheckBox>
#include <QMenu>
#include <QInputDialog>
#include <QFileInfo>
#include <QCloseEvent>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QPushButton>

namespace mt {

// ---------------------------------------------------------------- generic panel
// Parameter cells shared by the generic panel and the audio clip editor: every cell is as tall as a knob cell (knob 40 +
// name + value) so knobs, choices and toggles sit level; callers set the width
static QWidget* paramCell(QWidget* parent, QVBoxLayout*& vl) {
    auto* w = new QWidget(parent);
    w->setFixedHeight(GenericParamPanel::kCellH);
    vl = new QVBoxLayout(w);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(1);
    return w;
}
static QWidget* knobCell(QWidget* parent, const QString& name, float pos, float def, bool bipolar, Knob*& knob, QLabel*& value) {
    QVBoxLayout* vl;
    QWidget* w = paramCell(parent, vl);
    knob = new Knob(w, 40);
    knob->setLabel(name);
    knob->setBipolar(bipolar);
    knob->setDefault(def);
    knob->setValue(pos);
    vl->addWidget(knob, 0, Qt::AlignHCenter);
    value = new QLabel(w);
    value->setAlignment(Qt::AlignCenter);
    value->setStyleSheet("color:#d6dae0; font-size:8pt;");
    vl->addWidget(value);
    return w;
}
static QWidget* choiceCell(QWidget* parent, const QString& name, const std::vector<std::string>& items, int cur, QComboBox*& combo) {
    QVBoxLayout* vl;
    QWidget* w = paramCell(parent, vl);
    vl->addStretch(1);
    auto* lbl = theme::dimLabel(name, w, "font-size:8pt;");
    lbl->setAlignment(Qt::AlignCenter);
    vl->addWidget(lbl);
    combo = new QComboBox(w);
    combo->setFixedHeight(22);
    for (auto& i : items) combo->addItem(QString::fromStdString(i));
    combo->setCurrentIndex(cur);
    vl->addWidget(combo);
    vl->addStretch(1);
    return w;
}
static QWidget* toggleCell(QWidget* parent, const QString& name, bool on, QCheckBox*& check) {
    QVBoxLayout* vl;
    QWidget* w = paramCell(parent, vl);
    check = new QCheckBox(name, w);
    check->setChecked(on);
    vl->addStretch(1);
    vl->addWidget(check);
    vl->addStretch(1);
    return w;
}
GenericParamPanel::GenericParamPanel(
    App& app, Id ownerId, IProcessor* proc, std::function<bool(const ParamDesc&)> filter, int columns, QWidget* parent, int cellWidth)
    : QWidget(parent)
    , m_app(app)
    , m_owner(ownerId)
    , m_proc(proc) {
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(6, 6, 6, 6);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(6);
    auto& ps = proc->params();
    int gridRow = 0, gridCol = 0;
    for (int i = 0; i < ps.count(); ++i) {
        const ParamDesc& d = ps.desc(i);
        if (filter && !filter(d)) continue;
        Row row;
        row.id = d.id;
        uint32_t id = d.id;
        QWidget* cellW = nullptr;
        auto contextMenu = [this, id, d](const QPoint& g) {
            QMenu m;
            bool inst = m_app.project().channel(m_owner) != nullptr;
            ParamTarget t;
            t.kind = inst ? ParamTarget::InstrumentParam : ParamTarget::EffectParam;
            t.ownerId = inst ? m_owner : 0;
            t.slotId = inst ? 0 : m_owner;
            t.paramId = id;
            if (!inst)
                for (auto& mt : m_app.project().mixerTracks)
                    for (auto& e : mt.effects)
                        if (e.id == m_owner) t.ownerId = mt.id;
            m.addAction("Create automation clip", [this, t] { m_app.createAutomationLane(t, QString()); });
            m.addAction("Link to MIDI controller...", [this, t, d] { m_app.startMidiLearn(t, QString::fromStdString(d.name)); });
            m.addAction("Reset to default", [this, id, d] { m_app.setProcessorParam(m_owner, id, d.toNormalized(d.def), true); });
            m.exec(g);
        };
        int span = cellWidth > 0 && d.unit == ParamUnit::Choice && !d.choices.empty() ? 2 : 1;
        if (d.unit == ParamUnit::Choice && !d.choices.empty()) {
            cellW = choiceCell(this, QString::fromStdString(d.name), d.choices, ps.getInt(id), row.combo);
            connect(row.combo, &QComboBox::currentIndexChanged, this, [this, id, d](int idx) {
                if (!m_updating) {
                    m_app.setProcessorParam(m_owner, id, d.toNormalized((float)idx), true);
                    Q_EMIT edited();
                }
            });
            row.combo->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(row.combo, &QComboBox::customContextMenuRequested, this,
                [contextMenu, cb = row.combo](const QPoint& p) { contextMenu(cb->mapToGlobal(p)); });
        } else if (d.unit == ParamUnit::Toggle) {
            cellW = toggleCell(this, QString::fromStdString(d.name), ps.getBool(id), row.check);
            connect(row.check, &QCheckBox::toggled, this, [this, id](bool b) {
                if (!m_updating) {
                    m_app.setProcessorParam(m_owner, id, b ? 1.f : 0.f, true);
                    Q_EMIT edited();
                }
            });
            row.check->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(row.check, &QCheckBox::customContextMenuRequested, this,
                [contextMenu, cb = row.check](const QPoint& p) { contextMenu(cb->mapToGlobal(p)); });
        } else {
            cellW = knobCell(this, QString::fromStdString(d.name), ps.getNormalized(id), d.toNormalized(d.def), d.min < 0 && d.max > 0, row.knob,
                row.value);
            row.value->setText(proc->formatValue(id));
            if (!d.automatable) row.knob->setColor(theme::textDim());
            if ((d.unit == ParamUnit::Semitones || d.unit == ParamUnit::Bpm) && !d.steps)
                row.knob->snap = Knob::wholeStepSnap(d.min, d.max);   // land on whole semitones / BPM
            row.knob->valueText = [this, id](float) { return m_proc->formatValue(id); };
            makeValueEditable(row.value, [this, id, d, k = row.knob, lbl = row.value](double v) {
                QString shown = lbl->text();   // typed in the unit on display
                if (d.unit == ParamUnit::Percent) v /= 100.0;
                else if (d.unit == ParamUnit::Seconds && shown.endsWith(" ms")) v /= 1000.0;
                else if (d.unit == ParamUnit::Hertz && shown.endsWith(" kHz")) v *= 1000.0;
                float n = d.toNormalized((float)v);
                m_app.setProcessorParam(m_owner, id, n, true);
                k->setValue(n);
                lbl->setText(m_proc->formatValue(id));
                Q_EMIT edited();
            });
            connect(row.knob, &Knob::valueChanged, this, [this, id, lbl = row.value](float v) {
                if (m_updating) return;
                m_app.setProcessorParam(m_owner, id, v, false);
                lbl->setText(m_proc->formatValue(id));
                Q_EMIT edited();
            });
            connect(
                row.knob, &Knob::dragFinished, this, [this, id, k = row.knob] { m_app.setProcessorParam(m_owner, id, k->value(), true); });
            connect(row.knob, &Knob::contextRequested, this, [contextMenu](const QPoint& g) { contextMenu(g); });
        }
        if (cellWidth > 0) cellW->setFixedWidth(span * cellWidth + (span - 1) * grid->horizontalSpacing());
        if (gridCol > 0 && gridCol + span > columns) {
            ++gridRow;
            gridCol = 0;
        }
        grid->addWidget(cellW, gridRow, gridCol, 1, span);
        gridCol += span;
        m_rows.push_back(row);
    }
    grid->setRowStretch(grid->rowCount(), 1);
    if (cellWidth > 0) grid->setColumnStretch(columns, 1);   // spare width goes after the cells, never between them
}
void GenericParamPanel::sync() {
    m_updating = true;
    auto& ps = m_proc->params();
    for (auto& r : m_rows) {
        if (r.knob) {
            float v = ps.getNormalized(r.id);
            if (std::fabs(v - r.knob->value()) > 1e-4f) {
                r.knob->setValue(v);
                if (r.value) r.value->setText(m_proc->formatValue(r.id));
            }
        } else if (r.combo) {
            int i = ps.getInt(r.id);
            if (i != r.combo->currentIndex()) r.combo->setCurrentIndex(i);
        } else if (r.check) {
            bool b = ps.getBool(r.id);
            if (b != r.check->isChecked()) r.check->setChecked(b);
        }
    }
    m_updating = false;
}

// ---------------------------------------------------------------- plugin window
PluginWindow::PluginWindow(App& app, Id ownerId, const QString& title, QWidget* parent)
    : QWidget(parent, Qt::Tool)
    , m_app(app)
    , m_owner(ownerId)
    , m_titleText(title) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(title);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(4);
    auto* hdr = new QHBoxLayout();
    m_title = new QLabel(title, this);
    m_title->setStyleSheet("font-weight:bold; color:#f2a93b;");
    hdr->addWidget(m_title);
    hdr->addStretch(1);
    m_bypass = new QToolButton(this);
    m_bypass->setText("Bypass");
    m_bypass->setCheckable(true);
    hdr->addWidget(m_bypass);
    m_genericToggle = new QToolButton(this);
    m_genericToggle->setText("Knobs");
    m_genericToggle->setCheckable(true);
    m_genericToggle->setToolTip("Show the generic parameter editor instead of the custom GUI");
    hdr->addWidget(m_genericToggle);
    m_preset = new QToolButton(this);
    m_preset->setText("Presets ▾");
    hdr->addWidget(m_preset);
    lay->addLayout(hdr);
    connect(m_preset, &QToolButton::clicked, this, &PluginWindow::showPresetMenu);
    connect(m_genericToggle, &QToolButton::toggled, this, [this](bool b) {
        m_useGeneric = b;
        build();
    });
    connect(m_bypass, &QToolButton::toggled, this, [this](bool b) {
        for (auto& t : m_app.project().mixerTracks)
            for (auto& e : t.effects)
                if (e.id == m_owner) {
                    m_app.setEffectBypass(t.id, e.id, b);
                    return;
                }
    });
    build();
    connect(&m_timer, &QTimer::timeout, this, &PluginWindow::tick);
    m_timer.start(33);
}
void PluginWindow::refreshTitle(const QString& t) {
    m_titleText = t;
    setWindowTitle(t);
    m_title->setText(t);
}
void PluginWindow::build() {
    if (m_body) {
        delete m_body;
        m_body = nullptr;
        m_generic = nullptr;
    }
    m_proc = m_app.processor(m_owner);
    if (!m_proc) {
        m_body = new QLabel("Processor unavailable (missing plugin?)", this);
        layout()->addWidget(m_body);
        return;
    }
    bool isEffect = m_proc->kind() == ProcessorKind::Effect;
    m_bypass->setVisible(isEffect);
    m_genericToggle->setVisible(m_proc->hasCustomEditor());
    for (auto& t : m_app.project().mixerTracks)
        for (auto& e : t.effects)
            if (e.id == m_owner) m_bypass->setChecked(e.bypass);
    QWidget* custom = (!m_useGeneric && m_proc->hasCustomEditor()) ? m_proc->createEditor(this) : nullptr;
    if (custom) {
        m_body = custom;
        layout()->addWidget(m_body);
        if (auto* v3 = dynamic_cast<Vst3Plugin*>(m_proc)) v3->onParamEdited = [this](uint32_t, float) { m_app.model().setDirty(true); };
        if (auto* v2 = dynamic_cast<Vst2Plugin*>(m_proc)) v2->onParamEdited = [this](uint32_t, float) { m_app.model().setDirty(true); };
    } else {
        auto* sc = new QScrollArea(this);
        sc->setWidgetResizable(true);
        sc->setFrameShape(QFrame::NoFrame);
        m_generic = new GenericParamPanel(m_app, m_owner, m_proc, nullptr, 6, sc);
        sc->setWidget(m_generic);
        m_body = sc;
        layout()->addWidget(m_body);
        int rows = (m_proc->params().count() + 5) / 6;
        resize(6 * 86 + 40, std::min(760, 64 + rows * 84));
    }
    adjustSize();
}
void PluginWindow::tick() {
    IProcessor* p = m_app.processor(m_owner);
    if (p != m_proc) {
        if (!p) {
            close();
            return;
        }
        build();
    }
    if (m_generic) m_generic->sync();
    if (auto* v3 = dynamic_cast<Vst3Plugin*>(m_proc)) v3->syncControllerFromParams();
    if (auto* v2 = dynamic_cast<Vst2Plugin*>(m_proc)) {
        if (m_generic) v2->syncFromPlugin();
    }
}
void PluginWindow::showPresetMenu() {
    if (!m_proc) return;
    QMenu m;
    m.addAction("Save preset...", [this] {
        if (auto n = askName(this, "Save preset", QString())) m_app.savePreset(m_owner, *n);
    });
    m.addAction("Reset to defaults", [this] {
        m_proc->params().resetAll();
        m_app.model().setDirty(true);
    });
    m.addSeparator();
    auto presets = m_app.presetsFor(m_proc->key());
    if (presets.isEmpty()) m.addAction("(no presets)")->setEnabled(false);
    for (auto& f : presets)
        m.addAction(QFileInfo(f).completeBaseName(), [this, f] {
            m_app.loadPreset(m_owner, f);
            if (m_generic) m_generic->sync();
        });
    m.exec(m_preset->mapToGlobal(QPoint(0, m_preset->height())));
}
void PluginWindow::closeEvent(QCloseEvent* e) {
    m_timer.stop();
    QWidget::closeEvent(e);
}

// ---------------------------------------------------------------- audio clip editor
AudioClipEditor::AudioClipEditor(App& app, Id clipId, QWidget* parent)
    : AudioClipEditor(app, clipId, parent, 0) {}
AudioClipEditor::AudioClipEditor(App& app, Id clipId, QWidget* parent, Id sourceId)
    : QWidget(parent, Qt::Tool)
    , m_app(app)
    , m_clip(clipId) {
    setAttribute(Qt::WA_DeleteOnClose);
    const Clip* liveClip = app.project().clip(clipId);
    const AudioSource* s = app.project().audioSource(liveClip ? liveClip->sourceId : sourceId);
    if (!s) {
        deleteLater();
        return;
    }
    // no clip (a project sample not placed yet): the same window edits the settings its clips will start with
    const bool sampleOnly = !liveClip;
    Clip c0;
    if (liveClip) c0 = *liveClip;
    else {
        c0.type = ClipType::Audio;
        c0.sourceId = s->id;
        c0.loopSource = false;   // as App::addAudioClip
        if (s->hasPlaceDefaults) copyClipSettings(s->placeDefaults, c0);
        c0.length = secondsToTicks(4.0 * (s->sampleRate > 0 ? s->frames / s->sampleRate : 1.0), app.project().transport.bpm);   // never cuts the drawing
        m_clip = 0;
    }
    const Clip* c = &c0;
    // Knobs edit this working copy and the waveform draws it, so every movement shows at once; releasing commits it
    m_draft = *c;
    m_draftSrc = *s;
    setWindowTitle(sampleOnly ? s->name : c->name);
    const QString path = s->filePath;
    const Id sid = s->id;
    const int64_t frames = std::max<int64_t>(1, s->frames);
    const double srcSec = s->sampleRate > 0 ? frames / s->sampleRate : 1.0;
    auto bpm = [this] { return m_app.project().transport.bpm; };
    auto speed = [bpm](const Clip& x) { return clipSourceSpeed(x, bpm()); };   // source seconds per clip second
    auto trimSec = [this, srcSec](bool end) {
        double sr = std::max(1.0, m_draftSrc.sampleRate);
        return end ? (m_draftSrc.trimEnd > 0 ? m_draftSrc.trimEnd / sr : srcSec) : m_draftSrc.trimStart / sr;
    };
    auto startSec = [this, bpm, speed, trimSec] { return trimSec(false) + ticksToSeconds(m_draft.sourceOffset, bpm()) * speed(m_draft); };
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 8, 10, 10);
    lay->setSpacing(6);
    // header: same layout as the sampler editor (play + file name, waveform underneath)
    auto* head = new QHBoxLayout();
    auto* play = new QPushButton("▶ Play", this);
    play->setToolTip("Preview the source file (click again to stop). Clip settings are not applied to the preview.");
    connect(play, &QPushButton::clicked, this, [this, path] {
        if (m_previewing) m_app.stopPreview();
        else m_app.previewFile(path);
    });
    connect(&app, &App::previewChanged, this, [this, play, path](const QString& p) {
        m_previewing = p == path;
        play->setText(m_previewing ? "■ Stop" : "▶ Play");
    });
    head->addWidget(play);
    auto* fname = new QLabel(QFileInfo(path).fileName(), this);
    fname->setToolTip(path);
    head->addWidget(fname, 1);
    lay->addLayout(head);
    auto* wave = new WaveView(this);
    wave->asset = [this, path] { return m_app.cache().get(path); };
    connect(&app.cache(), &SampleCache::assetLoaded, wave, [wave] { wave->update(); });   // opened while the file was still loading
    wave->reversed = [this] { return m_draft.reverse != m_draftSrc.reverse; };
    wave->startFrac = [startSec, srcSec] { return (float)clamp(startSec() / srcSec, 0.0, 1.0); };
    // what the clip plays, drawn over the source: gain (and normalize), fades, trims, the clip's end and mute
    wave->envelope = [this, path, srcSec, bpm, speed, trimSec, startSec](double frac) -> float {
        double pos = frac * srcSec;
        if (m_draft.muted || pos < trimSec(false) || pos > trimSec(true)) return 0.f;
        float g = m_draft.gain;
        if (m_draftSrc.normalize)
            if (auto a = m_app.cache().get(path); a && a->peakValue > 0) g /= a->peakValue;
        double t = (pos - startSec()) / speed(m_draft);   // seconds into the clip
        if (t < 0) return g;   // before the start: greyed out by the view
        double len = ticksToSeconds(m_draft.length, bpm());
        if (t > len) return 0.f;
        double fi = ticksToSeconds(m_draft.fadeIn, bpm()), fo = ticksToSeconds(m_draft.fadeOut, bpm());
        if (fi > 0 && t < fi) g *= (float)(t / fi);
        if (fo > 0 && len - t < fo) g *= (float)((len - t) / fo);
        return g;
    };
    wave->showEnvelope = [this] { return m_draft.fadeIn > 0 || m_draft.fadeOut > 0 || std::fabs(m_draft.gain - 1.f) > 1e-3f; };
    wave->setStart = [this, srcSec, bpm, speed, trimSec, sid](float f) {   // click / drag on the waveform sets the start offset
        double sec = std::max(0.0, (f * srcSec - trimSec(false)) / speed(m_draft));
        Tick off = secondsToTicks(sec, bpm());
        m_draft.sourceOffset = off;
        if (m_clip) m_app.edit<Clip>(m_clip, "Start offset", [off](Clip& y) { y.sourceOffset = off; }, 0x5a11);
        else
            m_app.edit<AudioSource>(sid, "Start offset", [d = m_draft](AudioSource& a) {
                a.hasPlaceDefaults = true;
                copyClipSettings(d, a.placeDefaults);
            });
    };
    wave->caption = [sampleOnly] { return QString(sampleOnly ? "not on the playlist yet: settings apply when placed" : "click to set the start"); };
    lay->addWidget(wave);
    // commit the working copy's fields this editor owns (never position / length / track, which the playlist may change meanwhile)
    auto commit = [this, sid](const QString& desc, bool source) {
        if (!m_clip) {   // not placed: the clip settings are the sample's placement defaults
            if (!source) {
                Clip d = m_draft;
                m_app.edit<AudioSource>(sid, desc, [d](AudioSource& a) {
                    a.hasPlaceDefaults = true;
                    copyClipSettings(d, a.placeDefaults);
                });
                m_lengthDirty = false;
                return;
            }
        } else if (m_lengthDirty) {   // only fitting the tempo resizes the clip; otherwise the playlist owns its length
            Tick len = m_draft.length;
            m_app.edit<Clip>(m_clip, desc, [len](Clip& x) { x.length = len; }, 0x5a12);
            m_lengthDirty = false;
        }
        if (source) {
            AudioSource d = m_draftSrc;
            m_app.edit<AudioSource>(sid, desc, [d](AudioSource& a) {
                a.trimStart = d.trimStart;
                a.trimEnd = d.trimEnd;
                a.normalize = d.normalize;
            });
            return;
        }
        Clip d = m_draft;
        m_app.edit<Clip>(m_clip, desc, [d](Clip& x) { copyClipSettings(d, x); });
    };
    // knob groups on the sampler's fixed cell grid, so both editors look the same and knobs line up across rows
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(4);
    auto group = [&](const char* title, int row, int col) {
        auto* box = new QVBoxLayout();
        box->setSpacing(2);
        auto* t = theme::dimLabel(title, this, "font-weight:bold; font-size:8pt;");
        box->addWidget(t);
        auto* cells = new QHBoxLayout();
        cells->setContentsMargins(6, 6, 6, 6);   // as GenericParamPanel
        cells->setSpacing(8);
        box->addLayout(cells);
        grid->addLayout(box, row, col, Qt::AlignLeft | Qt::AlignTop);
        return cells;
    };
    // text: knob position -> label; apply: knob position -> working copy. Moving redraws, releasing commits (one undo step,
    // and a time-stretch render only once)
    auto knob = [&](QHBoxLayout* cells, const QString& name, float pos, float def, bool bipolar, std::function<QString(float)> text,
                    std::function<void(float)> apply, const QString& desc, bool source) {
        Knob* k;
        QLabel* v;
        QWidget* w = knobCell(this, name, pos, def, bipolar, k, v);
        w->setFixedWidth(GenericParamPanel::kCellW);
        k->valueText = text;
        v->setText(text(pos));
        connect(k, &Knob::valueChanged, v, [v, text, apply, wave](float x) {
            v->setText(text(x));
            apply(x);
            wave->update();
        });
        connect(k, &Knob::dragFinished, this, [commit, desc, source] { commit(desc, source); });
        // typed value: every mapping here rises with the knob, so bisect the label's number for the knob position
        makeValueEditable(v, [k, text, commit, desc, source](double target) {
            auto valueAt = [&](float p) {
                bool ok;
                QString t = text(p);
                double x = t.left(t.indexOf(' ')).toDouble(&ok);
                return ok ? x : 1e300;   // "end" (trim end) sits at the top
            };
            float lo = 0.f, hi = 1.f;
            for (int i = 0; i < 40; ++i) {
                float mid = 0.5f * (lo + hi);
                (valueAt(mid) < target ? lo : hi) = mid;
            }
            k->setValue(target >= valueAt(1.f) ? 1.f : hi, true);   // updates the label, the working copy and the waveform
            commit(desc, source);
        });
        cells->addWidget(w);
        return k;
    };
    auto check = [&](QHBoxLayout* cells, const QString& name, bool on, std::function<void(bool)> apply, const QString& desc, bool source) {
        QCheckBox* cb;
        QWidget* w = toggleCell(this, name, on, cb);
        w->setFixedWidth(GenericParamPanel::kCellW);
        connect(cb, &QCheckBox::toggled, this, [apply, commit, desc, source, wave](bool b) {
            apply(b);
            commit(desc, source);
            wave->update();
        });
        cells->addWidget(w);
    };
    auto num = [](double v, int dec, const char* unit) { return QString::number(v, 'f', dec) + unit; };
    // Level
    auto* level = group("Level", 0, 0);
    knob(level, "Gain", (float)((gainToDb(c->gain) + 60) / 84), 60.f / 84, false, [num](float p) { return num(p * 84 - 60, 1, " dB"); },
        [this](float p) { m_draft.gain = dbToGain(p * 84 - 60); }, "Clip gain", false);
    check(level, "Normalize", s->normalize, [this](bool b) { m_draftSrc.normalize = b; }, "Normalize", true);
    // Fades: quadratic so short fades get most of the knob travel
    auto fadePos = [](Tick t) { return (float)std::sqrt(std::min(1.0, ticksToBeats(t) / 32.0)); };
    auto fadeText = [num](float p) { return num(32.0 * p * p, 2, " beats"); };
    auto* fades = group("Fades", 0, 1);
    knob(fades, "Fade in", fadePos(c->fadeIn), 0.f, false, fadeText, [this](float p) { m_draft.fadeIn = beatsToTicks(32.0 * p * p); },
        "Fade in", false);
    knob(fades, "Fade out", fadePos(c->fadeOut), 0.f, false, fadeText, [this](float p) { m_draft.fadeOut = beatsToTicks(32.0 * p * p); },
        "Fade out", false);
    auto combo = [&](QHBoxLayout* cells, const QString& name, const std::vector<std::string>& items, int cur, std::function<void(int)> apply,
                     const QString& desc) {
        QComboBox* cb;
        QWidget* w = choiceCell(this, name, items, cur, cb);
        w->setFixedWidth(2 * GenericParamPanel::kCellW + 8);   // a choice spans two cells, as in the sampler
        connect(cb, &QComboBox::currentIndexChanged, this, [apply, commit, desc, wave](int i) {
            apply(i);
            commit(desc, false);
            wave->update();
        });
        cells->addWidget(w);
        return cb;
    };
    // Pitch & time
    auto* pt = group("Pitch & Time", 0, 2);
    combo(pt, "Mode", stretchModeNames(), clamp(c->stretchMode, 0, (int)stretchModeNames().size() - 1),
        [this](int i) { m_draft.stretchMode = i; }, "Stretch mode")
        ->setToolTip("Resample: pitch, stretch and tempo change the speed together, like a turntable.\n"
                     "Stretch modes: pitch and length are independent (rendered with Rubber Band), tuned for drums, pads or vocals.");
    knob(pt, "Pitch", (c->pitch + 24.f) / 48.f, 0.5f, true, [num](float p) { return num(p * 48 - 24, 2, " st"); },
        [this](float p) { m_draft.pitch = p * 48 - 24; }, "Pitch", false)
        ->snap = Knob::wholeStepSnap(-24, 24);   // lands on whole semitones
    knob(pt, "Stretch", (float)(std::log(clamp(c->timeStretch, 0.25, 4.0) / 0.25) / std::log(16.0)), 0.5f, false,
        [num](float p) { return num(0.25 * std::pow(16.0, p), 3, " x"); }, [this](float p) { m_draft.timeStretch = 0.25 * std::pow(16.0, p); },
        "Time stretch", false);
    // Region
    auto* region = group("Region", 1, 0);
    double lenBeats = std::max(0.01, srcSec * bpm() / 60.0);
    knob(region, "Start offset", (float)std::min(1.0, ticksToBeats(c->sourceOffset) / lenBeats), 0.f, false,
        [num, lenBeats](float p) { return num(p * lenBeats, 2, " beats"); },
        [this, lenBeats](float p) { m_draft.sourceOffset = beatsToTicks(p * lenBeats); }, "Start offset", false);
    knob(region, "Trim start", (float)s->trimStart / frames, 0.f, false, [num, srcSec](float p) { return num(p * srcSec, 2, " s"); },
        [this, frames](float p) { m_draftSrc.trimStart = (int64_t)(p * frames); }, "Trim", true);
    knob(region, "Trim end", s->trimEnd > 0 ? (float)s->trimEnd / frames : 1.f, 1.f, false,
        [num, srcSec](float p) { return p >= 0.999f ? QString("end") : num(p * srcSec, 2, " s"); },
        [this, frames](float p) { m_draftSrc.trimEnd = p >= 0.999f ? 0 : (int64_t)(p * frames); }, "Trim", true);   // 0 = end of file
    // Tempo: follow the project tempo, treating the file as recorded at 'Source BPM' (read from names like "loop 83 bpm.wav")
    auto* tempo = group("Tempo", 1, 2);
    auto bpmKnob = std::make_shared<Knob*>(nullptr);   // created after the checkbox that may set it
    auto bpmPos = [](double b) { return (float)((clamp(b, 40.0, 250.0) - 40.0) / 210.0); };
    check(tempo, "Fit tempo", c->fitTempo, [this, path, trimSec, bpm, bpmKnob](bool b) {
        m_draft.fitTempo = b;
        if (!b) return;
        if (m_draft.sourceBpm <= 0) {
            double guess = AudioFile::bpmFromName(path);
            m_draft.sourceBpm = guess > 0 ? guess : bpm();
            if (*bpmKnob) (*bpmKnob)->setValue((float)((m_draft.sourceBpm - 40.0) / 210.0), true);
        }
        // the clip takes the fitted length of the (trimmed) audio
        double sec = (trimSec(true) - trimSec(false)) / clipSourceSpeed(m_draft, bpm());
        m_draft.length = std::max<Tick>(PPQ / 4, secondsToTicks(sec, bpm()));
        m_lengthDirty = true;
    }, "Fit tempo", false);
    double bpm0 = c->sourceBpm > 0 ? c->sourceBpm : AudioFile::bpmFromName(path);
    *bpmKnob = knob(tempo, "Source BPM", bpmPos(bpm0 > 0 ? bpm0 : bpm()), bpmPos(120), false,
        [num](float p) { return num(40.0 + p * 210.0, 1, " BPM"); }, [this](float p) { m_draft.sourceBpm = 40.0 + p * 210.0; }, "Source BPM", false);
    (*bpmKnob)->snap = Knob::wholeStepSnap(40, 250);
    // Options
    auto* opts = group("Options", 1, 1);
    check(opts, "Reverse", c->reverse, [this](bool b) { m_draft.reverse = b; }, "Reverse", false);
    check(opts, "Loop", c->loopSource, [this](bool b) { m_draft.loopSource = b; }, "Loop", false);
    check(opts, "Mute", c->muted, [this](bool b) { m_draft.muted = b; }, "Mute", false);
    grid->setColumnStretch(3, 1);
    lay->addLayout(grid);
    auto* note = theme::dimLabel("Edits are non-destructive: the original file is never modified. Loop repeats the audio when the clip is longer.", this, "font-size:8pt;");
    lay->addWidget(note);
}

}   // namespace mt
