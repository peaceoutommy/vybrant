#include "ui/PluginWindow.h"
#include "ui/Theme.h"
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
GenericParamPanel::GenericParamPanel(App& app, Id ownerId, IProcessor* proc, std::function<bool(const ParamDesc&)> filter, int columns, QWidget* parent) : QWidget(parent), m_app(app), m_owner(ownerId), m_proc(proc) {
    auto* grid = new QGridLayout(this); grid->setContentsMargins(6, 6, 6, 6); grid->setHorizontalSpacing(8); grid->setVerticalSpacing(6);
    auto& ps = proc->params(); int cell = 0;
    for (int i = 0; i < ps.count(); ++i) {
        const ParamDesc& d = ps.desc(i); if (filter && !filter(d)) continue; Row row; row.id = d.id; uint32_t id = d.id;
        auto* cellW = new QWidget(this); auto* vl = new QVBoxLayout(cellW); vl->setContentsMargins(0, 0, 0, 0); vl->setSpacing(1);
        auto contextMenu = [this, id, d](const QPoint& g) { QMenu m; bool inst = m_app.project().channel(m_owner) != nullptr; ParamTarget t; t.kind = inst ? ParamTarget::InstrumentParam : ParamTarget::EffectParam; t.ownerId = inst ? m_owner : 0; t.slotId = inst ? 0 : m_owner; t.paramId = id; if (!inst) for (auto& mt : m_app.project().mixerTracks) for (auto& e : mt.effects) if (e.id == m_owner) t.ownerId = mt.id; m.addAction("Create automation clip", [this, t] { m_app.createAutomationLane(t, QString()); }); m.addAction("Link to MIDI controller...", [this, t, d] { m_app.startMidiLearn(t, QString::fromStdString(d.name)); }); m.addAction("Reset to default", [this, id, d] { m_app.setProcessorParam(m_owner, id, d.toNormalized(d.def), true); }); m.exec(g); };
        if (d.unit == ParamUnit::Choice && !d.choices.empty()) { auto* lbl = new QLabel(QString::fromStdString(d.name), cellW); theme::styled(lbl, "color:@textDim@; font-size:8pt;"); lbl->setAlignment(Qt::AlignCenter); vl->addWidget(lbl); row.combo = new QComboBox(cellW); for (auto& c : d.choices) row.combo->addItem(QString::fromStdString(c)); row.combo->setCurrentIndex(ps.getInt(id)); vl->addWidget(row.combo); connect(row.combo, &QComboBox::currentIndexChanged, this, [this, id, d](int idx) { if (!m_updating) m_app.setProcessorParam(m_owner, id, d.toNormalized((float)idx), true); }); row.combo->setContextMenuPolicy(Qt::CustomContextMenu); connect(row.combo, &QComboBox::customContextMenuRequested, this, [contextMenu, cb = row.combo](const QPoint& p) { contextMenu(cb->mapToGlobal(p)); }); }
        else if (d.unit == ParamUnit::Toggle) { row.check = new QCheckBox(QString::fromStdString(d.name), cellW); row.check->setChecked(ps.getBool(id)); vl->addStretch(1); vl->addWidget(row.check); vl->addStretch(1); connect(row.check, &QCheckBox::toggled, this, [this, id](bool b) { if (!m_updating) m_app.setProcessorParam(m_owner, id, b ? 1.f : 0.f, true); }); row.check->setContextMenuPolicy(Qt::CustomContextMenu); connect(row.check, &QCheckBox::customContextMenuRequested, this, [contextMenu, cb = row.check](const QPoint& p) { contextMenu(cb->mapToGlobal(p)); }); }
        else { row.knob = new Knob(cellW, 40); row.knob->setLabel(QString::fromStdString(d.name)); row.knob->setBipolar(d.min < 0 && d.max > 0); row.knob->setDefault(d.toNormalized(d.def)); row.knob->setValue(ps.getNormalized(id)); if (!d.automatable) row.knob->setColor(theme::textDim()); row.knob->valueText = [this, id](float) { return m_proc->formatValue(id); }; vl->addWidget(row.knob, 0, Qt::AlignHCenter); row.value = new QLabel(proc->formatValue(id), cellW); row.value->setAlignment(Qt::AlignCenter); row.value->setStyleSheet("color:#d6dae0; font-size:8pt;"); vl->addWidget(row.value);
            connect(row.knob, &Knob::valueChanged, this, [this, id, lbl = row.value](float v) { if (m_updating) return; m_app.setProcessorParam(m_owner, id, v, false); lbl->setText(m_proc->formatValue(id)); }); connect(row.knob, &Knob::dragFinished, this, [this, id, k = row.knob] { m_app.setProcessorParam(m_owner, id, k->value(), true); }); connect(row.knob, &Knob::contextRequested, this, [contextMenu](const QPoint& g) { contextMenu(g); }); }
        grid->addWidget(cellW, cell / columns, cell % columns); ++cell; m_rows.push_back(row);
    }
    grid->setRowStretch(grid->rowCount(), 1);
}
void GenericParamPanel::sync() { m_updating = true; auto& ps = m_proc->params(); for (auto& r : m_rows) { if (r.knob) { float v = ps.getNormalized(r.id); if (std::fabs(v - r.knob->value()) > 1e-4f) { r.knob->setValue(v); if (r.value) r.value->setText(m_proc->formatValue(r.id)); } } else if (r.combo) { int i = ps.getInt(r.id); if (i != r.combo->currentIndex()) r.combo->setCurrentIndex(i); } else if (r.check) { bool b = ps.getBool(r.id); if (b != r.check->isChecked()) r.check->setChecked(b); } } m_updating = false; }

// ---------------------------------------------------------------- plugin window
PluginWindow::PluginWindow(App& app, Id ownerId, const QString& title, QWidget* parent) : QWidget(parent, Qt::Tool), m_app(app), m_owner(ownerId), m_titleText(title) {
    setAttribute(Qt::WA_DeleteOnClose); setWindowTitle(title);
    auto* lay = new QVBoxLayout(this); lay->setContentsMargins(4, 4, 4, 4); lay->setSpacing(4); auto* hdr = new QHBoxLayout();
    m_title = new QLabel(title, this); m_title->setStyleSheet("font-weight:bold; color:#f2a93b;"); hdr->addWidget(m_title); hdr->addStretch(1);
    m_bypass = new QToolButton(this); m_bypass->setText("Bypass"); m_bypass->setCheckable(true); hdr->addWidget(m_bypass);
    m_genericToggle = new QToolButton(this); m_genericToggle->setText("Knobs"); m_genericToggle->setCheckable(true); m_genericToggle->setToolTip("Show the generic parameter editor instead of the custom GUI"); hdr->addWidget(m_genericToggle);
    m_preset = new QToolButton(this); m_preset->setText("Presets ▾"); hdr->addWidget(m_preset); lay->addLayout(hdr);
    connect(m_preset, &QToolButton::clicked, this, &PluginWindow::showPresetMenu); connect(m_genericToggle, &QToolButton::toggled, this, [this](bool b) { m_useGeneric = b; build(); });
    connect(m_bypass, &QToolButton::toggled, this, [this](bool b) { for (auto& t : m_app.project().mixerTracks) for (auto& e : t.effects) if (e.id == m_owner) { m_app.setEffectBypass(t.id, e.id, b); return; } });
    build(); connect(&m_timer, &QTimer::timeout, this, &PluginWindow::tick); m_timer.start(33);
}
void PluginWindow::refreshTitle(const QString& t) { m_titleText = t; setWindowTitle(t); m_title->setText(t); }
void PluginWindow::build() {
    if (m_body) { delete m_body; m_body = nullptr; m_generic = nullptr; }
    m_proc = m_app.processor(m_owner); if (!m_proc) { m_body = new QLabel("Processor unavailable (missing plugin?)", this); layout()->addWidget(m_body); return; }
    bool isEffect = m_proc->kind() == ProcessorKind::Effect; m_bypass->setVisible(isEffect); m_genericToggle->setVisible(m_proc->hasCustomEditor());
    for (auto& t : m_app.project().mixerTracks) for (auto& e : t.effects) if (e.id == m_owner) m_bypass->setChecked(e.bypass);
    QWidget* custom = (!m_useGeneric && m_proc->hasCustomEditor()) ? m_proc->createEditor(this) : nullptr;
    if (custom) { m_body = custom; layout()->addWidget(m_body); if (auto* v3 = dynamic_cast<Vst3Plugin*>(m_proc)) v3->onParamEdited = [this](uint32_t, float) { m_app.model().setDirty(true); }; if (auto* v2 = dynamic_cast<Vst2Plugin*>(m_proc)) v2->onParamEdited = [this](uint32_t, float) { m_app.model().setDirty(true); }; }
    else { auto* sc = new QScrollArea(this); sc->setWidgetResizable(true); sc->setFrameShape(QFrame::NoFrame); m_generic = new GenericParamPanel(m_app, m_owner, m_proc, nullptr, 6, sc); sc->setWidget(m_generic); m_body = sc; layout()->addWidget(m_body); int rows = (m_proc->params().count() + 5) / 6; resize(6 * 86 + 40, std::min(760, 64 + rows * 84)); }
    adjustSize();
}
void PluginWindow::tick() {
    IProcessor* p = m_app.processor(m_owner); if (p != m_proc) { if (!p) { close(); return; } build(); }
    if (m_generic) m_generic->sync();
    if (auto* v3 = dynamic_cast<Vst3Plugin*>(m_proc)) v3->syncControllerFromParams();
    if (auto* v2 = dynamic_cast<Vst2Plugin*>(m_proc)) { if (m_generic) v2->syncFromPlugin(); }
}
void PluginWindow::showPresetMenu() {
    if (!m_proc) return; QMenu m; m.addAction("Save preset...", [this] { bool ok; QString n = QInputDialog::getText(this, "Save preset", "Name:", QLineEdit::Normal, QString(), &ok); if (ok && !n.isEmpty()) m_app.savePreset(m_owner, n); });
    m.addAction("Reset to defaults", [this] { m_proc->params().resetAll(); m_app.model().setDirty(true); }); m.addSeparator();
    auto presets = m_app.presetsFor(m_proc->key()); if (presets.isEmpty()) m.addAction("(no presets)")->setEnabled(false); for (auto& f : presets) m.addAction(QFileInfo(f).completeBaseName(), [this, f] { m_app.loadPreset(m_owner, f); if (m_generic) m_generic->sync(); });
    m.exec(m_preset->mapToGlobal(QPoint(0, m_preset->height())));
}
void PluginWindow::closeEvent(QCloseEvent* e) { m_timer.stop(); QWidget::closeEvent(e); }

// ---------------------------------------------------------------- audio clip editor
AudioClipEditor::AudioClipEditor(App& app, Id clipId, QWidget* parent) : QWidget(parent, Qt::Tool), m_app(app), m_clip(clipId) {
    setAttribute(Qt::WA_DeleteOnClose); const Clip* c = app.project().clip(clipId); const AudioSource* s = c ? app.project().audioSource(c->sourceId) : nullptr; if (!c || !s) { deleteLater(); return; }
    setWindowTitle("Audio clip: " + c->name); auto* form = new QFormLayout(this);
    auto* info = new QLabel(QString("%1\n%2 Hz, %3 ch, %4 s").arg(s->filePath).arg(s->sampleRate).arg(s->channels).arg(s->sampleRate > 0 ? s->frames / s->sampleRate : 0, 0, 'f', 2), this); info->setWordWrap(true); form->addRow(info);
    auto mkSpin = [&](double min, double max, double val, int dec, const QString& suffix) { auto* sp = new QDoubleSpinBox(this); sp->setRange(min, max); sp->setDecimals(dec); sp->setValue(val); sp->setSuffix(suffix); return sp; };
    auto* gain = mkSpin(-60, 24, gainToDb(c->gain), 1, " dB"); form->addRow("Clip gain", gain); connect(gain, &QDoubleSpinBox::editingFinished, this, [this, gain] { double db = gain->value(); m_app.editClip(m_clip, "Clip gain", [db](Clip& x) { x.gain = dbToGain((float)db); }); });
    auto* fi = mkSpin(0, 64, ticksToBeats(c->fadeIn), 2, " beats"); form->addRow("Fade in", fi); connect(fi, &QDoubleSpinBox::editingFinished, this, [this, fi] { double b = fi->value(); m_app.editClip(m_clip, "Fade in", [b](Clip& x) { x.fadeIn = beatsToTicks(b); }); });
    auto* fo = mkSpin(0, 64, ticksToBeats(c->fadeOut), 2, " beats"); form->addRow("Fade out", fo); connect(fo, &QDoubleSpinBox::editingFinished, this, [this, fo] { double b = fo->value(); m_app.editClip(m_clip, "Fade out", [b](Clip& x) { x.fadeOut = beatsToTicks(b); }); });
    auto* stretch = mkSpin(0.25, 4.0, c->timeStretch, 3, " x"); stretch->setToolTip("Time stretch ratio (1 = original length). Rendered offline with Rubber Band; pitch is preserved."); form->addRow("Time stretch", stretch); connect(stretch, &QDoubleSpinBox::editingFinished, this, [this, stretch] { double r = stretch->value(); m_app.editClip(m_clip, "Time stretch", [r](Clip& x) { x.timeStretch = r; }); });
    auto* pitch = mkSpin(-24, 24, c->pitch, 2, " st"); pitch->setToolTip("Pitch shift in semitones (with stretch = 1 this changes speed like a turntable; with stretch != 1 the pitch is shifted independently)"); form->addRow("Pitch", pitch); connect(pitch, &QDoubleSpinBox::editingFinished, this, [this, pitch] { double p = pitch->value(); m_app.editClip(m_clip, "Pitch", [p](Clip& x) { x.pitch = (float)p; }); });
    auto* off = mkSpin(0, 100000, ticksToBeats(c->sourceOffset), 2, " beats"); form->addRow("Start offset", off); connect(off, &QDoubleSpinBox::editingFinished, this, [this, off] { double b = off->value(); m_app.editClip(m_clip, "Start offset", [b](Clip& x) { x.sourceOffset = beatsToTicks(b); }); });
    auto* ts = mkSpin(0, 1e9, (double)s->trimStart, 0, " frames"); form->addRow("Trim start (source)", ts); connect(ts, &QDoubleSpinBox::editingFinished, this, [this, ts, sid = s->id] { double v = ts->value(); m_app.editAudioSource(sid, "Trim", [v](AudioSource& a) { a.trimStart = (int64_t)v; }); });
    auto* te = mkSpin(0, 1e9, (double)s->trimEnd, 0, " frames"); te->setToolTip("0 = end of file"); form->addRow("Trim end (source)", te); connect(te, &QDoubleSpinBox::editingFinished, this, [this, te, sid = s->id] { double v = te->value(); m_app.editAudioSource(sid, "Trim", [v](AudioSource& a) { a.trimEnd = (int64_t)v; }); });
    auto* rev = new QCheckBox("Reverse", this); rev->setChecked(c->reverse); form->addRow(rev); connect(rev, &QCheckBox::toggled, this, [this](bool b) { m_app.editClip(m_clip, "Reverse", [b](Clip& x) { x.reverse = b; }); });
    auto* norm = new QCheckBox("Normalize (source)", this); norm->setChecked(s->normalize); form->addRow(norm); connect(norm, &QCheckBox::toggled, this, [this, sid = s->id](bool b) { m_app.editAudioSource(sid, "Normalize", [b](AudioSource& a) { a.normalize = b; }); });
    auto* loop = new QCheckBox("Loop when clip is longer than the audio", this); loop->setChecked(c->loopSource); form->addRow(loop); connect(loop, &QCheckBox::toggled, this, [this](bool b) { m_app.editClip(m_clip, "Loop", [b](Clip& x) { x.loopSource = b; }); });
    auto* mute = new QCheckBox("Mute clip", this); mute->setChecked(c->muted); form->addRow(mute); connect(mute, &QCheckBox::toggled, this, [this](bool b) { m_app.editClip(m_clip, "Mute", [b](Clip& x) { x.muted = b; }); });
    auto* note = new QLabel("Edits are non-destructive: the original file is never modified.", this); theme::styled(note, "color:@textDim@;"); form->addRow(note);
}

} // namespace mt
