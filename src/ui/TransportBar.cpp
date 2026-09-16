#include "ui/TransportBar.h"
#include "ui/Theme.h"
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMenu>
#include <QMouseEvent>

namespace mt {

static const struct { const char* label; int div; } kSnapItems[] = {{"Line", 0}, {"Bar", -1}, {"Beat", 1}, {"1/2 beat", 2}, {"1/3 beat", 3}, {"1/4 beat", 4}, {"1/6 beat", 6}, {"1/8 beat", 8}, {"1/16 beat", 16}, {"None", 0}};
QComboBox* TransportBar::makeSnapCombo(QWidget* parent) { auto* c = new QComboBox(parent); for (auto& s : kSnapItems) c->addItem(s.label); c->setCurrentIndex(5); c->setToolTip("Snap to grid"); return c; }
int TransportBar::snapDivisionFromIndex(int idx) { if (idx < 0 || idx >= (int)(sizeof(kSnapItems) / sizeof(kSnapItems[0]))) return 4; if (idx == 9) return 0; if (idx == 0) return 0; return kSnapItems[idx].div; }
int TransportBar::indexFromSnapDivision(int div) { for (int i = 1; i < 9; ++i) if (kSnapItems[i].div == div) return i; return div == 0 ? 9 : 5; }

TransportBar::TransportBar(App& app, QWidget* parent) : QWidget(parent), m_app(app) {
    auto* lay = new QHBoxLayout(this); lay->setContentsMargins(6, 3, 6, 3); lay->setSpacing(6);
    auto mkBtn = [&](const QString& text, const QString& tip, bool checkable) { auto* b = new QToolButton(this); b->setText(text); b->setToolTip(tip); b->setCheckable(checkable); b->setToolButtonStyle(Qt::ToolButtonTextOnly); b->setFocusPolicy(Qt::NoFocus); b->setMinimumWidth(std::max(30, b->fontMetrics().horizontalAdvance(text) + 18)); lay->addWidget(b); return b; };
    m_play = mkBtn("▶", "Play / Pause (Space)", true); m_stop = mkBtn("■", "Stop", false); m_record = mkBtn("●", "Record (R)", true); m_record->setStyleSheet("QToolButton:checked { background: #e25c5c; color: white; border-color: #e25c5c; }");
    lay->addSpacing(8);
    m_patMode = mkBtn("PAT", "Pattern mode: play the selected pattern", true); m_songMode = mkBtn("SONG", "Song mode: play the playlist", true);
    lay->addSpacing(8); m_winLayout = new QHBoxLayout(); m_winLayout->setSpacing(2); lay->addLayout(m_winLayout); lay->addSpacing(8);
    auto mkLabel = [&](const QString& t) { auto* l = new QLabel(t, this); l->setStyleSheet("color:#8a919a;"); lay->addWidget(l); return l; };
    mkLabel("BPM"); m_bpm = new DragValue(this); m_bpm->setRange(20, 999, 0.1, 1); m_bpm->setMinimumWidth(70); m_bpm->setToolTip("Tempo (drag / wheel / double-click)"); lay->addWidget(m_bpm);
    m_tsNum = new DragValue(this); m_tsNum->setRange(1, 32, 1, 0); m_tsNum->setMinimumWidth(34); m_tsNum->setToolTip("Time signature numerator"); lay->addWidget(m_tsNum); mkLabel("/"); m_tsDen = new DragValue(this); m_tsDen->setRange(1, 32, 1, 0); m_tsDen->setMinimumWidth(34); m_tsDen->setToolTip("Time signature denominator"); lay->addWidget(m_tsDen);
    m_position = new QLabel("001:1:000", this); m_position->setStyleSheet("QLabel { background:#15181c; color:#f2a93b; border:1px solid #3a4048; border-radius:3px; padding:2px 8px; font-family: monospace; font-size: 12pt; }"); m_position->setToolTip("Position (bar:beat:tick) - click to jump"); lay->addWidget(m_position);
    lay->addSpacing(8);
    m_loop = mkBtn("LOOP", "Loop the loop region (song mode)", true); m_metro = mkBtn("⏱", "Metronome", true); m_countIn = mkBtn("C-IN", "Count-in before recording", true); m_punch = mkBtn("PUNCH", "Punch in/out recording between loop points", true); m_loopRec = mkBtn("TAKES", "Loop recording: new take on every loop pass", true);
    lay->addSpacing(8);
    mkLabel("Snap"); m_snap = makeSnapCombo(this); lay->addWidget(m_snap);
    mkLabel("Rec"); m_recMode = new QComboBox(this); m_recMode->addItems({"Audio + Notes", "Notes only", "Audio only", "Automation"}); m_recMode->setToolTip("Recording mode"); lay->addWidget(m_recMode);
    lay->addSpacing(8);
    mkLabel("Pattern"); m_pattern = new QComboBox(this); m_pattern->setMinimumWidth(140); m_pattern->setToolTip("Selected pattern (edited in Channel Rack / Piano Roll, played in pattern mode)"); lay->addWidget(m_pattern);
    m_newPattern = mkBtn("+", "New pattern", false); m_newPattern->setMinimumWidth(22);
    lay->addStretch(1);
    m_midiLed = new QLabel("MIDI", this); m_midiLed->setStyleSheet("color:#4e5866; font-weight:bold;"); lay->addWidget(m_midiLed);
    m_stats = new QLabel(this); m_stats->setStyleSheet("color:#8a919a; font-family: monospace;"); m_stats->setToolTip("CPU load of the audio callback / xruns / buffer / latency compensation"); lay->addWidget(m_stats);
    m_masterVol = new Knob(this, 26); m_masterVol->setLabel("Master"); m_masterVol->valueText = [](float v) { return QString::number(gainToDb(v * 1.25f / 0.8f), 'f', 1) + " dB"; }; lay->addWidget(m_masterVol);

    connect(m_play, &QToolButton::clicked, this, [this] { if (m_app.engine().isPlaying()) m_app.pause(); else m_app.play(); });
    connect(m_stop, &QToolButton::clicked, this, [this] { m_app.stop(); });
    connect(m_record, &QToolButton::toggled, this, [this](bool on) { if (!m_updating) m_app.record(on); });
    connect(m_patMode, &QToolButton::clicked, this, [this] { m_app.setSongMode(false); });
    connect(m_songMode, &QToolButton::clicked, this, [this] { m_app.setSongMode(true); });
    connect(m_bpm, &DragValue::valueChanged, this, [this](double v) { if (!m_updating) m_app.setBpm(v, false); });
    connect(m_bpm, &DragValue::editingFinished, this, [this] { if (!m_updating) m_app.setBpm(m_bpm->value(), true); });
    connect(m_tsNum, &DragValue::editingFinished, this, [this] { if (!m_updating) m_app.setTimeSig((int)m_tsNum->value(), (int)m_tsDen->value()); });
    connect(m_tsDen, &DragValue::editingFinished, this, [this] { if (!m_updating) m_app.setTimeSig((int)m_tsNum->value(), (int)m_tsDen->value()); });
    connect(m_loop, &QToolButton::toggled, this, [this](bool on) { if (!m_updating) m_app.setLoop(on); });
    connect(m_metro, &QToolButton::toggled, this, [this](bool on) { if (!m_updating) m_app.setMetronome(on); });
    connect(m_countIn, &QToolButton::toggled, this, [this](bool on) { if (!m_updating) m_app.setCountIn(on); });
    connect(m_punch, &QToolButton::toggled, this, [this](bool on) { if (!m_updating) { auto& t = m_app.project().transport; m_app.setPunch(on, t.loopStart, t.loopEnd); } });
    connect(m_loopRec, &QToolButton::toggled, this, [this](bool on) { if (!m_updating) m_app.setLoopRecord(on); });
    connect(m_snap, &QComboBox::currentIndexChanged, this, [this](int i) { if (!m_updating) m_app.setSnap(snapDivisionFromIndex(i)); });
    connect(m_recMode, &QComboBox::currentIndexChanged, this, [this](int i) { if (!m_updating) m_app.model().editTransport("Record mode", [i](TransportSettings& t) { t.recordMode = i; }); });
    connect(m_pattern, &QComboBox::currentIndexChanged, this, [this](int i) { if (!m_updating && i >= 0) m_app.selectPattern((Id)m_pattern->itemData(i).toULongLong()); });
    connect(m_newPattern, &QToolButton::clicked, this, [this] { m_app.addPattern(); });
    connect(m_masterVol, &Knob::valueChanged, this, [this](float v) { m_app.setMixerVolume(m_app.project().masterId(), v * 1.25f, false); });
    connect(m_masterVol, &Knob::dragFinished, this, [this] { m_app.setMixerVolume(m_app.project().masterId(), m_masterVol->value() * 1.25f, true); });
    m_position->installEventFilter(this);
    refresh(ChangeAll);
}

void TransportBar::rebuildPatternCombo() {
    m_pattern->clear(); for (auto& p : m_app.project().patterns) { m_pattern->addItem(p.name, (qulonglong)p.id); m_pattern->setItemData(m_pattern->count() - 1, p.color, Qt::DecorationRole); }
    for (int i = 0; i < m_pattern->count(); ++i) if ((Id)m_pattern->itemData(i).toULongLong() == m_app.project().selectedPatternId) m_pattern->setCurrentIndex(i);
}
void TransportBar::refresh(uint32_t flags) {
    m_updating = true; const auto& t = m_app.project().transport;
    m_bpm->setValue(t.bpm); m_tsNum->setValue(t.timeSig.numerator); m_tsDen->setValue(t.timeSig.denominator); m_loop->setChecked(t.loopEnabled); m_metro->setChecked(t.metronome); m_countIn->setChecked(t.countIn); m_punch->setChecked(t.punchEnabled); m_loopRec->setChecked(t.loopRecord);
    m_patMode->setChecked(!t.songMode); m_songMode->setChecked(t.songMode); m_snap->setCurrentIndex(t.snap ? indexFromSnapDivision(t.snapDivision) : 9); m_recMode->setCurrentIndex(t.recordMode);
    if (flags & (ChangePatterns | ChangeStructure | ChangeMeta | ChangeAll)) rebuildPatternCombo();
    if (auto* m = m_app.project().master()) m_masterVol->setValue(m->volume / 1.25f);
    m_updating = false;
}
void TransportBar::uiTick() {
    auto tv = m_app.engine().transport(); m_updating = true;
    m_play->setChecked(tv.playing); m_record->setChecked(tv.recording);
    m_position->setText(tv.countingIn ? "COUNT-IN" : theme::tickToBBT((int64_t)tv.positionTicks, tv.tsNum, tv.tsDen));
    auto& e = m_app.engine(); double lat = e.deviceRunning() ? m_app.devices().reportedOutputLatency() * 1000 : 0;
    m_stats->setText(QString("CPU %1%  xruns %2  %3 Hz / %4  lat %5 ms  PDC %6").arg((int)(e.cpuLoad() * 100), 3).arg(e.xruns()).arg((int)e.sampleRate()).arg(m_app.devices().config().bufferSize).arg(lat, 0, 'f', 1).arg(e.totalLatencySamples()));
    int act = m_app.midi().activityCounter(); m_midiLed->setStyleSheet(act != m_lastMidiActivity ? "color:#5dc26d; font-weight:bold;" : "color:#4e5866; font-weight:bold;"); m_lastMidiActivity = act;
    if (auto* m = m_app.project().master()) { float v = m_app.engine().registry().track(m->id).volume.load(); m_masterVol->setValue(v / 1.25f); }
    m_updating = false;
}

} // namespace mt

namespace mt {
bool TransportBar::eventFilter(QObject* o, QEvent* e) {
    if (o == m_position && e->type() == QEvent::MouseButtonPress) { bool ok; const auto& ts = m_app.project().transport.timeSig; int bar = QInputDialog::getInt(this, "Jump to bar", "Bar:", (int)(m_app.engine().positionTicks() / ts.ticksPerBar()) + 1, 1, 9999, 1, &ok); if (ok) m_app.seek((Tick)(bar - 1) * ts.ticksPerBar()); return true; }
    return QWidget::eventFilter(o, e);
}
}
