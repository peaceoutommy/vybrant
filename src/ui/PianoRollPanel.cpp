#include "ui/PianoRollPanel.h"
#include "ui/TransportBar.h"
#include "ui/Theme.h"
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QInputDialog>
#include <QFrame>
#include <algorithm>
#include <climits>
#include <QDateTime>
#include <QSettings>
#include <QApplication>

namespace mt {

// Scale highlighting: the set FL Studio ships (modes, pentatonics, blues, symmetric and "world" scales), as semitone offsets from the root
struct ScaleDef {
    const char* name;
    std::vector<int> steps;
};
static const std::vector<ScaleDef>& scaleDefs() {
    static const std::vector<ScaleDef> d = {
        {"Off", {}},
        {"Major (Ionian)", {0, 2, 4, 5, 7, 9, 11}},
        {"Minor (Aeolian)", {0, 2, 3, 5, 7, 8, 10}},
        {"Harmonic minor", {0, 2, 3, 5, 7, 8, 11}},
        {"Melodic minor", {0, 2, 3, 5, 7, 9, 11}},
        {"Dorian", {0, 2, 3, 5, 7, 9, 10}},
        {"Phrygian", {0, 1, 3, 5, 7, 8, 10}},
        {"Lydian", {0, 2, 4, 6, 7, 9, 11}},
        {"Mixolydian", {0, 2, 4, 5, 7, 9, 10}},
        {"Locrian", {0, 1, 3, 5, 6, 8, 10}},
        {"Major pentatonic", {0, 2, 4, 7, 9}},
        {"Minor pentatonic", {0, 3, 5, 7, 10}},
        {"Blues", {0, 3, 5, 6, 7, 10}},
        {"Major blues", {0, 2, 3, 4, 7, 9}},
        {"Whole tone", {0, 2, 4, 6, 8, 10}},
        {"Diminished (whole-half)", {0, 2, 3, 5, 6, 8, 9, 11}},
        {"Diminished (half-whole)", {0, 1, 3, 4, 6, 7, 9, 10}},
        {"Chromatic", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
        {"Hungarian minor", {0, 2, 3, 6, 7, 8, 11}},
        {"Hungarian major", {0, 3, 4, 6, 7, 9, 10}},
        {"Neapolitan major", {0, 1, 3, 5, 7, 9, 11}},
        {"Neapolitan minor", {0, 1, 3, 5, 7, 8, 11}},
        {"Arabic", {0, 2, 4, 5, 6, 8, 10}},
        {"Byzantine (double harmonic)", {0, 1, 4, 5, 7, 8, 11}},
        {"Persian", {0, 1, 4, 5, 6, 8, 11}},
        {"Spanish (Phrygian dominant)", {0, 1, 4, 5, 7, 8, 10}},
        {"Spanish 8-tone", {0, 1, 3, 4, 5, 6, 8, 10}},
        {"Gypsy", {0, 2, 3, 6, 7, 8, 10}},
        {"Hindu", {0, 2, 4, 5, 7, 8, 10}},
        {"Japanese (Hirajoshi)", {0, 2, 3, 7, 8}},
        {"Japanese (In sen)", {0, 1, 5, 7, 10}},
        {"Enigmatic", {0, 1, 4, 6, 8, 10, 11}},
    };
    return d;
}
QStringList PianoRollView::scaleNames() {
    QStringList l;
    for (auto& d : scaleDefs()) l << d.name;
    return l;
}
static const std::vector<std::vector<int>> kChords = {{}, {0, 4, 7}, {0, 3, 7}, {0, 4, 7, 11}, {0, 3, 7, 10}, {0, 4, 7, 10}, {0, 3, 6},
    {0, 4, 8}, {0, 5, 7}, {0, 2, 7}, {0, 7}, {0, 12}, {0, 4, 7, 12}, {0, 3, 7, 12}, {0, 4, 7, 11, 14}, {0, 3, 7, 10, 14}};
QStringList PianoRollView::chordNames() {
    return {"None", "Major", "Minor", "Maj7", "Min7", "Dom7", "Dim", "Aug", "Sus4", "Sus2", "Power (5th)", "Octave", "Major + oct",
        "Minor + oct", "Maj9", "Min9"};
}

PianoRollView::PianoRollView(App& app, QWidget* parent)
    : QWidget(parent)
    , m_app(app) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    m_velH = clamp(QSettings().value("pianoroll/laneHeight", 80).toInt(), 40, 400);
    m_ticker = new anim::Ticker(this);
    m_hbar = new QScrollBar(Qt::Horizontal, this);
    m_vbar = new QScrollBar(Qt::Vertical, this);
    m_hbar->setCursor(Qt::ArrowCursor);
    m_vbar->setCursor(Qt::ArrowCursor);   // tool cursors are for the canvas only
    connect(m_hbar, &QScrollBar::valueChanged, this, [this](int v) {
        if (!m_updatingBars) {
            m_scrollTick = (Tick)v * PPQ / 16;
            invalidate();
        }
    });
    connect(m_vbar, &QScrollBar::valueChanged, this, [this](int v) {
        if (!m_updatingBars) {
            m_topPitch = clamp(127 - v, minTopPitch(), 127);
            invalidate();
        }
    });
    connect(&m_repeatTimer, &QTimer::timeout, this, [this] {
        if (m_previewPitch >= 0) {
            m_app.noteOff(m_previewPitch);
            m_app.noteOn(m_previewPitch);
        }
    });
    connect(&m_autoScroll, &QTimer::timeout, this, [this] {
        if (m_drag != Moving && m_drag != Creating && m_drag != Resizing && m_drag != Rubber) {
            m_autoScroll.stop();
            return;
        }
        Layout L = layout();
        bool moved = false;
        if (m_lastMouse.y() < L.grid.top()) {
            m_topPitch = clamp(m_topPitch + 1, minTopPitch(), 127);
            moved = true;
        } else if (m_lastMouse.y() > L.grid.bottom()) {
            m_topPitch = clamp(m_topPitch - 1, minTopPitch(), 127);
            moved = true;
        }
        if (m_lastMouse.x() < L.grid.left()) {
            m_scrollTick = std::max<Tick>(0, m_scrollTick - snapTicks());
            moved = true;
        } else if (m_lastMouse.x() > L.grid.right()) {
            m_scrollTick += snapTicks();
            moved = true;
        }
        if (moved) {
            updateScrollbars();
            invalidate();
            QMouseEvent ev(
                QEvent::MouseMove, m_lastMouse, mapToGlobal(m_lastMouse), Qt::NoButton, Qt::LeftButton, QApplication::keyboardModifiers());
            mouseMoveEvent(&ev);
        }
    });
}
void PianoRollView::setTool(Tool t) {
    m_tool = t;
    setCursor(toolCursorFor(t));
    Q_EMIT modeChanged(t);
}
// The pointer carries the active tool's icon; the pencil points from its tip, the rest sit centred on the hotspot.
QCursor PianoRollView::toolCursorFor(Tool t) {
    switch (t) {
    case Erase: return theme::toolCursor(theme::Glyph::Eraser);
    case MuteTool: return theme::toolCursor(theme::Glyph::MuteSpeaker);
    case Select: return theme::toolCursor(theme::Glyph::Marquee);
    default: return QCursor(Qt::ArrowCursor);
    }
}   // draw tool: the regular pointer
PianoRollView::Layout PianoRollView::layout() const {
    Layout l;
    l.velH = m_velH;
    int w = width() - 14, h = height() - 14;
    l.keys = QRect(0, l.rulerH, l.keyW, h - l.rulerH - l.velH);
    l.ruler = QRect(l.keyW, 0, w - l.keyW, l.rulerH);
    l.grid = QRect(l.keyW, l.rulerH, w - l.keyW, h - l.rulerH - l.velH);
    l.vel = QRect(l.keyW, h - l.velH, w - l.keyW, l.velH);
    return l;
}
void PianoRollView::resizeEvent(QResizeEvent*) {
    m_hbar->setGeometry(layout().keyW, height() - 14, width() - layout().keyW - 14, 14);
    m_vbar->setGeometry(width() - 14, layout().rulerH, 14, height() - layout().rulerH - 14);
    updateScrollbars();
    invalidate();
}
Pattern* PianoRollView::pattern() const { return m_app.project().pattern(m_app.project().selectedPatternId); }
PatternChannelData* PianoRollView::data() const {
    Pattern* p = pattern();
    if (!p || !m_channel) return nullptr;
    return &p->ensureChannel(m_channel);
}
// Channels that only have step-sequencer steps are shown as "virtual" notes (one per step); they only become real
// notes (and the steps are cleared) when the user actually modifies something in the piano roll.
// Notes of another channel as ghosts: its piano-roll notes, or its step-sequencer steps converted the same way the view converts its own
std::vector<Note> PianoRollView::ghostNotesOf(const PatternChannelData& oc) const {
    if (!oc.notes.empty()) return oc.notes;
    std::vector<Note> v;
    Pattern* p = pattern();
    const Channel* ch = m_app.project().channel(oc.id);
    if (!p || !ch) return v;
    Tick st = p->stepTicks();
    for (size_t i = 0; i < oc.steps.size(); ++i) {
        const Step& sp = oc.steps[i];
        if (!sp.enabled) continue;
        Note n;
        n.pitch = clamp((int)std::lround(ch->rootNote + sp.pitch), 0, 127);
        n.start = (Tick)i * st + (Tick)(sp.offset * st);
        n.duration = st;
        n.velocity = sp.velocity;
        v.push_back(n);
    }
    return v;
}
std::vector<Note> PianoRollView::stepsAsNotes() const {
    std::vector<Note> v;
    Pattern* p = pattern();
    PatternChannelData* d = data();
    const Channel* ch = m_app.project().channel(m_channel);
    if (!p || !d || !ch) return v;
    Tick st = p->stepTicks();
    for (size_t i = 0; i < d->steps.size(); ++i) {
        const Step& sp = d->steps[i];
        if (!sp.enabled) continue;
        Note n;
        n.id = kVirtualBase + (Id)i;
        n.pitch = clamp((int)std::lround(ch->rootNote + sp.pitch), 0, 127);
        n.start = (Tick)i * st + (Tick)(sp.offset * st);
        n.duration = st;
        n.velocity = sp.velocity;
        n.pan = sp.pan;
        v.push_back(n);
    }
    return v;
}
bool PianoRollView::stepMode() const {
    PatternChannelData* d = data();
    if (!d || !d->notes.empty()) return false;
    for (auto& s : d->steps)
        if (s.enabled) return true;
    return false;
}
const std::vector<Note>& PianoRollView::notesView() const {
    PatternChannelData* d = data();
    static const std::vector<Note> none;
    if (!d) return none;
    if (!d->notes.empty()) return d->notes;
    m_virtual = stepsAsNotes();
    return m_virtual;
}
Tick PianoRollView::xToTick(int x) const { return m_scrollTick + (Tick)((x - layout().keyW) / m_ppt); }
int PianoRollView::tickToX(Tick t) const { return layout().keyW + (int)((t - m_scrollTick) * m_ppt); }
int PianoRollView::yToPitch(int y) const { return m_topPitch - (y - layout().rulerH) / m_rowH; }
int PianoRollView::pitchToY(int pitch) const { return layout().rulerH + (m_topPitch - pitch) * m_rowH; }
// lowest allowed top pitch: pitch 0 sits on the bottom row, so the grid never scrolls past the end of the keyboard
int PianoRollView::minTopPitch() const {
    int rows = (layout().grid.height() + m_rowH - 1) / m_rowH;
    return clamp(rows - 1, 0, 127);
}
Tick PianoRollView::snapTicks() const {
    if (m_snapDiv == 0) return 1;
    if (m_snapDiv < 0) return m_app.project().transport.timeSig.ticksPerBar() * -m_snapDiv;
    return PPQ / m_snapDiv;
}
Tick PianoRollView::snap(Tick t, bool force) const {
    if (m_altHeld && !force) return t;
    Tick s = snapTicks();
    if (s <= 1) return t;
    return (t / s) * s;
}
Tick PianoRollView::snapNear(Tick t) const {
    if (m_altHeld) return t;
    Tick s = snapTicks();
    if (s <= 1) return t;
    return ((t + s / 2) / s) * s;
}
bool PianoRollView::inScale(int pitch) const {
    if (m_scale <= 0 || m_scale >= (int)scaleDefs().size()) return true;
    int d = ((pitch - m_scaleRoot) % 12 + 12) % 12;
    for (int st : scaleDefs()[m_scale].steps)
        if (st == d) return true;
    return false;
}
void PianoRollView::updateScrollbars() {
    m_updatingBars = true;
    Pattern* p = pattern();
    Tick visible = (Tick)(layout().grid.width() / m_ppt);
    Tick len = std::max<Tick>((p ? p->effectiveLength(m_app.project().transport.timeSig) : PPQ * 16) + PPQ * 8, m_scrollTick + visible);
    int hmax = (int)std::max<Tick>(0, (len - visible) * 16 / PPQ);
    m_hbar->setRange(0, hmax);
    m_hbar->setPageStep((int)(visible * 16 / PPQ));
    if (m_scrollTick * 16 / PPQ > hmax) m_scrollTick = (Tick)hmax * PPQ / 16;
    m_hbar->setValue((int)(m_scrollTick * 16 / PPQ));
    int rows = (layout().grid.height() + m_rowH - 1) / m_rowH;
    m_topPitch = clamp(m_topPitch, minTopPitch(), 127);
    m_vbar->setRange(0, 127 - minTopPitch());
    m_vbar->setPageStep(rows);
    m_vbar->setValue(127 - m_topPitch);
    m_updatingBars = false;
}
void PianoRollView::refresh() {
    updateScrollbars();
    invalidate();
}
void PianoRollView::zoomToFit() {
    Pattern* p = pattern();
    if (!p) return;
    Tick len = std::max<Tick>(p->effectiveLength(m_app.project().transport.timeSig), PPQ);
    m_ppt = std::max(0.01, (double)layout().grid.width() / (len + PPQ));
    m_scrollTick = 0;
    int lo = 127, hi = 0;
    for (auto& n : notesView()) {
        lo = std::min(lo, n.pitch);
        hi = std::max(hi, n.pitch);
    }
    if (lo <= hi) m_topPitch = std::min(127, hi + 4);
    refresh();
}
void PianoRollView::uiTick() {
    // Playhead is shown whether or not the transport runs (so the start position is visible), mapped into this pattern
    auto tv = m_app.engine().transport();
    double pt = -1;
    Pattern* p = pattern();
    if (p && !tv.countingIn) {
        if (!tv.songMode) pt = tv.positionTicks;
        else {
            for (auto& c : m_app.project().clips)
                if (c.type == ClipType::Pattern && c.sourceId == p->id && tv.positionTicks >= c.start &&
                    tv.positionTicks < c.start + m_app.project().clipLength(c)) {
                    Tick len = std::max<Tick>(p->effectiveLength(m_app.project().transport.timeSig), 1);
                    double local = tv.positionTicks - c.start + c.sourceOffset;
                    pt = c.loopSource ? std::fmod(local, (double)len) : local;
                    break;
                }
        }
    }
    if (pt != m_playTick) {
        m_playTick = pt;
        update();
    }
}

// ---------------------------------------------------------------- painting
void PianoRollView::renderCache() {
    Layout L = layout();
    m_cache = QPixmap(size() * devicePixelRatioF());
    m_cache.setDevicePixelRatio(devicePixelRatioF());
    m_cache.fill(theme::bg());
    QPainter p(&m_cache);
    p.setRenderHint(
        QPainter::TextAntialiasing); /* the cache is rendered at the screen's real resolution: text stays crisp at 125 % / 150 % scaling */
    Pattern* pat = pattern();
    PatternChannelData* d = data();
    const auto& ts = m_app.project().transport.timeSig;
    Tick patLen = pat ? pat->effectiveLength(ts) : 0;
    // grid rows
    p.setClipRect(L.grid);
    for (int pitch = m_topPitch; pitch >= 0; --pitch) {
        int y = pitchToY(pitch);
        if (y > L.grid.bottom()) break;
        bool black = (pitch % 12 == 1 || pitch % 12 == 3 || pitch % 12 == 6 || pitch % 12 == 8 || pitch % 12 == 10);
        QColor c = black ? QColor(0x17, 0x1a, 0x1f) : QColor(0x1c, 0x1f, 0x25);
        if (m_scale > 0)
            c = inScale(pitch) ? QColor(0x1c, 0x1f, 0x25)
                               : QColor(0x17, 0x1a, 0x1f);   // scale: the same two colours, but light = in the scale, dark = outside it
        p.fillRect(QRect(L.grid.left(), y, L.grid.width(), m_rowH), c);
        if (pitch % 12 == 0) {
            p.setPen(theme::gridStrong());
            p.drawLine(L.grid.left(), y + m_rowH, L.grid.right(), y + m_rowH);
        }
    }
    // grid columns
    Tick beat = ts.ticksPerBeat(), bar = ts.ticksPerBar();
    Tick step = snapTicks();
    if (step * m_ppt < 8) step = beat;
    if (beat * m_ppt < 8) step = bar;
    Tick barStep = bar;
    while (barStep * m_ppt < 10) barStep *= 2;
    if (bar * m_ppt < 10) step = barStep;
    Tick first = (m_scrollTick / step) * step;
    Tick last = xToTick(L.grid.right());
    for (Tick t = first; t <= last; t += step) {
        int x = tickToX(t);
        QColor c = (t % bar == 0) ? theme::gridStrong().lighter(130) : (t % beat == 0) ? theme::gridStrong() : theme::grid();
        p.setPen(c);
        p.drawLine(x, L.grid.top(), x, L.grid.bottom());
    }
    if (patLen > 0) {
        int x = tickToX(patLen);
        p.fillRect(QRect(x, L.grid.top(), L.grid.right() - x, L.grid.height()), QColor(0, 0, 0, 60));
    }   // shading alone marks the pattern end - no accent rule
    if (m_timeSel) {
        int x1 = tickToX(m_selStart), x2 = tickToX(m_selEnd);
        p.fillRect(QRect(x1, L.grid.top(), x2 - x1, L.grid.height()), theme::withAlpha(theme::accent(), 22));
        p.setPen(QPen(theme::withAlpha(theme::accent(), 160), 1));
        p.drawLine(x1, L.grid.top(), x1, L.grid.bottom());
        p.drawLine(x2, L.grid.top(), x2, L.grid.bottom());
    }
    // ghost notes
    if (m_ghost && pat)
        for (auto& oc : pat->channels) {
            if (oc.id == m_channel) continue;
            const Channel* ch = m_app.project().channel(oc.id);
            QColor c = ch ? ch->color : theme::textDim();
            c.setAlpha(70);
            for (auto& n : ghostNotesOf(oc)) {
                QRect r(tickToX(n.start), pitchToY(n.pitch), std::max(2, (int)(n.duration * m_ppt)), m_rowH);
                if (r.right() < L.grid.left() || r.left() > L.grid.right()) continue;
                p.fillRect(r.adjusted(0, 1, -1, -1), c);
            }
        }
    // notes
    const Channel* ch = m_app.project().channel(m_channel);
    QColor base = ch ? ch->color : theme::accent();
    bool virt = stepMode();
    p.setClipping(false);
    // ruler
    p.fillRect(L.ruler, theme::panel());
    p.setClipRect(L.ruler);
    p.setFont(theme::smallFont());
    Tick rstep = beat * m_ppt >= 6 ? beat : barStep;
    Tick lstep = barStep;
    while (lstep * m_ppt < 34) lstep *= 2;
    for (Tick t = (m_scrollTick / rstep) * rstep; t <= last; t += rstep) {
        int x = tickToX(t);
        bool isBar = t % bar == 0;
        bool labeled = t % lstep == 0;
        p.setPen(labeled ? theme::text() : theme::textDim());
        p.drawLine(x, labeled ? L.ruler.top() + 6 : (isBar ? L.ruler.top() + 11 : L.ruler.top() + 15), x, L.ruler.bottom());
        if (labeled) p.drawText(x + 3, L.ruler.top() + 12, QString::number(t / bar + 1));
    }
    if (m_timeSel) {
        int x1 = tickToX(m_selStart), x2 = tickToX(m_selEnd);
        p.fillRect(QRect(x1, L.ruler.top(), x2 - x1, 5), theme::accent());
    }
    p.setClipping(false);
    // keys
    p.fillRect(L.keys, theme::panel());
    p.setClipRect(L.keys);
    for (int pitch = m_topPitch; pitch >= 0; --pitch) {
        int y = pitchToY(pitch);
        if (y > L.keys.bottom()) break;
        bool black = (pitch % 12 == 1 || pitch % 12 == 3 || pitch % 12 == 6 || pitch % 12 == 8 || pitch % 12 == 10);
        QRect kr(0, y, black ? L.keyW * 0.6 : L.keyW - 1, m_rowH);
        p.fillRect(QRect(0, y, L.keyW - 1, m_rowH), black ? theme::panel().darker(120) : QColor(0xc9, 0xcd, 0xd3));
        if (black) p.fillRect(kr, theme::panel().darker(120));
        else p.fillRect(kr, QColor(0xc9, 0xcd, 0xd3));
        if (m_previewPitch == pitch) p.fillRect(kr, theme::accent());
        p.setPen(theme::border());
        p.drawLine(0, y + m_rowH, L.keyW, y + m_rowH);
        if ((m_keyLabels == LabelsAll || (m_keyLabels == LabelsC && pitch % 12 == 0)) && m_rowH >= 9) {
            p.setPen(black ? QColor(0x9a, 0x9f, 0xa6) : QColor(0x40, 0x44, 0x4a));
            p.setFont(theme::smallFont());
            p.drawText(QRect(0, y, L.keyW - 4, m_rowH), Qt::AlignRight | Qt::AlignVCenter, theme::noteName(pitch));
        }
    }
    p.setClipping(false);
    // velocity lane
    p.fillRect(L.vel, theme::surface());
    p.setPen(theme::border());
    p.drawLine(L.vel.left(), L.vel.top(), L.vel.right(), L.vel.top());
    p.setClipRect(L.vel);
    {
        int bw = laneBarWidth();
        bool bip = laneBipolar();
        int midY = L.vel.top() + L.vel.height() / 2;
        if (bip) {
            p.setPen(theme::gridStrong());
            p.drawLine(L.vel.left(), midY, L.vel.right(), midY);
        }
        if (d)
            for (auto& n : notesView()) {
                int x = tickToX(n.start);
                if (x < L.vel.left() - bw || x > L.vel.right()) continue;
                float v = laneValue(n);
                bool sel = m_selected.count(n.id);
                p.setPen(sel ? Qt::white : base);
                p.setBrush(sel ? base.lighter(150) : base);
                if (bip) {
                    int h = (int)((v - 0.5f) * (L.vel.height() - 8));
                    p.drawRect(QRect(x, h >= 0 ? midY - h : midY, bw, std::abs(h)));
                    p.drawRect(QRect(x, midY - 1, bw, 2));
                } else {
                    int h = (int)(v * (L.vel.height() - 6));
                    p.drawRect(QRect(x, L.vel.bottom() - h, bw, h));
                }
            }
        {
            QFont f = theme::smallFont();
            p.setFont(f);
            QString t = laneName() + "  \u25BE";
            int tw = p.fontMetrics().horizontalAdvance(t) + 12;
            m_laneLabelRect = QRect(L.vel.left() + 4, L.vel.top() + 3, tw, 16);
            p.setPen(Qt::NoPen);
            p.setBrush(theme::raised());
            p.drawRoundedRect(m_laneLabelRect, 3, 3);
            p.setPen(theme::text());
            p.drawText(m_laneLabelRect, Qt::AlignCenter, t);
        }
        p.setPen(theme::border());
        p.drawLine(L.vel.left(), L.vel.top(), L.vel.right(), L.vel.top());
    }
    p.setClipping(false);
    if (virt) {
        p.setPen(theme::textDim());
        p.setFont(theme::smallFont());
        p.drawText(QRect(L.grid.left() + 6, L.grid.top() + 3, 500, 14), Qt::AlignLeft,
            "Showing the channel's step-sequencer steps - editing anything here turns them into piano roll notes");
    }
    m_cacheValid = true;
}
void PianoRollView::paintEvent(QPaintEvent*) {
    if (m_themeRev != theme::revision()) {
        m_themeRev = theme::revision();
        m_cacheValid = false;
    }   // the cached pixmap still holds the old theme
    if (!m_cacheValid) renderCache();
    QPainter p(this);
    p.drawPixmap(0, 0, m_cache);
    Layout L = layout();
    {   // notes: animated (appear / move / vanish), instant while being dragged
        p.save();
        p.setClipRect(L.grid);
        p.setRenderHint(QPainter::Antialiasing);
        const Channel* ch = m_app.project().channel(m_channel);
        QColor base = ch ? ch->color : theme::accent();
        bool virt = stepMode();
        bool dragging = m_drag == Moving || m_drag == Creating || m_drag == Resizing || m_drag == Velocity;
        m_noteAnim.begin();
        // tracked in musical space (tick / pitch) so scrolling, zooming or resizing the view never "moves" notes
        auto toPx = [&](const QRectF& m) {
            double x = tickToX(0) + m.x() * m_ppt;
            return QRectF(
                x, pitchToY(0) - (m.y() + m.height() - 1) * m_rowH + 1, std::max(3.0, m.width() * m_ppt) - 1, m.height() * m_rowH - 1);
        };
        for (auto& n : notesView()) {
            QRectF target(n.start, n.pitch, n.duration, 1);
            double alpha = 1;
            bool instant = dragging && m_selected.count(n.id);
            QRectF r = toPx(m_noteAnim.item(n.id, target, instant, &alpha));
            if (r.right() < L.grid.left() || r.left() > L.grid.right()) continue;
            bool sel = m_selected.count(n.id);
            QColor c = base.lighter((int)(55 + n.velocity * 65));
            if (n.muted) c = c.darker(200);
            QColor cc = c;
            cc.setAlphaF(alpha * (virt ? 0.67 : 1.0));
            QColor pen = sel ? QColor(Qt::white) : base.darker(160);
            pen.setAlphaF(alpha);
            p.setPen(QPen(pen, sel ? 1.2 : 1, virt ? Qt::DashLine : Qt::SolidLine));
            p.setBrush(cc);
            { double rr = theme::radiusFor(r.height()); p.drawRoundedRect(r, rr, rr); }
            if (r.width() > 28 && m_rowH >= 10 && alpha >= 0.99) {
                p.setPen(theme::bg());
                p.setFont(theme::smallFont());
                p.drawText(r.adjusted(3, 0, 0, 0).toRect(), Qt::AlignVCenter | Qt::AlignLeft, theme::noteName(n.pitch));
            }
        }
        m_noteAnim.end([&](Id) { return base; });
        m_noteAnim.drawGhosts([&](const QRectF& m, const QColor& c, double a) {
            QRectF r = toPx(m);
            QColor cc = c.lighter(110);
            cc.setAlphaF(a * 0.8);
            p.setPen(Qt::NoPen);
            p.setBrush(cc);
            { double rr = theme::radiusFor(r.height()); p.drawRoundedRect(r, rr, rr); }
        });
        p.restore();
        m_ticker->keepAlive(m_noteAnim.active());
    }
    if (m_playTick >= 0) {
        int x = tickToX((Tick)m_playTick);
        if (x >= L.grid.left() && x <= L.grid.right()) {
            p.setPen(QPen(theme::playhead(), 1.5));
            p.drawLine(x, L.ruler.top(), x, L.vel.bottom());
        }
    }
    if (m_drag == Rubber) {
        p.save();
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(QPen(theme::withAlpha(theme::accent(), 220), 1));
        p.setBrush(theme::selection());
        p.drawRect(m_rubber.normalized());
        p.restore();
    }
    if (m_tool == Draw && m_drag == NoDrag && L.grid.contains(m_hoverPos) && underMouse()) {
        int pitch = yToPitch(m_hoverPos.y());
        Tick t = snap(xToTick(m_hoverPos.x()));
        QRect r(tickToX(t), pitchToY(pitch), std::max(3, (int)(m_lastLength * m_ppt)), m_rowH);
        p.save();
        p.setClipRect(L.grid);
        p.setPen(QPen(theme::withAlpha(theme::accent(), 200), 1));
        p.setBrush(theme::withAlpha(theme::accent(), 40));
        p.drawRect(r.adjusted(0, 1, -1, -1));
        p.restore();
    }
    p.setPen(theme::border());
    p.setBrush(Qt::NoBrush);
    p.drawRect(rect().adjusted(0, 0, -1, -1));
}

// ---------------------------------------------------------------- editing helpers
Note* PianoRollView::noteAt(const QPoint& pt, bool* nearEdge) const {
    PatternChannelData* d = data();
    if (!d) return nullptr;
    Note* found = nullptr;
    auto& notes = const_cast<std::vector<Note>&>(notesView());
    for (auto it = notes.rbegin(); it != notes.rend(); ++it) {
        QRect r(tickToX(it->start), pitchToY(it->pitch), std::max(3, (int)(it->duration * m_ppt)), m_rowH);
        if (r.contains(pt)) {
            found = &*it;
            if (nearEdge) *nearEdge = (r.right() - pt.x()) <= 6 && r.width() > 10;
            break;
        }
    }
    return found;
}
// Move the playhead to a position inside this pattern. In song mode the position is mapped into the playlist clip
// that plays this pattern (the one under the playhead if there is one, otherwise the first one).
void PianoRollView::seekToPatternTick(Tick t) {
    Pattern* p = pattern();
    if (!p) return;
    const auto& proj = m_app.project();
    if (!proj.transport.songMode) {
        m_app.seek(t);
        return;
    }
    const Clip* best = nullptr;
    double pos = m_app.engine().positionTicks();
    for (auto& c : proj.clips) {
        if (c.type != ClipType::Pattern || c.sourceId != p->id) continue;
        Tick len = proj.clipLength(c);
        if (pos >= c.start && pos < c.start + len) {
            best = &c;
            break;
        }
        if (!best || c.start < best->start) best = &c;
    }
    if (!best) {
        m_app.setSongMode(false);
        m_app.seek(t);
        m_app.statusMessage("Pattern is not in the playlist - switched to pattern mode", 3000);
        return;
    }
    Tick len = std::max<Tick>(p->effectiveLength(proj.transport.timeSig), 1);
    Tick local = std::max<Tick>(0, t - best->sourceOffset);
    if (best->loopSource) local %= len;
    m_app.seek(best->start + local);
}
// The bottom lane edits one note property (velocity, pan, release velocity or fine pitch); values are 0..1 for the lane
float PianoRollView::laneValue(const Note& n) const {
    switch (m_lane) {
    case 1: return (n.pan + 1.f) * 0.5f;
    case 2: return n.releaseVelocity;
    case 3: return (n.finePitch + 100.f) / 200.f;
    default: return n.velocity;
    }
}
void PianoRollView::setLaneValue(Note& n, float v) const {
    v = clamp(v, 0.f, 1.f);
    switch (m_lane) {
    case 1: n.pan = v * 2.f - 1.f; break;
    case 2: n.releaseVelocity = v; break;
    case 3: n.finePitch = v * 200.f - 100.f; break;
    default: n.velocity = v; break;
    }
}
bool PianoRollView::laneBipolar() const { return m_lane == 1 || m_lane == 3; }
QString PianoRollView::laneName() const {
    static const char* names[] = {"Velocity", "Pan", "Release", "Fine pitch"};
    return names[clamp(m_lane, 0, 3)];
}
void PianoRollView::beginEdit() {
    PatternChannelData* d = data();
    if (!d) return;
    m_before = *d;
    m_editing = true;
    if (stepMode()) {
        d->notes = stepsAsNotes();
        for (auto& s : d->steps) s.enabled = false;
    }
    m_baseNotes = d->notes; /* drag reference: includes steps just turned into notes */
}
void PianoRollView::commitEdit(const QString& desc) {
    PatternChannelData* d = data();
    if (!d || !m_editing) return;
    m_editing = false;
    PatternChannelData after = *d;
    *d = m_before;
    Id ch = m_channel;
    Id pid = pattern()->id;
    // only push if something changed
    bool same = after.notes.size() == m_before.notes.size() && after.steps.size() == m_before.steps.size();
    if (same)
        for (size_t i = 0; i < after.steps.size(); ++i)
            if (after.steps[i].enabled != m_before.steps[i].enabled) {
                same = false;
                break;
            }
    if (same)
        for (size_t i = 0; i < after.notes.size(); ++i) {
            auto& a = after.notes[i];
            auto& b = m_before.notes[i];
            if (a.id != b.id || a.pitch != b.pitch || a.start != b.start || a.duration != b.duration || a.velocity != b.velocity ||
                a.pan != b.pan || a.muted != b.muted || a.finePitch != b.finePitch) {
                same = false;
                break;
            }
        }
    if (!same && m_before.notes.empty()) {   // before = steps only: unchanged if 'after' is exactly the step conversion
        *d = m_before;
        std::vector<Note> conv = stepsAsNotes();
        bool eq = conv.size() == after.notes.size() &&
            std::all_of(after.steps.begin(), after.steps.end(), [](const Step& s) { return !s.enabled; });
        if (eq)
            for (size_t i = 0; i < conv.size(); ++i) {
                auto& a = after.notes[i];
                auto& b = conv[i];
                if (a.id != b.id || a.pitch != b.pitch || a.start != b.start || a.duration != b.duration || a.velocity != b.velocity ||
                    a.pan != b.pan) {
                    eq = false;
                    break;
                }
            }
        if (eq) same = true;
    }
    if (same) {
        invalidate();
        return;
    }
    // real notes get proper project ids
    // real notes get proper project ids; the selection follows them so transpose / move / quantize keep the notes selected
    std::set<Id> keep;
    for (auto& n : after.notes) {
        bool sel = m_selected.count(n.id);
        if (n.id >= kVirtualBase) n.id = m_app.project().newId();
        if (sel) keep.insert(n.id);
    }
    m_selected = keep;
    // Notes that are all plain step-shaped (one root-note hit per step, one step long) go back to being steps, so the
    // channel rack keeps showing them as step buttons instead of a piano-roll preview
    if (Pattern* pat = pattern(); pat && !after.notes.empty()) {
        const Channel* chn = m_app.project().channel(ch);
        Tick st = pat->stepTicks();
        bool stepShaped = chn != nullptr;
        std::set<int> used;
        for (auto& n : after.notes) {
            int i = (int)(n.start / st);
            if (!stepShaped || n.start % st != 0 || n.duration != st || n.pitch != chn->rootNote || n.muted || n.finePitch != 0.f ||
                n.channel != 0 || used.count(i)) {
                stepShaped = false;
                break;
            }
            used.insert(i);
        }
        if (stepShaped) {
            std::set<Id> ksel;
            std::vector<Step> steps = after.steps;
            for (auto& s : steps) s.enabled = false;
            for (auto& n : after.notes) {
                int i = (int)(n.start / st);
                if ((int)steps.size() <= i) steps.resize(i + 1);
                Step& sp = steps[i];
                sp = Step();
                sp.enabled = true;
                sp.velocity = n.velocity;
                sp.pan = n.pan;
                if (i < (int)m_before.steps.size() && m_before.steps[i].enabled) sp.probability = m_before.steps[i].probability;
                if (keep.count(n.id)) ksel.insert(kVirtualBase + (Id)i);
            }
            after.notes.clear();
            after.steps = std::move(steps);
            m_selected = ksel;
            TimeSignature ts = m_app.project().transport.timeSig;
            m_app.editPattern(pid, desc, [after, ch, ts](Pattern& p) {
                p.ensureChannel(ch) = after;
                int spb = std::max(1, (int)(ts.ticksPerBar() / p.stepTicks()));
                int last = -1;
                for (auto& c : p.channels)
                    for (size_t i = 0; i < c.steps.size(); ++i)
                        if (c.steps[i].enabled) last = std::max(last, (int)i);
                int need = std::max((last / spb + 1) * spb, spb);
                if (need > p.stepCount) {
                    p.stepCount = need;
                    for (auto& c : p.channels)
                        if ((int)c.steps.size() < need) c.steps.resize(need);
                }
            });
            return;
        }
    }
    m_app.editPattern(pid, desc, [after, ch](Pattern& p) { p.ensureChannel(ch) = after; });
}
void PianoRollView::cancelEdit() {
    PatternChannelData* d = data();
    if (d && m_editing) *d = m_before;
    m_editing = false;
    invalidate();
}
// Note preview policy (Options > Note preview): 0 = when placing / re-pitching while the transport is stopped (default), 1 = always, 2 = never
bool PianoRollView::previewAllowed() const {
    int mode = QSettings().value("pianoroll/preview", 0).toInt();
    if (mode == 2) return false;
    if (mode == 1) return true;
    return !m_app.engine().isPlaying();
}
void PianoRollView::previewNote(int pitch, bool on) {
    if (on) {
        m_app.selectChannel(m_channel);
        m_app.noteOn(pitch, m_lastVelocity);
    } else m_app.noteOff(pitch);
}

// ---------------------------------------------------------------- mouse
void PianoRollView::mousePressEvent(QMouseEvent* e) {
    setFocus();
    Layout L = layout();
    m_pressPos = m_lastPos = e->pos();
    m_altHeld = e->modifiers() & Qt::AltModifier;
    if (e->button() == Qt::MiddleButton) {
        m_drag = Pan;
        return;
    }
    if (L.ruler.contains(e->pos())) {
        Tick t = std::max<Tick>(0, snapNear(xToTick(e->pos().x())));
        if (e->modifiers() & Qt::ControlModifier) {
            m_drag = TimeSelect;
            m_selAnchor = t;
            m_selStart = m_selEnd = t;
            m_timeSel = false;
            invalidate();
            return;
        }
        if (m_timeSel) {
            m_timeSel = false;
            invalidate();
        }
        m_drag = Seek;
        m_pressTick = t;
        seekToPatternTick(t);
        return;
    }
    if (L.keys.contains(e->pos())) {
        int pitch = yToPitch(e->pos().y());
        m_previewPitch = pitch;
        previewNote(pitch, true);
        if (m_noteRepeat) m_repeatTimer.start((int)(ticksToSeconds(snapTicks(), m_app.project().transport.bpm) * 1000));
        m_drag = KeyPreview;
        invalidate();
        return;
    }
    if (std::abs(e->pos().y() - L.vel.top()) <= 4 && e->pos().x() >= L.vel.left()) {
        m_drag = LaneResize;
        m_laneResizeStart = m_velH;
        return;
    }
    if (L.vel.contains(e->pos())) {
        if (m_laneLabelRect.contains(e->pos())) {
            QMenu m(this);
            const char* names[] = {"Velocity", "Pan", "Release velocity", "Fine pitch"};
            for (int i = 0; i < 4; ++i) {
                auto* a = m.addAction(names[i], [this, i] { setLane(i); });
                a->setCheckable(true);
                a->setChecked(i == m_lane);
            }
            m.exec(mapToGlobal(QPoint(m_laneLabelRect.left(), m_laneLabelRect.bottom() + 2)));
            return;
        }
        beginEdit();
        m_drag = Velocity;
        mouseMoveEvent(e);
        return;
    }
    if (!L.grid.contains(e->pos())) return;
    PatternChannelData* d = data();
    if (!d) return;
    bool edge = false;
    Note* n = noteAt(e->pos(), &edge);
    if (m_tool == MuteTool && e->button() == Qt::LeftButton) {
        beginEdit();
        m_drag = Muting;
        m_touched.clear();
        if (n) {
            n->muted = !n->muted;
            m_touched.insert(n->id);
            invalidate();
        }
        return;
    }
    if (e->button() == Qt::RightButton || m_tool == Erase) {
        beginEdit();
        m_drag = Erasing;
        m_erasedAny = false;
        setCursor(theme::toolCursor(theme::Glyph::Eraser));
        Q_EMIT modeChanged(Erase);
        if (n) {
            std::erase_if(d->notes, [id = n->id](const Note& x) { return x.id == id; });
            m_selected.erase(n->id);
            m_erasedAny = true;
            invalidate();
        }
        return;
    }
    if (n) {
        if (n->id < kVirtualBase && e->button() == Qt::LeftButton)
            m_lastLength = n->duration;   // FL: clicking a note makes its length the length of the notes you draw next
        if (e->modifiers() & Qt::ControlModifier && !(e->modifiers() & Qt::ShiftModifier)) {
            m_toggleOnRelease = n->id;
            m_selBase = m_selected;
            m_drag = Rubber;
            m_rubber = QRect(e->pos(), QSize(0, 0));
            invalidate();
            return;
        }   // Ctrl: drag = rubber-band selection from here, click = toggle this note
        if (!m_selected.count(n->id)) {
            m_selected.clear();
            m_selected.insert(n->id);
        }
        beginEdit();
        m_dragNote = n->id;
        m_pressTick = xToTick(e->pos().x());
        m_pressPitch = yToPitch(e->pos().y());
        m_duplicated = false;
        if (e->modifiers() & Qt::ShiftModifier && e->modifiers() & Qt::ControlModifier) {   // ctrl+shift drag = duplicate
            std::vector<Note> copies;
            for (auto& x : d->notes)
                if (m_selected.count(x.id)) {
                    Note c = x;
                    c.id = m_app.project().newId();
                    copies.push_back(c);
                }
            m_selected.clear();
            for (auto& c : copies) {
                m_selected.insert(c.id);
                d->notes.push_back(c);
            }
            m_duplicated = true;
        }
        m_drag = edge ? Resizing : Moving;
        m_previewPitch = -1;
        invalidate();
        return;
    }
    if (m_tool == Select || (e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))) {
        if (!(e->modifiers() & Qt::ShiftModifier)) m_selected.clear();
        m_selBase = m_selected;
        m_drag = Rubber;
        m_rubber = QRect(e->pos(), QSize(0, 0));
        invalidate();
        return;
    }
    // draw: create note (+ chord)
    beginEdit();
    int pitch = clamp(yToPitch(e->pos().y()), 0, 127);
    Tick t = std::max<Tick>(0, snap(xToTick(e->pos().x())));
    m_selected.clear();
    std::vector<int> intervals = m_chord > 0 && m_chord < (int)kChords.size() ? kChords[m_chord] : std::vector<int>{0};
    m_created.clear();
    for (int iv : intervals) {
        Note nn;
        nn.id = m_app.project().newId();
        nn.pitch = clamp(pitch + iv, 0, 127);
        nn.start = t;
        nn.duration = m_lastLength;
        nn.velocity = m_lastVelocity;
        d->notes.push_back(nn);
        m_selected.insert(nn.id);
        m_created[nn.id] = nn;
        if (iv == 0) m_dragNote = nn.id;
    }
    if (previewAllowed()) {
        previewNote(pitch, true);
        m_previewPitch = pitch;
    }
    m_pressTick = xToTick(e->pos().x());
    m_pressPitch = pitch;
    m_drag = Creating;
    invalidate();
}
void PianoRollView::mouseMoveEvent(QMouseEvent* e) {
    m_hoverPos = e->pos();
    m_lastMouse = e->pos();
    m_altHeld = e->modifiers() & Qt::AltModifier;
    Layout L = layout();
    PatternChannelData* d = data();
    if ((m_drag == Moving || m_drag == Creating || m_drag == Resizing || m_drag == Rubber) && !L.grid.contains(e->pos()) &&
        !m_autoScroll.isActive())
        m_autoScroll.start(40);
    if (m_drag == NoDrag && std::abs(e->pos().y() - L.vel.top()) <= 4 && e->pos().x() >= L.vel.left()) {
        setCursor(Qt::SizeVerCursor);
        return;
    }
    if (m_drag == NoDrag) {
        bool edge = false;
        Note* n = L.grid.contains(e->pos()) ? noteAt(e->pos(), &edge) : nullptr;
        if (m_tool == Draw || m_tool == Select)
            setCursor(n && edge ? QCursor(Qt::SizeHorCursor) : (n ? QCursor(Qt::SizeAllCursor) : toolCursorFor(m_tool)));
        if (m_tool == Draw) update();
        return;
    }
    if (m_drag == Pan) {
        int dx = e->pos().x() - m_lastPos.x(), dy = e->pos().y() - m_lastPos.y();
        m_scrollTick = std::max<Tick>(0, m_scrollTick - (Tick)(dx / m_ppt));
        m_topPitch = clamp(m_topPitch + dy / m_rowH, minTopPitch(), 127);
        m_lastPos = e->pos();
        updateScrollbars();
        invalidate();
        return;
    }
    if (m_drag == LaneResize) {
        int h = clamp(m_laneResizeStart - (e->pos().y() - m_pressPos.y()), 40, 400);
        if (h != m_velH) {
            m_velH = h;
            updateScrollbars();
            invalidate();
        }
        return;
    }
    if (m_drag == Seek) {
        Tick t = std::max<Tick>(0, snapNear(xToTick(e->pos().x())));
        if (t != m_pressTick) {
            m_pressTick = t;
            seekToPatternTick(t);
        }
        return;
    }
    if (m_drag == TimeSelect) {
        Tick t = std::max<Tick>(0, snapNear(xToTick(e->pos().x())));
        m_selStart = std::min(m_selAnchor, t);
        m_selEnd = std::max(m_selAnchor, t);
        m_timeSel = m_selEnd > m_selStart;
        invalidate();
        return;
    }
    if (m_drag == KeyPreview) {
        int pitch = yToPitch(e->pos().y());
        if (pitch != m_previewPitch && L.keys.contains(e->pos())) {
            previewNote(m_previewPitch, false);
            m_previewPitch = pitch;
            previewNote(pitch, true);
            invalidate();
        }
        return;
    }
    if (!d) return;
    if (m_drag == Velocity) {
        float v = clamp((float)(L.vel.bottom() - e->pos().y()) / (L.vel.height() - 6), 0.f, 1.f);
        int hw = laneBarWidth();
        bool any = !m_selected.empty();
        for (auto& n : d->notes) {
            int x = tickToX(n.start);
            if (any ? m_selected.count(n.id) : (e->pos().x() >= x - 1 && e->pos().x() <= x + hw + 1)) setLaneValue(n, v);
        }
        if (m_lane == 0) m_lastVelocity = v;
        invalidate();
        return;
    }
    if (m_drag == Erasing) {
        if (Note* n = noteAt(e->pos())) {
            std::erase_if(d->notes, [id = n->id](const Note& x) { return x.id == id; });
            m_erasedAny = true;
            invalidate();
        }
        return;
    }
    if (m_drag == Muting) {
        if (Note* n = noteAt(e->pos())) {
            if (!m_touched.count(n->id)) {
                n->muted = !n->muted;
                m_touched.insert(n->id);
                invalidate();
            }
        }
        return;
    }
    if (m_drag == Rubber) {
        m_rubber = QRect(m_pressPos, e->pos());
        QRect r = m_rubber.normalized();
        if (m_toggleOnRelease && (r.width() >= 3 || r.height() >= 3)) {
            m_toggleOnRelease = 0;
            if (!(e->modifiers() & Qt::ShiftModifier)) m_selBase.clear();
        }
        m_selected = m_selBase;
        for (auto& n : notesView()) {
            QRect nr(tickToX(n.start), pitchToY(n.pitch), std::max(3, (int)(n.duration * m_ppt)), m_rowH);
            if (r.intersects(nr)) m_selected.insert(n.id);
        }
        invalidate();
        return;
    }
    if (m_drag == Moving || m_drag == Creating) {
        Tick dt = xToTick(e->pos().x()) - m_pressTick;
        int dp = clamp(yToPitch(e->pos().y()), 0, 127) - m_pressPitch;
        // snap the dragged note's new start, apply the same delta to all selected
        const Note* anchorBefore = nullptr;
        for (auto& n : m_baseNotes)
            if (n.id == m_dragNote) anchorBefore = &n;
        Note anchorNew;
        if (!anchorBefore) {
            for (auto& n : d->notes)
                if (n.id == m_dragNote) anchorNew = n;
        }
        Tick anchorStart = anchorBefore ? anchorBefore->start : anchorNew.start - 0;
        if (!anchorBefore) { anchorStart = snap(xToTick(m_pressPos.x())); }
        Tick target = snapNear(anchorStart + dt);
        Tick delta = target - anchorStart;
        int minPitch = 127, maxPitch = 0;
        for (auto& n : m_baseNotes)
            if (m_selected.count(n.id)) {
                minPitch = std::min(minPitch, n.pitch);
                maxPitch = std::max(maxPitch, n.pitch);
            }
        if (m_duplicated || m_drag == Creating) {
            for (auto& n : d->notes)
                if (m_selected.count(n.id)) {
                    minPitch = std::min(minPitch, n.pitch);
                    maxPitch = std::max(maxPitch, n.pitch);
                }
        }
        dp = clamp(dp, -minPitch, 127 - maxPitch);
        Tick minStart = LLONG_MAX;
        for (auto& n : m_baseNotes)
            if (m_selected.count(n.id)) minStart = std::min(minStart, n.start);
        if (minStart != LLONG_MAX && minStart + delta < 0) delta = -minStart;
        for (auto& n : d->notes) {
            if (!m_selected.count(n.id)) continue;
            const Note* b = nullptr;
            for (auto& x : m_baseNotes)
                if (x.id == n.id) b = &x;
            if (b) {
                n.start = b->start + delta;
                n.pitch = b->pitch + dp;
            } else if (m_drag == Creating) {
                auto it = m_created.find(n.id);
                Tick base = it != m_created.end() ? it->second.start : snap(xToTick(m_pressPos.x()));
                int bp = it != m_created.end() ? it->second.pitch : n.pitch;
                n.start = std::max<Tick>(0, base + delta);
                n.pitch = clamp(bp + dp, 0, 127);
            } else { /* duplicated notes: relative to press */
                n.start = std::max<Tick>(0, n.start);
            }
        }
        {
            int np = -1;
            for (auto& n : d->notes)
                if (n.id == m_dragNote) np = n.pitch;
            if (np >= 0 && np != m_previewPitch && previewAllowed()) {
                if (m_previewPitch >= 0) previewNote(m_previewPitch, false);
                m_previewPitch = np;
                previewNote(np, true);
            }
        }
        if (m_drag == Creating) {
            for (auto& n : d->notes)
                if (n.id == m_dragNote) {
                    Tick len = snap(xToTick(e->pos().x()) + snapTicks(), true) - n.start;
                    if (e->pos().x() > m_pressPos.x() + 8 && len > 0) {
                        for (auto& m : d->notes)
                            if (m_selected.count(m.id)) m.duration = std::max<Tick>(len, snapTicks() > 1 ? snapTicks() : PPQ / 32);
                        m_lastLength = len;
                    }
                }
        }
        invalidate();
        return;
    }
    if (m_drag == Resizing) {
        Tick end = snapNear(xToTick(e->pos().x()));
        const Note* anchor = nullptr;
        for (auto& n : m_baseNotes)
            if (n.id == m_dragNote) anchor = &n;
        if (!anchor) return;
        Tick newLen = std::max<Tick>(end - anchor->start, m_altHeld ? PPQ / 64 : snapTicks());
        Tick dl = newLen - anchor->duration;
        for (auto& n : d->notes)
            if (m_selected.count(n.id)) {
                const Note* b = nullptr;
                for (auto& x : m_baseNotes)
                    if (x.id == n.id) b = &x;
                if (b) n.duration = std::max<Tick>(PPQ / 64, b->duration + dl);
            }
        m_lastLength = newLen;
        invalidate();
        return;
    }
}
void PianoRollView::mouseReleaseEvent(QMouseEvent* e) {
    if (m_drag == KeyPreview || m_drag == Moving || m_drag == Creating) {
        if (m_previewPitch >= 0) previewNote(m_previewPitch, false);
        m_previewPitch = -1;
        m_repeatTimer.stop();
    }
    if (m_drag == Erasing) {
        m_lastEraseWasClick = m_erasedAny && (e->pos() - m_pressPos).manhattanLength() < 4;
        m_lastEraseTime = QDateTime::currentMSecsSinceEpoch();
    }
    if (m_drag == Erasing || m_drag == Muting) setTool(m_tool);   // restore the tool cursor and toolbar state
    switch (m_drag) {
    case Moving: commitEdit(m_duplicated ? "Duplicate notes" : "Move notes"); break;
    case Creating: commitEdit("Draw note"); break;
    case Resizing: commitEdit("Resize notes"); break;
    case Velocity: commitEdit("Edit velocity"); break;
    case Erasing: commitEdit("Erase notes"); break;
    case Muting: commitEdit("Mute notes"); break;
    default: break;
    }
    if (m_drag == Rubber) {
        bool tiny = m_rubber.normalized().width() < 3 && m_rubber.normalized().height() < 3;
        if (tiny && m_toggleOnRelease) {
            m_selected = m_selBase;
            if (m_selected.count(m_toggleOnRelease)) m_selected.erase(m_toggleOnRelease);
            else m_selected.insert(m_toggleOnRelease);
        } else if (tiny && !(e->modifiers() & Qt::ShiftModifier)) m_selected.clear();
        m_toggleOnRelease = 0;
    }
    m_drag = NoDrag;
    m_dragNote = 0;
    invalidate();
}
void PianoRollView::mouseDoubleClickEvent(QMouseEvent* e) {
    Layout L = layout();
    if (e->button() == Qt::RightButton && L.grid.contains(e->pos())) {   // double right-click: mute mode while dragging
        if (m_lastEraseWasClick && QDateTime::currentMSecsSinceEpoch() - m_lastEraseTime < 600) {
            m_app.model().undoStack().undo();
            m_lastEraseWasClick = false;
        }
        beginEdit();
        m_drag = Muting;
        m_touched.clear();
        setCursor(theme::muteCursor());
        Q_EMIT modeChanged(MuteTool);
        if (Note* n = noteAt(e->pos())) {
            n->muted = !n->muted;
            m_touched.insert(n->id);
            invalidate();
        }
        return;
    }
    if (L.grid.contains(e->pos())) {
        if (!noteAt(e->pos()) && m_ghost) {   // double-click on a ghost note: jump to that channel's piano roll
            if (Pattern* pat = pattern())
                for (auto& oc : pat->channels) {
                    if (oc.id == m_channel) continue;
                    for (auto& n : ghostNotesOf(oc)) {
                        QRect r(tickToX(n.start), pitchToY(n.pitch), std::max(2, (int)(n.duration * m_ppt)), m_rowH);
                        if (r.contains(e->pos())) {
                            Q_EMIT channelRequested(oc.id);
                            return;
                        }
                    }
                }
        }
        if (Note* n = noteAt(e->pos())) {
            bool ok;
            int v = QInputDialog::getInt(this, "Note", "Velocity (0-127):", (int)(n->velocity * 127), 1, 127, 1, &ok);
            if (ok) {
                beginEdit();
                n->velocity = v / 127.f;
                commitEdit("Note velocity");
            }
        }
    } else if (L.ruler.contains(e->pos())) zoomToFit();
}
void PianoRollView::wheelEvent(QWheelEvent* e) {
    int dy = e->angleDelta().y();
    Layout L = layout();
    if (m_laneLabelRect.contains(e->position().toPoint()) ||
        (L.vel.contains(e->position().toPoint()) && e->position().toPoint().y() < L.vel.top() + 20)) {
        setLane(((m_lane + (dy > 0 ? 3 : 1)) % 4));
        e->accept();
        return;
    }
    if (e->modifiers() & Qt::AltModifier) {
        int d = e->angleDelta().y() != 0 ? e->angleDelta().y() : e->angleDelta().x();
        int pitchAtMouse = yToPitch(e->position().toPoint().y());
        m_rowH = clamp(m_rowH + (d > 0 ? 1 : -1), 5, 30);
        m_topPitch = clamp(pitchAtMouse + (e->position().toPoint().y() - L.rulerH) / m_rowH, minTopPitch(), 127);
        updateScrollbars();
        invalidate();
        e->accept();
        return;
    }
    if (e->modifiers() & Qt::ControlModifier) {
        if (e->modifiers() & Qt::ShiftModifier) {
            m_rowH = clamp(m_rowH + (dy > 0 ? 1 : -1), 5, 30);
        } else {
            Tick anchor = xToTick(e->position().toPoint().x());
            m_ppt = clamp(m_ppt * (dy > 0 ? 1.15 : 1 / 1.15), 0.002, 2.0);
            m_scrollTick = std::max<Tick>(0, anchor - (Tick)std::llround((e->position().toPoint().x() - L.keyW) / m_ppt));
        }
        updateScrollbars();
        invalidate();
    } else if (e->modifiers() & Qt::ShiftModifier || e->angleDelta().x() != 0) {
        int d = e->angleDelta().x() != 0 ? e->angleDelta().x() : dy;
        m_scrollTick = std::max<Tick>(0, m_scrollTick - (Tick)(d / m_ppt * 0.5));
        updateScrollbars();
        invalidate();
    } else {
        m_topPitch = clamp(m_topPitch + (dy > 0 ? 3 : -3), minTopPitch(), 127);
        updateScrollbars();
        invalidate();
    }
    e->accept();
}
void PianoRollView::leaveEvent(QEvent*) { update(); }
// Arrow keys (with any modifier) are note editing while the piano roll has focus: claim them before application shortcuts (Ctrl+Up/Down = tempo) can take them
bool PianoRollView::event(QEvent* e) {
    if (e->type() == QEvent::ShortcutOverride) {
        auto* ke = static_cast<QKeyEvent*>(e);
        int k = ke->key();
        if (k == Qt::Key_Up || k == Qt::Key_Down || k == Qt::Key_Left || k == Qt::Key_Right) {
            ke->accept();
            return true;
        }
    }
    return QWidget::event(e);
}
// FL Studio default keybinds (typing keyboard mode intercepts unmodified letters before this, see MainWindow)
void PianoRollView::keyPressEvent(QKeyEvent* e) {
    PatternChannelData* d = data();
    if (!d) {
        QWidget::keyPressEvent(e);
        return;
    }
    bool ctrl = e->modifiers() & Qt::ControlModifier, shift = e->modifiers() & Qt::ShiftModifier, alt = e->modifiers() & Qt::AltModifier;
    auto tool = [this](Tool t) {
        setTool(t);
        Q_EMIT toolChanged(t);
    };
    auto neighbourChannel = [this](int dir, bool withNotes) {
        auto& ch = m_app.project().channels;
        int i = -1;
        for (size_t k = 0; k < ch.size(); ++k)
            if (ch[k].id == m_channel) i = (int)k;
        if (i < 0) return;
        Pattern* pat = pattern();
        for (int j = i + dir; j >= 0 && j < (int)ch.size(); j += dir) {
            if (withNotes) {
                auto* cd = pat ? pat->channelData(ch[j].id) : nullptr;
                if (!cd || cd->notes.empty()) continue;
            }
            Q_EMIT channelRequested(ch[j].id);
            return;
        }
    };
    switch (e->key()) {
    case Qt::Key_Delete:
        if (ctrl) {
            deleteTimeSpace();
            return;
        }
        deleteSelected();
        return;
    case Qt::Key_Insert:
        if (ctrl) {
            insertTimeSpace();
            return;
        }
        break;
    case Qt::Key_C:
        if (ctrl) {
            copySelected();
            return;
        }
        if (shift) {
            selectByPitch();
            return;
        }
        break;
    case Qt::Key_X:
        if (ctrl) {
            cutSelected();
            return;
        }
        break;
    case Qt::Key_V:
        if (ctrl) {
            paste();
            return;
        }
        break;
    case Qt::Key_D:
        if (ctrl) {
            m_selected.clear();
            invalidate();
            return;
        }
        if (shift) {
            setSelectedLength(snapTicks());
            return;
        }
        tool(Erase);
        return;
    case Qt::Key_B:
        if (ctrl) {
            duplicateSelected();
            return;
        }
        tool(Draw);
        return;
    case Qt::Key_P:
        if (!ctrl && !alt) {
            tool(Draw);
            return;
        }
        break;
    case Qt::Key_E:
        if (!ctrl && !alt) {
            tool(Select);
            return;
        }
        break;
    case Qt::Key_T:
        if (!ctrl && !alt) {
            tool(MuteTool);
            return;
        }
        break;
    case Qt::Key_I:
        if (shift) {
            invertSelection();
            return;
        }
        break;
    case Qt::Key_A:
        if (alt) {
            arpeggiate();
            return;
        }
        if (ctrl) {
            selectAll();
            return;
        }
        break;
    case Qt::Key_S:
        if (alt) {
            strum();
            return;
        }
        break;
    case Qt::Key_R:
        if (alt) {
            randomize(0.2f, 0.2f);
            return;
        }
        break;
    case Qt::Key_Q:
        if (ctrl || alt) {
            quantize(shift);
            return;
        }
        break;
    case Qt::Key_L:
        if (ctrl) {
            legato();
            return;
        }
        break;
    case Qt::Key_G:
        if (ctrl) {
            glue();
            return;
        }
        if (!alt) {
            neighbourChannel(-1, false);
            return;
        }
        break;
    case Qt::Key_K:
        if (!ctrl && !alt) {
            neighbourChannel(1, false);
            return;
        }
        break;
    case Qt::Key_H:
        if (!ctrl && !alt) {
            neighbourChannel(-1, true);
            return;
        }
        break;
    case Qt::Key_J:
        if (!ctrl && !alt) {
            neighbourChannel(1, true);
            return;
        }
        break;
    case Qt::Key_M:
        if (alt) {
            muteSelected(!shift);
            return;
        }
        break;
    case Qt::Key_F:
        if (!ctrl && !alt) {
            setLane((m_lane + 1) % 4);
            return;
        }
        break;
    case Qt::Key_Up: transpose(ctrl ? 12 : 1); return;   // Shift+Up/Down: move selection; Ctrl: one octave
    case Qt::Key_Down: transpose(ctrl ? -12 : -1); return;
    case Qt::Key_Left:
    case Qt::Key_Right: {
        if (m_selected.empty()) break;
        Tick dt = (e->key() == Qt::Key_Left ? -1 : 1) * ((ctrl || alt) ? PPQ / 32 : snapTicks());
        beginEdit();
        Tick minS = LLONG_MAX;
        for (auto& n : d->notes)
            if (m_selected.count(n.id)) minS = std::min(minS, n.start);
        if (minS + dt < 0) dt = -minS;
        for (auto& n : d->notes)
            if (m_selected.count(n.id)) n.start += dt;
        commitEdit(alt ? "Nudge notes" : "Move notes");
        return;
    }
    case Qt::Key_PageUp:
    case Qt::Key_PageDown: {
        Layout L = layout();
        Tick anchor = xToTick(L.grid.center().x());
        m_ppt = clamp(m_ppt * (e->key() == Qt::Key_PageUp ? 1.3 : 1 / 1.3), 0.002, 2.0);
        m_scrollTick = std::max<Tick>(0, anchor - (Tick)std::llround(L.grid.width() / 2 / m_ppt));
        updateScrollbars();
        invalidate();
        return;
    }
    case Qt::Key_Escape:
        m_selected.clear();
        m_timeSel = false;
        invalidate();
        return;
    case Qt::Key_1: tool(Draw); return;
    case Qt::Key_2: tool(Select); return;
    case Qt::Key_3: tool(Erase); return;
    default: break;
    }
    QWidget::keyPressEvent(e);
}
// selection helpers used by the keybinds above
void PianoRollView::invertSelection() {
    PatternChannelData* d = data();
    if (!d) return;
    std::set<Id> inv;
    for (auto& n : notesView())
        if (!m_selected.count(n.id)) inv.insert(n.id);
    m_selected = inv;
    invalidate();
}
void PianoRollView::selectByPitch() {
    PatternChannelData* d = data();
    if (!d || m_selected.empty()) return;
    std::set<int> pitches;
    for (auto& n : notesView())
        if (m_selected.count(n.id)) pitches.insert(n.pitch);
    for (auto& n : notesView())
        if (pitches.count(n.pitch)) m_selected.insert(n.id);
    invalidate();
}
void PianoRollView::muteSelected(bool mute) {
    PatternChannelData* d = data();
    if (!d || m_selected.empty()) return;
    beginEdit();
    for (auto& n : d->notes)
        if (m_selected.count(n.id)) n.muted = mute;
    commitEdit(mute ? "Mute notes" : "Unmute notes");
}
// Glue: merge each selected note into the previous selected note of the same pitch that it touches or overlaps
void PianoRollView::glue() {
    PatternChannelData* d = data();
    if (!d || m_selected.size() < 2) return;
    beginEdit();
    std::vector<Note*> sel;
    for (auto& n : d->notes)
        if (m_selected.count(n.id)) sel.push_back(&n);
    std::sort(sel.begin(), sel.end(), [](Note* a, Note* b) { return a->pitch != b->pitch ? a->pitch < b->pitch : a->start < b->start; });
    std::set<Id> gone;
    for (size_t i = 1; i < sel.size(); ++i) {
        Note* a = sel[i - 1];
        Note* b = sel[i];
        if (a->pitch == b->pitch && b->start <= a->start + a->duration + snapTicks() / 4) {
            a->duration = std::max(a->duration, b->start + b->duration - a->start);
            gone.insert(b->id);
            sel[i] = a;
        }
    }
    std::erase_if(d->notes, [&](const Note& n) { return gone.count(n.id); });
    for (Id id : gone) m_selected.erase(id);
    commitEdit("Glue notes");
}
// Ctrl+Delete / Ctrl+Insert: remove or insert the time selection's span, shifting everything after it
void PianoRollView::deleteTimeSpace() {
    PatternChannelData* d = data();
    if (!d || !m_timeSel || m_selEnd <= m_selStart) return;
    Tick len = m_selEnd - m_selStart;
    beginEdit();
    std::erase_if(d->notes, [&](const Note& n) { return n.start >= m_selStart && n.start < m_selEnd; });
    for (auto& n : d->notes)
        if (n.start >= m_selEnd) n.start -= len;
    commitEdit("Delete space");
    m_timeSel = false;
    invalidate();
}
void PianoRollView::insertTimeSpace() {
    PatternChannelData* d = data();
    if (!d || !m_timeSel || m_selEnd <= m_selStart) return;
    Tick len = m_selEnd - m_selStart;
    beginEdit();
    for (auto& n : d->notes)
        if (n.start >= m_selStart) n.start += len;
    commitEdit("Insert space");
}
void PianoRollView::keyReleaseEvent(QKeyEvent* e) { QWidget::keyReleaseEvent(e); }
void PianoRollView::contextMenuEvent(QContextMenuEvent* e) { e->accept(); }   // menus open on right-button release (right-drag erases)
void PianoRollView::showAreaMenu(const QPoint& pos, const QPoint& globalPos) {
    Layout L = layout();
    if (!L.grid.contains(pos)) return;
    QContextMenuEvent ev(QContextMenuEvent::Mouse, pos, globalPos);
    QContextMenuEvent* e = &ev;
    QMenu m(this);
    m.addAction("Select all", [this] { selectAll(); });
    m.addAction("Quantize (Ctrl+Q)", [this] { quantize(false); });
    m.addAction("Quantize starts and lengths", [this] { quantize(true); });
    m.addSeparator();
    m.addAction("Arpeggiate", [this] { arpeggiate(); });
    m.addAction("Strum", [this] { strum(); });
    m.addAction("Legato (Ctrl+L)", [this] { legato(); });
    m.addAction("Randomize...", [this] {
        bool ok;
        int a = QInputDialog::getInt(this, "Randomize", "Velocity / pan amount (%):", 20, 0, 100, 1, &ok);
        if (ok) randomize(a / 100.f, a / 100.f);
    });
    m.addAction("Transpose +12", [this] { transpose(12); });
    m.addAction("Transpose -12", [this] { transpose(-12); });
    m.addSeparator();
    m.addAction("Zoom to fit", [this] { zoomToFit(); });
    m.exec(e->globalPos());
}

// ---------------------------------------------------------------- tools
void PianoRollView::selectAll() {
    if (!data()) return;
    for (auto& n : notesView()) m_selected.insert(n.id);
    invalidate();
}
void PianoRollView::deleteSelected() {
    PatternChannelData* d = data();
    if (!d) return;
    if (m_selected.empty() && m_timeSel) {
        beginEdit();
        std::erase_if(d->notes, [this](const Note& n) { return n.start >= m_selStart && n.start < m_selEnd; });
        commitEdit("Delete notes in selection");
        return;
    }
    if (m_selected.empty()) return;
    beginEdit();
    std::erase_if(d->notes, [this](const Note& n) { return m_selected.count(n.id) > 0; });
    m_selected.clear();
    commitEdit("Delete notes");
}
void PianoRollView::copySelected() {
    if (!data()) return;
    m_clipboard.clear();
    Tick minS = LLONG_MAX;
    for (auto& n : notesView())
        if (m_selected.count(n.id)) {
            m_clipboard.push_back(n);
            minS = std::min(minS, n.start);
        }
    for (auto& n : m_clipboard) n.start -= minS;
}
void PianoRollView::cutSelected() {
    copySelected();
    deleteSelected();
}
void PianoRollView::paste() {
    PatternChannelData* d = data();
    if (!d || m_clipboard.empty()) return;
    beginEdit();
    Tick at = m_playTick >= 0 ? snap((Tick)m_playTick, true) : (m_hoverPos.x() > 0 ? snap(xToTick(m_hoverPos.x()), true) : m_scrollTick);
    m_selected.clear();
    for (auto n : m_clipboard) {
        n.id = m_app.project().newId();
        n.start += at;
        d->notes.push_back(n);
        m_selected.insert(n.id);
    }
    commitEdit("Paste notes");
}
void PianoRollView::duplicateSelected() {
    PatternChannelData* d = data();
    if (!d) return;
    if (m_timeSel) {   // FL-style: duplicate the notes inside the time selection to right after it and move the selection along
        Tick span = m_selEnd - m_selStart;
        beginEdit();
        std::vector<Note> copies;
        for (auto& n : d->notes) {
            bool inside = m_selected.empty() ? (n.start >= m_selStart && n.start < m_selEnd) : m_selected.count(n.id) > 0;
            if (inside) {
                Note c = n;
                c.id = m_app.project().newId();
                c.start += span;
                copies.push_back(c);
            }
        }
        m_selected.clear();
        for (auto& c : copies) {
            d->notes.push_back(c);
            m_selected.insert(c.id);
        }
        commitEdit("Duplicate selection");
        m_selStart += span;
        m_selEnd += span;
        invalidate();
        return;
    }
    if (m_selected.empty()) return;
    beginEdit();
    Tick minS = LLONG_MAX, maxE = 0;
    for (auto& n : d->notes)
        if (m_selected.count(n.id)) {
            minS = std::min(minS, n.start);
            maxE = std::max(maxE, n.start + n.duration);
        }
    Tick span = snap(maxE - minS + snapTicks() - 1, true);
    if (span <= 0) span = maxE - minS;
    std::vector<Note> copies;
    for (auto& n : d->notes)
        if (m_selected.count(n.id)) {
            Note c = n;
            c.id = m_app.project().newId();
            c.start += span;
            copies.push_back(c);
        }
    m_selected.clear();
    for (auto& c : copies) {
        d->notes.push_back(c);
        m_selected.insert(c.id);
    }
    commitEdit("Duplicate notes");
}
void PianoRollView::quantize(bool lengths) {
    PatternChannelData* d = data();
    if (!d) return;
    beginEdit();
    Tick s = snapTicks();
    if (s <= 1) s = PPQ / 4;
    bool any = !m_selected.empty();
    for (auto& n : d->notes) {
        if (any && !m_selected.count(n.id)) continue;
        n.start = ((n.start + s / 2) / s) * s;
        if (lengths) n.duration = std::max<Tick>(s, ((n.duration + s / 2) / s) * s);
    }
    commitEdit("Quantize");
}
void PianoRollView::transpose(int semis) {
    PatternChannelData* d = data();
    if (!d || m_selected.empty()) return;
    beginEdit();
    for (auto& n : d->notes)
        if (m_selected.count(n.id)) n.pitch = clamp(n.pitch + semis, 0, 127);
    commitEdit("Transpose");
}
void PianoRollView::legato() {
    PatternChannelData* d = data();
    if (!d) return;
    beginEdit();
    std::vector<Note*> sel;
    for (auto& n : d->notes)
        if (m_selected.empty() || m_selected.count(n.id)) sel.push_back(&n);
    std::sort(sel.begin(), sel.end(), [](Note* a, Note* b) { return a->start < b->start; });
    for (size_t i = 0; i + 1 < sel.size(); ++i) {
        Tick next = sel[i + 1]->start;
        if (next > sel[i]->start) sel[i]->duration = next - sel[i]->start;
    }
    commitEdit("Legato");
}
void PianoRollView::strum() {
    PatternChannelData* d = data();
    if (!d || m_selected.size() < 2) return;
    beginEdit();
    std::vector<Note*> sel;
    for (auto& n : d->notes)
        if (m_selected.count(n.id)) sel.push_back(&n);
    std::sort(sel.begin(), sel.end(), [](Note* a, Note* b) { return a->pitch < b->pitch; });
    Tick step = PPQ / 16;
    Tick base = sel.front()->start;
    for (size_t i = 0; i < sel.size(); ++i) {
        Tick off = (Tick)i * step;
        Tick end = sel[i]->start + sel[i]->duration;
        sel[i]->start = base + off;
        sel[i]->duration = std::max<Tick>(PPQ / 32, end - sel[i]->start);
    }
    commitEdit("Strum");
}
void PianoRollView::arpeggiate() {
    PatternChannelData* d = data();
    if (!d || m_selected.size() < 2) return;
    beginEdit();
    std::vector<Note> sel;
    for (auto& n : d->notes)
        if (m_selected.count(n.id)) sel.push_back(n);
    std::sort(sel.begin(), sel.end(), [](const Note& a, const Note& b) { return a.pitch < b.pitch; });
    Tick start = LLONG_MAX, end = 0;
    for (auto& n : sel) {
        start = std::min(start, n.start);
        end = std::max(end, n.start + n.duration);
    }
    Tick rate = snapTicks() > 1 ? snapTicks() : PPQ / 4;
    std::erase_if(d->notes, [this](const Note& n) { return m_selected.count(n.id) > 0; });
    m_selected.clear();
    size_t k = 0;
    for (Tick t = start; t + rate <= end; t += rate, ++k) {
        Note n = sel[k % sel.size()];
        n.id = m_app.project().newId();
        n.start = t;
        n.duration = rate;
        d->notes.push_back(n);
        m_selected.insert(n.id);
    }
    commitEdit("Arpeggiate");
}
void PianoRollView::randomize(float velAmt, float panAmt) {
    PatternChannelData* d = data();
    if (!d) return;
    beginEdit();
    dsp::Rng r;
    r.s = (uint32_t)QDateTime::currentMSecsSinceEpoch();
    for (auto& n : d->notes) {
        if (!m_selected.empty() && !m_selected.count(n.id)) continue;
        n.velocity = clamp(n.velocity + r.bipolar() * velAmt, 0.05f, 1.f);
        n.pan = clamp(n.pan + r.bipolar() * panAmt, -1.f, 1.f);
    }
    commitEdit("Randomize");
}
void PianoRollView::setSelectedLength(Tick len) {
    PatternChannelData* d = data();
    if (!d) return;
    beginEdit();
    for (auto& n : d->notes)
        if (m_selected.count(n.id)) n.duration = len;
    commitEdit("Note length");
}

// ---------------------------------------------------------------- panel
PianoRollPanel::PianoRollPanel(App& app, QWidget* parent)
    : QWidget(parent)
    , m_app(app) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(2, 2, 2, 2);
    lay->setSpacing(2);
    auto* top = new QHBoxLayout();
    top->setSpacing(4);
    // FL-style header: a menu button at the far left, then tight icon-only groups with no labels, and the channel the
    // roll is pointed at parked on the right. Related buttons sit flush against each other so they read as one control.
    const int kH = theme::kBarH;
    auto sep = [&] {
        top->addSpacing(4);
        top->addWidget(theme::barSeparator(this));
        top->addSpacing(4);
    };
    auto group = [&]() -> QHBoxLayout* {
        auto* g = new QHBoxLayout();
        g->setSpacing(1);
        g->setContentsMargins(0, 0, 0, 0);
        top->addLayout(g);
        return g;
    };
    // The icon is painted in one fixed light colour, so a checked button only changes its background - no orange glyphs.
    auto mkTool = [&](QHBoxLayout* g, theme::Glyph icon, const QString& tip, int w = 0) {
        auto* b = new QToolButton(this);
        theme::setGlyph(b, icon, 24);
        b->setToolTip(tip);
        b->setCheckable(true);
        b->setFixedSize(w ? w : kH + 4, kH);
        b->setFocusPolicy(Qt::NoFocus);
        g->addWidget(b);
        return b;
    };
    auto mkCombo = [&](int w, const QString& tip) {
        auto* c = new QComboBox(this);
        c->setFixedHeight(kH);
        c->setFixedWidth(w);
        c->setToolTip(tip);
        c->setFocusPolicy(Qt::NoFocus);
        top->addWidget(c);
        return c;
    };

    auto* menuBtn = new QToolButton(this);
    menuBtn->setText("\u2261");
    menuBtn->setToolTip("Piano roll tools and edit actions");
    menuBtn->setFixedSize(30, kH);
    menuBtn->setFocusPolicy(Qt::NoFocus);
    menuBtn->setPopupMode(QToolButton::InstantPopup);
    top->addWidget(menuBtn);
    auto* tm = new QMenu(menuBtn);
    tm->addAction("Quantize starts\tCtrl+Q", [this] { m_view->quantize(false); });
    tm->addAction("Quantize starts + lengths", [this] { m_view->quantize(true); });
    tm->addSeparator();
    tm->addAction("Arpeggiate selection", [this] { m_view->arpeggiate(); });
    tm->addAction("Strum selection", [this] { m_view->strum(); });
    tm->addAction("Legato\tCtrl+L", [this] { m_view->legato(); });
    tm->addAction("Randomize velocity/pan...", [this] {
        bool ok;
        int a = QInputDialog::getInt(this, "Randomize", "Amount (%):", 20, 0, 100, 1, &ok);
        if (ok) m_view->randomize(a / 100.f, a / 100.f);
    });
    tm->addSeparator();
    tm->addAction("Transpose +1", [this] { m_view->transpose(1); });
    tm->addAction("Transpose -1", [this] { m_view->transpose(-1); });
    tm->addAction("Transpose +12", [this] { m_view->transpose(12); });
    tm->addAction("Transpose -12", [this] { m_view->transpose(-12); });
    tm->addSeparator();
    tm->addAction("Select all\tCtrl+A", [this] { m_view->selectAll(); });
    tm->addAction("Duplicate\tCtrl+D", [this] { m_view->duplicateSelected(); });
    tm->addAction("Copy\tCtrl+C", [this] { m_view->copySelected(); });
    tm->addAction("Paste\tCtrl+V", [this] { m_view->paste(); });
    tm->addAction("Delete\tDel", [this] { m_view->deleteSelected(); });
    tm->addSeparator();
    tm->addAction("Zoom to fit", [this] { m_view->zoomToFit(); });
    sep();
    {
        auto* g = group();
        m_draw = mkTool(
            g, theme::Glyph::Pencil, "Draw (P): click to add notes, drag to move, drag the right edge to resize, right-click to erase");
        m_select = mkTool(g, theme::Glyph::Marquee, "Select (E): drag a rectangle to select notes");
        m_erase = mkTool(g, theme::Glyph::Eraser, "Erase (D) - also active while you hold the right button");
        m_mute = mkTool(
            g, theme::Glyph::MuteSpeaker, "Mute (T): click or drag over notes to mute them - also after a double right-click + drag");
    }
    m_draw->setChecked(true);
    sep();
    m_snap = TransportBar::makeSnapCombo(this);
    m_snap->setFixedHeight(kH);
    m_snap->setFixedWidth(96);
    m_snap->setToolTip("Snap: the grid notes are placed and resized to (hold Alt to bypass)");
    m_snap->setFocusPolicy(Qt::NoFocus);
    top->addWidget(m_snap);
    sep();
    m_root = mkCombo(62, "Scale root: notes outside the scale are dimmed in the grid");
    for (int i = 0; i < 12; ++i) m_root->addItem(theme::noteName(i).chopped(1));
    m_scale = mkCombo(150, "Scale highlight: notes outside the scale are dimmed in the grid");
    m_scale->addItems(PianoRollView::scaleNames());
    m_chord = mkCombo(100, "Chord helper: drawing a note adds the chord intervals");
    m_chord->addItems(PianoRollView::chordNames());
    sep();
    {
        auto* g = group();
        m_ghost = mkTool(g, theme::Glyph::Ghost, "Ghost notes: show the other channels behind this one", 30);
        m_ghost->setChecked(true);
        m_repeat = mkTool(g, theme::Glyph::Repeat, "Note repeat: holding a key on the keyboard retriggers at the snap rate", 30);
    }
    m_labels = mkCombo(96, "Note names on the key column");
    m_labels->addItems({"C only", "All notes", "No labels"});
    top->addStretch(1);
    m_channel = new QComboBox(this);
    m_channel->setFixedHeight(kH);
    m_channel->setMinimumWidth(150);
    m_channel->setToolTip("Channel this piano roll is editing");
    m_channel->setFocusPolicy(Qt::NoFocus);
    top->addWidget(m_channel);
    menuBtn->setMenu(tm);
    lay->addLayout(top);
    m_view = new PianoRollView(app, this);
    lay->addWidget(m_view, 1);
    connect(m_view, &PianoRollView::channelRequested, this, [this](Id id) {
        m_app.selectChannel(id);
        showChannel(id);
    });
    auto showMode = [this](int t) {
        m_draw->setChecked(t == 0);
        m_select->setChecked(t == 1);
        m_erase->setChecked(t == 2);
        m_mute->setChecked(t == 3);
    };
    connect(m_view, &PianoRollView::toolChanged, this, showMode);
    connect(m_view, &PianoRollView::modeChanged, this, showMode);
    connect(m_mute, &QToolButton::clicked, this, [this] { m_view->setTool(PianoRollView::MuteTool); });
    connect(m_draw, &QToolButton::clicked, this, [this] {
        m_view->setTool(PianoRollView::Draw);
        m_draw->setChecked(true);
        m_select->setChecked(false);
        m_erase->setChecked(false);
    });
    connect(m_select, &QToolButton::clicked, this, [this] {
        m_view->setTool(PianoRollView::Select);
        m_draw->setChecked(false);
        m_select->setChecked(true);
        m_erase->setChecked(false);
    });
    connect(m_erase, &QToolButton::clicked, this, [this] {
        m_view->setTool(PianoRollView::Erase);
        m_draw->setChecked(false);
        m_select->setChecked(false);
        m_erase->setChecked(true);
    });
    connect(m_channel, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (!m_updating && i >= 0) { m_view->setChannel((Id)m_channel->itemData(i).toULongLong()); }
    });
    connect(m_snap, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_view->setSnapDivision(TransportBar::snapDivisionFromIndex(i));
        m_view->refresh();
    });
    connect(
        m_scale, &QComboBox::currentIndexChanged, this, [this](int) { m_view->setScale(m_root->currentIndex(), m_scale->currentIndex()); });
    connect(
        m_root, &QComboBox::currentIndexChanged, this, [this](int) { m_view->setScale(m_root->currentIndex(), m_scale->currentIndex()); });
    connect(m_chord, &QComboBox::currentIndexChanged, this, [this](int i) { m_view->setChord(i); });
    {
        int m = QSettings().value("pianoroll/keyLabels", (int)PianoRollView::LabelsC).toInt();
        m_labels->setCurrentIndex(m);
        m_view->setKeyLabels(m);
    }
    connect(m_labels, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_view->setKeyLabels(i);
        QSettings().setValue("pianoroll/keyLabels", i);
    });
    connect(m_ghost, &QToolButton::toggled, this, [this](bool b) { m_view->setGhost(b); });
    connect(m_repeat, &QToolButton::toggled, this, [this](bool b) { m_view->setNoteRepeat(b); });
    refresh(ChangeAll);
}
void PianoRollPanel::showChannel(Id id) {
    for (int i = 0; i < m_channel->count(); ++i)
        if ((Id)m_channel->itemData(i).toULongLong() == id) {
            m_channel->setCurrentIndex(i);
            break;
        }
    m_view->setChannel(id);
}
void PianoRollPanel::refresh(uint32_t flags) {
    if (flags & (ChangeChannels | ChangeStructure | ChangeAll)) {
        m_updating = true;
        Id cur = m_view->channel();
        m_channel->clear();
        for (auto& c : m_app.project().channels) {
            m_channel->addItem(c.name, (qulonglong)c.id);
            m_channel->setItemData(m_channel->count() - 1, c.color, Qt::DecorationRole);
        }
        bool found = false;
        for (int i = 0; i < m_channel->count(); ++i)
            if ((Id)m_channel->itemData(i).toULongLong() == cur) {
                m_channel->setCurrentIndex(i);
                found = true;
            }
        if (!found && m_channel->count()) {
            m_channel->setCurrentIndex(0);
            m_view->setChannel((Id)m_channel->itemData(0).toULongLong());
        }
        m_updating = false;
    }
    m_view->refresh();
}
void PianoRollPanel::uiTick() { m_view->uiTick(); }

}   // namespace mt
