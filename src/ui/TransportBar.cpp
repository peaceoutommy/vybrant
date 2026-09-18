#include "ui/TransportBar.h"
#include "ui/Theme.h"
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMenu>
#include <QMouseEvent>
#include <QFrame>
#include <QApplication>
#include <QEvent>
#include <QPainter>

namespace mt {

static const struct {
    const char* label;
    int div;
} kSnapItems[] = {{"Line", 0}, {"Bar", -1}, {"Beat", 1}, {"1/2 beat", 2}, {"1/3 beat", 3}, {"1/4 beat", 4}, {"1/6 beat", 6},
    {"1/8 beat", 8}, {"1/16 beat", 16}, {"None", 0}};
QComboBox* TransportBar::makeSnapCombo(QWidget* parent) {
    auto* c = new QComboBox(parent);
    for (auto& s : kSnapItems) c->addItem(s.label);
    c->setCurrentIndex(5);
    c->setToolTip("Snap to grid");
    return c;
}
int TransportBar::snapDivisionFromIndex(int idx) {
    if (idx < 0 || idx >= (int)(sizeof(kSnapItems) / sizeof(kSnapItems[0]))) return 4;
    if (idx == 9) return 0;
    if (idx == 0) return 0;
    return kSnapItems[idx].div;
}
int TransportBar::indexFromSnapDivision(int div) {
    for (int i = 1; i < 9; ++i)
        if (kSnapItems[i].div == div) return i;
    return div == 0 ? 9 : 5;
}

TransportBar::TransportBar(App& app, QWidget* parent)
    : QWidget(parent)
    , m_app(app) {
    auto* rows = new QVBoxLayout(this);
    rows->setContentsMargins(6, 2, 0, 2);
    rows->setSpacing(2);
    auto* lay = new QHBoxLayout();
    lay->setSpacing(3);
    rows->addLayout(lay);
    auto* row2 = new QHBoxLayout();
    row2->setSpacing(3);
    rows->addLayout(row2);
    auto sepIn = [&](QHBoxLayout* l) {
        l->addSpacing(4);
        l->addWidget(theme::barSeparator(this));
        l->addSpacing(4);
    };
    auto sep = [&] { sepIn(lay); };
    auto mkIconIn = [&](QHBoxLayout* l, theme::Glyph g, const QString& tip, bool checkable, const QColor& c = theme::text()) {
        auto* b = new QToolButton(this);
        theme::setGlyph(b, g, 20);
        b->setToolTip(tip);
        b->setCheckable(checkable);
        b->setFocusPolicy(Qt::NoFocus);
        b->setFixedSize(32, theme::kBarH);
        l->addWidget(b);
        return b;
    };
    auto mkIcon = [&](theme::Glyph g, const QString& tip, bool checkable, const QColor& c = theme::text()) {
        return mkIconIn(lay, g, tip, checkable, c);
    };
    auto mkLabel = [&](const QString& t) {
        auto* l = new QLabel(t, this);
        theme::styled(l, "color:@textDim@; font-size:8pt; padding-left:2px;");
        lay->addWidget(l);
        return l;
    };
    // ---- row 1: mode + transport + tempo + time
    m_mode = new ModeSwitch(this);
    lay->addWidget(m_mode);
    m_play = mkIcon(theme::Glyph::Play, "Play / Stop (Space)", true);
    m_play->setStyleSheet("QToolButton:checked { background: #1f3a24; border-color: #2f5a36; }");
    m_stop = mkIcon(theme::Glyph::Stop, "Stop (press twice to return to the start)", false);
    m_record = mkIcon(theme::Glyph::Record, "Record (R)", true, theme::red());
    m_record->setStyleSheet("QToolButton:checked { background: #4a1f1f; border-color: #7a2c2c; }");
    sep();
    m_bpm = new DragValue(this);
    m_bpm->setRange(20, 999, 0.1, 1);
    m_bpm->setFixedSize(84, 28);
    m_bpm->setToolTip(
        "Tempo (BPM): drag or wheel - over the whole number for 1 BPM steps, over the decimal for 0.1 - double-click to type");
    lay->addWidget(m_bpm);
    auto* bpmLbl = mkLabel("BPM");
    (void)bpmLbl;
    m_tsNum = new DragValue(this);
    m_tsNum->setRange(1, 32, 1, 0);
    m_tsNum->setFixedSize(30, 26);
    m_tsNum->setToolTip("Time signature: beats per bar");
    lay->addWidget(m_tsNum);
    auto* slash = new QLabel("/", this);
    theme::styled(slash, "color:@textDim@;");
    lay->addWidget(slash);
    m_tsDen = new DragValue(this);
    m_tsDen->setRange(1, 32, 1, 0);
    m_tsDen->setFixedSize(30, 26);
    m_tsDen->setToolTip("Time signature: beat unit");
    lay->addWidget(m_tsDen);
    sep();
    m_position = new QLabel("001:1:000", this);
    theme::styled(m_position,
        "QLabel { background:@surface@; color:@accent@; border:1px solid @border@; border-radius:3px; padding:1px 9px; font-family: monospace; font-size: 12pt; }");
    m_position->setFixedHeight(28);
    m_position->setToolTip("Song position in bars : beats : ticks - click to jump to a bar");
    lay->addWidget(m_position);
    m_timeSec = new QLabel("0:00.0", this);
    theme::styled(m_timeSec, "color:@textDim@; font-family: monospace; font-size:8pt;");
    m_timeSec->setToolTip("Song position in minutes : seconds");
    lay->addWidget(m_timeSec);
    sep();
    m_metro = mkIcon(theme::Glyph::Metronome, "Metronome (Ctrl+M)", true);
    m_countIn = mkIcon(theme::Glyph::CountIn, "Count-in: one bar of clicks before recording starts (Ctrl+P)", true);
    m_punch = mkIcon(theme::Glyph::Punch, "Punch in/out: only record between the loop points", true);
    m_loopRec = mkIcon(theme::Glyph::Takes, "Loop recording: every pass of the loop becomes a new take", true);
    m_loop = nullptr;
    sep();
    m_typing = mkIcon(theme::Glyph::TypingKeys,
        "Typing keyboard to piano keyboard (Ctrl+T): on = letter keys play the selected channel, off = letter keys are shortcuts", true);
    sep();
    mkLabel("Record");
    m_recMode = new QComboBox(this);
    m_recMode->addItems({"Audio + Notes", "Notes only", "Audio only", "Automation"});
    m_recMode->setToolTip("What recording captures");
    m_recMode->setFixedHeight(theme::kBarH);
    lay->addWidget(m_recMode);
    sep();
    // The hint fills the rest of row 1, flush with the right edge of the window.
    m_hint = new QLabel(this);
    theme::styled(m_hint,
        "QLabel { color:@textDim@; font-size:8pt; padding:0 6px; background:@surface@; border:1px solid @border@; border-radius:3px; }");
    m_hint->setFixedHeight(theme::kBarH);
    m_hint->setMinimumWidth(240);
    m_hint->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    lay->addWidget(m_hint, 1);
    // ---- row 2: hint bar, window buttons, pattern selector, status
    row2->addStretch(1);   // the window buttons / pattern / stats stay on the right, where they have always been
    m_winLayout = new QHBoxLayout();
    m_winLayout->setSpacing(2);
    row2->addLayout(m_winLayout);
    sepIn(row2);
    auto* plbl = new QLabel("Pattern", this);
    theme::styled(plbl, "color:@textDim@; font-size:8pt; padding-left:2px;");
    row2->addWidget(plbl);
    m_pattern = new QComboBox(this);
    m_pattern->setMinimumWidth(130);
    m_pattern->setFixedHeight(26);
    m_pattern->setToolTip("Selected pattern: edited in the Channel Rack and Piano Roll, played in pattern mode");
    row2->addWidget(m_pattern);
    m_newPattern = new QToolButton(this);
    m_newPattern->setText("+");
    m_newPattern->setToolTip("New pattern (F4)");
    m_newPattern->setFixedSize(26, 26);
    m_newPattern->setFocusPolicy(Qt::NoFocus);
    row2->addWidget(m_newPattern);
    sepIn(row2);
    m_midiLed = new QLabel("MIDI", this);
    theme::styled(m_midiLed, "color:@focus@; font-weight:bold; font-size:7.5pt;");
    m_midiLed->setToolTip("Lights up when MIDI input arrives");
    row2->addWidget(m_midiLed);
    m_stats = new QLabel(this);
    theme::styled(m_stats, "color:@textDim@; font-family: monospace; font-size:7.5pt;");
    m_stats->setToolTip("Audio engine: callback CPU load, dropouts (xruns), sample rate / buffer size, output latency, delay compensation");
    row2->addWidget(m_stats);
    qApp->installEventFilter(this);
    m_position->installEventFilter(this);
    setHint(QString());

    connect(m_mode, &ModeSwitch::modeChanged, this, [this](bool song) {
        if (!m_updating) m_app.setSongMode(song);
    });
    connect(m_play, &QToolButton::clicked, this, [this] {
        if (m_app.engine().isPlaying()) m_app.pause();
        else m_app.play();
    });
    connect(m_stop, &QToolButton::clicked, this, [this] { m_app.stop(); });
    connect(m_record, &QToolButton::toggled, this, [this](bool on) {
        if (!m_updating) m_app.record(on);
    });
    connect(m_bpm, &DragValue::valueChanged, this, [this](double v) {
        if (!m_updating) m_app.setBpm(v, false);
    });
    connect(m_bpm, &DragValue::editingFinished, this, [this] {
        if (!m_updating) m_app.setBpm(m_bpm->value(), true);
    });
    connect(m_tsNum, &DragValue::editingFinished, this, [this] {
        if (!m_updating) m_app.setTimeSig((int)m_tsNum->value(), (int)m_tsDen->value());
    });
    connect(m_tsDen, &DragValue::editingFinished, this, [this] {
        if (!m_updating) m_app.setTimeSig((int)m_tsNum->value(), (int)m_tsDen->value());
    });
    connect(m_metro, &QToolButton::toggled, this, [this](bool on) {
        if (!m_updating) m_app.setMetronome(on);
    });
    connect(m_countIn, &QToolButton::toggled, this, [this](bool on) {
        if (!m_updating) m_app.setCountIn(on);
    });
    connect(m_punch, &QToolButton::toggled, this, [this](bool on) {
        if (!m_updating) {
            auto& t = m_app.project().transport;
            m_app.setPunch(on, t.loopStart, t.loopEnd);
        }
    });
    connect(m_loopRec, &QToolButton::toggled, this, [this](bool on) {
        if (!m_updating) m_app.setLoopRecord(on);
    });
    connect(m_recMode, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (!m_updating) m_app.model().editTransport("Record mode", [i](TransportSettings& t) { t.recordMode = i; });
    });
    connect(m_pattern, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (!m_updating && i >= 0) m_app.selectPattern((Id)m_pattern->itemData(i).toULongLong());
    });
    connect(m_newPattern, &QToolButton::clicked, this, [this] { m_app.addPattern(); });
    m_position->installEventFilter(this);
    refresh(ChangeAll);
}

void TransportBar::rebuildPatternCombo() {
    m_pattern->clear();
    for (auto& p : m_app.project().patterns) {
        m_pattern->addItem(p.name, (qulonglong)p.id);
        m_pattern->setItemData(m_pattern->count() - 1, p.color, Qt::DecorationRole);
    }
    for (int i = 0; i < m_pattern->count(); ++i)
        if ((Id)m_pattern->itemData(i).toULongLong() == m_app.project().selectedPatternId) m_pattern->setCurrentIndex(i);
}
void TransportBar::refresh(uint32_t flags) {
    m_updating = true;
    const auto& t = m_app.project().transport;
    m_bpm->setValue(t.bpm);
    m_tsNum->setValue(t.timeSig.numerator);
    m_tsDen->setValue(t.timeSig.denominator);
    m_metro->setChecked(t.metronome);
    m_countIn->setChecked(t.countIn);
    m_punch->setChecked(t.punchEnabled);
    m_loopRec->setChecked(t.loopRecord);
    m_mode->setSong(t.songMode);
    m_recMode->setCurrentIndex(t.recordMode);
    if (flags & (ChangePatterns | ChangeStructure | ChangeMeta | ChangeAll)) rebuildPatternCombo();
    m_updating = false;
}
void TransportBar::uiTick() {
    auto tv = m_app.engine().transport();
    m_updating = true;
    m_play->setChecked(tv.playing);
    m_record->setChecked(tv.recording);
    m_position->setText(tv.countingIn ? "COUNT-IN" : theme::tickToBBT((int64_t)tv.positionTicks, tv.tsNum, tv.tsDen));
    double secs = ticksToSeconds((Tick)tv.positionTicks, tv.bpm);
    m_timeSec->setText(QString("%1:%2").arg((int)secs / 60).arg(secs - (int)secs / 60 * 60, 4, 'f', 1, QChar('0')));
    auto& e = m_app.engine();
    double lat = e.deviceRunning() ? m_app.devices().reportedOutputLatency() * 1000 : 0;
    if (!e.deviceRunning()) {
        m_stats->setText("\u26a0 NO AUDIO DEVICE - Options > Audio settings");
        m_stats->setStyleSheet("color:#e2665c; font-family: monospace; font-size:7.5pt; font-weight:bold;");
        return;
    } else if (m_stats->styleSheet().contains("e2665c"))
        theme::styled(m_stats, "color:@textDim@; font-family: monospace; font-size:7.5pt;");
    m_stats->setText(QString("%1% \u00b7 %2 xr \u00b7 %3k/%4 \u00b7 %5 ms \u00b7 pdc %6")
            .arg((int)(e.cpuLoad() * 100), 2)
            .arg(e.xruns())
            .arg(e.sampleRate() / 1000.0, 0, 'g', 3)
            .arg(m_app.devices().config().bufferSize)
            .arg(lat, 0, 'f', 1)
            .arg(e.totalLatencySamples()));
    int act = m_app.midi().activityCounter();
    m_midiLed->setStyleSheet(act != m_lastMidiActivity ? "color:#5fc074; font-weight:bold; font-size:7.5pt;"
                                                       : "color:#3a4048; font-weight:bold; font-size:7.5pt;");
    m_lastMidiActivity = act;
    m_updating = false;
}

}   // namespace mt

namespace mt {
void ModeSwitch::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QRectF r = rect().adjusted(0, 0, -1, -1);
    p.setPen(QPen(theme::border(), 1));
    p.setBrush(theme::surface());
    p.drawRoundedRect(r, 3, 3);
    auto half = [&](QRectF hr, const QString& t, bool on) {
        if (on) {
            QColor a = theme::accent();
            QColor bgc = theme::bg();
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(
                (bgc.red() + a.red() * 0.30) / 1.30, (bgc.green() + a.green() * 0.30) / 1.30, (bgc.blue() + a.blue() * 0.30) / 1.30));
            p.drawRoundedRect(hr.adjusted(1, 1, -1, -1), 2, 2);
        }
        p.setPen(on ? theme::accent() : theme::textDim());
        QFont f = theme::smallFont();
        f.setBold(on);
        p.setFont(f);
        p.drawText(hr, Qt::AlignCenter, t);
    };
    half(QRectF(0, 0, width(), height() / 2.0), "PAT", !m_song);
    half(QRectF(0, height() / 2.0, width(), height() / 2.0), "SONG", m_song);
}
void ModeSwitch::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    m_song = !m_song;
    update();
    Q_EMIT modeChanged(m_song);
}   // any click flips PAT <-> SONG
void TransportBar::setHint(const QString& text) {
    m_hint->setText(text.isEmpty() ? "Hover over any control for a description - F1 for the quick guide" : text);
}
// Hint bar: shows the tooltip of whatever control the mouse is over (like FL's hint panel)
bool TransportBar::eventFilter(QObject* o, QEvent* e) {
    if (o == m_position && e->type() == QEvent::MouseButtonPress) {
        bool ok;
        const auto& ts = m_app.project().transport.timeSig;
        int bar = QInputDialog::getInt(
            this, "Jump to bar", "Bar:", (int)(m_app.engine().positionTicks() / ts.ticksPerBar()) + 1, 1, 9999, 1, &ok);
        if (ok) m_app.seek((Tick)(bar - 1) * ts.ticksPerBar());
        return true;
    }
    if (e->type() == QEvent::Enter || e->type() == QEvent::Leave) {
        auto* w = qobject_cast<QWidget*>(o);
        if (w && !w->toolTip().isEmpty() && !qobject_cast<QMenu*>(w)) {
            if (e->type() == QEvent::Enter) setHint(w->toolTip().left(160).replace('\n', ' '));
            else setHint(QString());
        }
    }
    return QWidget::eventFilter(o, e);
}
}   // namespace mt
