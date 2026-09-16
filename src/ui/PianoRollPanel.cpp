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
#include <algorithm>
#include <climits>
#include <QDateTime>

namespace mt {

static const int kScales[][12] = { {1,1,1,1,1,1,1,1,1,1,1,1}, {1,0,1,0,1,1,0,1,0,1,0,1}, {1,0,1,1,0,1,0,1,1,0,1,0}, {1,0,1,1,0,1,0,1,1,0,0,1}, {1,0,1,0,1,0,0,1,0,1,0,0}, {1,0,0,1,0,1,0,1,0,0,1,0}, {1,0,1,1,0,1,0,1,0,1,0,1}, {1,1,0,1,0,1,0,1,1,0,1,0}, {1,0,1,0,1,0,1,1,0,1,0,1}, {1,0,0,1,0,1,1,1,0,0,1,0} };
QStringList PianoRollView::scaleNames() { return {"Off", "Major", "Natural minor", "Harmonic minor", "Pentatonic major", "Pentatonic minor", "Dorian", "Phrygian", "Lydian", "Blues"}; }
static const std::vector<std::vector<int>> kChords = { {}, {0,4,7}, {0,3,7}, {0,4,7,11}, {0,3,7,10}, {0,4,7,10}, {0,3,6}, {0,4,8}, {0,5,7}, {0,2,7}, {0,7}, {0,12}, {0,4,7,12}, {0,3,7,12}, {0,4,7,11,14}, {0,3,7,10,14} };
QStringList PianoRollView::chordNames() { return {"None", "Major", "Minor", "Maj7", "Min7", "Dom7", "Dim", "Aug", "Sus4", "Sus2", "Power (5th)", "Octave", "Major + oct", "Minor + oct", "Maj9", "Min9"}; }

PianoRollView::PianoRollView(App& app, QWidget* parent) : QWidget(parent), m_app(app) {
    setFocusPolicy(Qt::StrongFocus); setMouseTracking(true);
    m_hbar = new QScrollBar(Qt::Horizontal, this); m_vbar = new QScrollBar(Qt::Vertical, this);
    connect(m_hbar, &QScrollBar::valueChanged, this, [this](int v) { if (!m_updatingBars) { m_scrollTick = (Tick)v * PPQ / 16; invalidate(); } });
    connect(m_vbar, &QScrollBar::valueChanged, this, [this](int v) { if (!m_updatingBars) { m_topPitch = 127 - v; invalidate(); } });
    connect(&m_repeatTimer, &QTimer::timeout, this, [this] { if (m_previewPitch >= 0) { m_app.noteOff(m_previewPitch); m_app.noteOn(m_previewPitch); } });
}
PianoRollView::Layout PianoRollView::layout() const { Layout l; int w = width() - 14, h = height() - 14; l.keys = QRect(0, l.rulerH, l.keyW, h - l.rulerH - l.velH); l.ruler = QRect(l.keyW, 0, w - l.keyW, l.rulerH); l.grid = QRect(l.keyW, l.rulerH, w - l.keyW, h - l.rulerH - l.velH); l.vel = QRect(l.keyW, h - l.velH, w - l.keyW, l.velH); return l; }
void PianoRollView::resizeEvent(QResizeEvent*) { m_hbar->setGeometry(layout().keyW, height() - 14, width() - layout().keyW - 14, 14); m_vbar->setGeometry(width() - 14, layout().rulerH, 14, height() - layout().rulerH - 14); updateScrollbars(); invalidate(); }
Pattern* PianoRollView::pattern() const { return m_app.project().pattern(m_app.project().selectedPatternId); }
PatternChannelData* PianoRollView::data() const { Pattern* p = pattern(); if (!p || !m_channel) return nullptr; return &p->ensureChannel(m_channel); }
Tick PianoRollView::xToTick(int x) const { return m_scrollTick + (Tick)((x - layout().keyW) / m_ppt); }
int PianoRollView::tickToX(Tick t) const { return layout().keyW + (int)((t - m_scrollTick) * m_ppt); }
int PianoRollView::yToPitch(int y) const { return m_topPitch - (y - layout().rulerH) / m_rowH; }
int PianoRollView::pitchToY(int pitch) const { return layout().rulerH + (m_topPitch - pitch) * m_rowH; }
Tick PianoRollView::snapTicks() const { if (m_snapDiv == 0) return 1; if (m_snapDiv < 0) return m_app.project().transport.timeSig.ticksPerBar() * -m_snapDiv; return PPQ / m_snapDiv; }
Tick PianoRollView::snap(Tick t, bool force) const { if (m_altHeld && !force) return t; Tick s = snapTicks(); if (s <= 1) return t; return (t / s) * s; }
bool PianoRollView::inScale(int pitch) const { if (m_scale <= 0) return true; return kScales[m_scale][((pitch - m_scaleRoot) % 12 + 12) % 12] != 0; }
void PianoRollView::updateScrollbars() { m_updatingBars = true; Pattern* p = pattern(); Tick len = p ? p->effectiveLength(m_app.project().transport.timeSig) : PPQ * 16; Tick visible = (Tick)(layout().grid.width() / m_ppt); m_hbar->setRange(0, (int)std::max<Tick>(0, (len + PPQ * 8 - visible) * 16 / PPQ)); m_hbar->setPageStep((int)(visible * 16 / PPQ)); m_hbar->setValue((int)(m_scrollTick * 16 / PPQ)); int rows = layout().grid.height() / m_rowH; m_vbar->setRange(0, std::max(0, 127 - rows)); m_vbar->setPageStep(rows); m_vbar->setValue(127 - m_topPitch); m_updatingBars = false; }
void PianoRollView::refresh() { updateScrollbars(); invalidate(); }
void PianoRollView::zoomToFit() { Pattern* p = pattern(); if (!p) return; Tick len = std::max<Tick>(p->effectiveLength(m_app.project().transport.timeSig), PPQ); m_ppt = std::max(0.01, (double)layout().grid.width() / (len + PPQ)); m_scrollTick = 0; int lo = 127, hi = 0; if (auto* d = data()) for (auto& n : d->notes) { lo = std::min(lo, n.pitch); hi = std::max(hi, n.pitch); } if (lo <= hi) m_topPitch = std::min(127, hi + 4); refresh(); }
void PianoRollView::uiTick() {
    auto tv = m_app.engine().transport(); double pt = -1; Pattern* p = pattern();
    if (p && tv.playing && !tv.countingIn) { if (!tv.songMode) pt = tv.positionTicks; else { for (auto& c : m_app.project().clips) if (c.type == ClipType::Pattern && c.sourceId == p->id && tv.positionTicks >= c.start && tv.positionTicks < c.start + c.length) { Tick len = std::max<Tick>(p->effectiveLength(m_app.project().transport.timeSig), 1); pt = std::fmod(tv.positionTicks - c.start + c.sourceOffset, (double)len); break; } } }
    if (pt != m_playTick) { m_playTick = pt; update(); }
}

// ---------------------------------------------------------------- painting
void PianoRollView::renderCache() {
    Layout L = layout(); m_cache = QPixmap(size()); m_cache.fill(theme::bg()); QPainter p(&m_cache);
    Pattern* pat = pattern(); PatternChannelData* d = data(); const auto& ts = m_app.project().transport.timeSig; Tick patLen = pat ? pat->effectiveLength(ts) : 0;
    // grid rows
    p.setClipRect(L.grid);
    for (int pitch = m_topPitch; pitch >= 0; --pitch) { int y = pitchToY(pitch); if (y > L.grid.bottom()) break; bool black = (pitch % 12 == 1 || pitch % 12 == 3 || pitch % 12 == 6 || pitch % 12 == 8 || pitch % 12 == 10); QColor c = black ? QColor(0x1e, 0x21, 0x26) : QColor(0x24, 0x28, 0x2e); if (!inScale(pitch)) c = c.darker(135); p.fillRect(QRect(L.grid.left(), y, L.grid.width(), m_rowH), c); if (pitch % 12 == 0) { p.setPen(theme::gridStrong()); p.drawLine(L.grid.left(), y + m_rowH, L.grid.right(), y + m_rowH); } }
    // grid columns
    Tick beat = ts.ticksPerBeat(), bar = ts.ticksPerBar(); Tick step = snapTicks(); if (step * m_ppt < 6) step = beat; Tick first = (m_scrollTick / step) * step; Tick last = xToTick(L.grid.right());
    for (Tick t = first; t <= last; t += step) { int x = tickToX(t); QColor c = (t % bar == 0) ? theme::gridStrong().lighter(130) : (t % beat == 0) ? theme::gridStrong() : theme::grid(); p.setPen(c); p.drawLine(x, L.grid.top(), x, L.grid.bottom()); }
    if (patLen > 0) { int x = tickToX(patLen); p.fillRect(QRect(x, L.grid.top(), L.grid.right() - x, L.grid.height()), QColor(0, 0, 0, 60)); p.setPen(theme::accent()); p.drawLine(x, L.grid.top(), x, L.grid.bottom()); }
    // ghost notes
    if (m_ghost && pat) for (auto& oc : pat->channels) { if (oc.id == m_channel) continue; const Channel* ch = m_app.project().channel(oc.id); QColor c = ch ? ch->color : theme::textDim(); c.setAlpha(70); for (auto& n : oc.notes) { QRect r(tickToX(n.start), pitchToY(n.pitch), std::max(2, (int)(n.duration * m_ppt)), m_rowH); if (r.right() < L.grid.left() || r.left() > L.grid.right()) continue; p.fillRect(r.adjusted(0, 1, -1, -1), c); } }
    // notes
    const Channel* ch = m_app.project().channel(m_channel); QColor base = ch ? ch->color : theme::accent();
    if (d) for (auto& n : d->notes) { QRect r(tickToX(n.start), pitchToY(n.pitch), std::max(3, (int)(n.duration * m_ppt)), m_rowH); if (r.right() < L.grid.left() || r.left() > L.grid.right()) continue; bool sel = m_selected.count(n.id); QColor c = base.lighter((int)(60 + n.velocity * 70)); if (n.muted) c = c.darker(200); p.setPen(sel ? QPen(Qt::white, 1.5) : QPen(base.darker(150), 1)); p.setBrush(c); p.drawRoundedRect(r.adjusted(0, 1, -1, -1), 2, 2); if (r.width() > 28 && m_rowH >= 10) { p.setPen(theme::bg()); p.setFont(theme::smallFont()); p.drawText(r.adjusted(3, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, theme::noteName(n.pitch)); } }
    p.setClipping(false);
    // ruler
    p.fillRect(L.ruler, theme::panel()); p.setClipRect(L.ruler); p.setFont(theme::smallFont());
    for (Tick t = (m_scrollTick / beat) * beat; t <= last; t += beat) { int x = tickToX(t); bool isBar = t % bar == 0; p.setPen(isBar ? theme::text() : theme::textDim()); p.drawLine(x, isBar ? L.ruler.top() + 6 : L.ruler.top() + 14, x, L.ruler.bottom()); if (isBar) p.drawText(x + 3, L.ruler.top() + 12, QString::number(t / bar + 1)); }
    p.setClipping(false);
    // keys
    p.fillRect(L.keys, theme::panel()); p.setClipRect(L.keys);
    for (int pitch = m_topPitch; pitch >= 0; --pitch) { int y = pitchToY(pitch); if (y > L.keys.bottom()) break; bool black = (pitch % 12 == 1 || pitch % 12 == 3 || pitch % 12 == 6 || pitch % 12 == 8 || pitch % 12 == 10); QRect kr(0, y, black ? L.keyW * 0.6 : L.keyW - 1, m_rowH); p.fillRect(QRect(0, y, L.keyW - 1, m_rowH), black ? QColor(0x1a, 0x1c, 0x20) : QColor(0xd8, 0xdc, 0xe2)); if (black) p.fillRect(kr, QColor(0x1a, 0x1c, 0x20)); else p.fillRect(kr, QColor(0xd8, 0xdc, 0xe2)); if (m_previewPitch == pitch) p.fillRect(kr, theme::accent()); p.setPen(theme::border()); p.drawLine(0, y + m_rowH, L.keyW, y + m_rowH); if (pitch % 12 == 0 && m_rowH >= 9) { p.setPen(QColor(0x40, 0x44, 0x4a)); p.setFont(theme::smallFont()); p.drawText(QRect(0, y, L.keyW - 4, m_rowH), Qt::AlignRight | Qt::AlignVCenter, theme::noteName(pitch)); } }
    p.setClipping(false);
    // velocity lane
    p.fillRect(L.vel, QColor(0x15, 0x18, 0x1c)); p.setPen(theme::border()); p.drawLine(L.vel.left(), L.vel.top(), L.vel.right(), L.vel.top()); p.setClipRect(L.vel);
    if (d) for (auto& n : d->notes) { int x = tickToX(n.start); if (x < L.vel.left() - 4 || x > L.vel.right()) continue; int h = (int)(n.velocity * (L.vel.height() - 6)); bool sel = m_selected.count(n.id); p.setPen(sel ? Qt::white : base); p.setBrush(sel ? base.lighter(150) : base); p.drawRect(QRect(x, L.vel.bottom() - h, 5, h)); }
    p.setClipping(false);
    m_cacheValid = true;
}
void PianoRollView::paintEvent(QPaintEvent*) {
    if (!m_cacheValid) renderCache();
    QPainter p(this); p.drawPixmap(0, 0, m_cache); Layout L = layout();
    if (m_playTick >= 0) { int x = tickToX((Tick)m_playTick); if (x >= L.grid.left() && x <= L.grid.right()) { p.setPen(QPen(theme::playhead(), 1.5)); p.drawLine(x, L.ruler.top(), x, L.vel.bottom()); } }
    if (m_drag == Rubber) { p.setPen(QPen(theme::accent(), 1, Qt::DashLine)); p.setBrush(theme::selection()); p.drawRect(m_rubber.normalized()); }
    if (m_tool == Draw && m_drag == NoDrag && L.grid.contains(m_hoverPos) && underMouse()) { int pitch = yToPitch(m_hoverPos.y()); Tick t = snap(xToTick(m_hoverPos.x())); QRect r(tickToX(t), pitchToY(pitch), std::max(3, (int)(m_lastLength * m_ppt)), m_rowH); p.setPen(QPen(theme::accent(), 1, Qt::DotLine)); p.setBrush(Qt::NoBrush); p.drawRect(r.adjusted(0, 1, -1, -1)); }
    p.setPen(theme::border()); p.drawRect(rect().adjusted(0, 0, -1, -1));
}

// ---------------------------------------------------------------- editing helpers
Note* PianoRollView::noteAt(const QPoint& pt, bool* nearEdge) const {
    PatternChannelData* d = data(); if (!d) return nullptr; Note* found = nullptr;
    for (auto it = d->notes.rbegin(); it != d->notes.rend(); ++it) { QRect r(tickToX(it->start), pitchToY(it->pitch), std::max(3, (int)(it->duration * m_ppt)), m_rowH); if (r.contains(pt)) { found = &*it; if (nearEdge) *nearEdge = (r.right() - pt.x()) <= 6 && r.width() > 10; break; } }
    return found;
}
void PianoRollView::beginEdit() { PatternChannelData* d = data(); if (!d) return; m_before = *d; m_editing = true; }
void PianoRollView::commitEdit(const QString& desc) {
    PatternChannelData* d = data(); if (!d || !m_editing) return; m_editing = false; PatternChannelData after = *d; *d = m_before; Id ch = m_channel; Id pid = pattern()->id;
    // only push if something changed
    bool same = after.notes.size() == m_before.notes.size(); if (same) for (size_t i = 0; i < after.notes.size(); ++i) { auto& a = after.notes[i]; auto& b = m_before.notes[i]; if (a.id != b.id || a.pitch != b.pitch || a.start != b.start || a.duration != b.duration || a.velocity != b.velocity || a.pan != b.pan || a.muted != b.muted || a.finePitch != b.finePitch) { same = false; break; } }
    if (same) { invalidate(); return; }
    m_app.editPattern(pid, desc, [after, ch](Pattern& p) { p.ensureChannel(ch) = after; });
}
void PianoRollView::cancelEdit() { PatternChannelData* d = data(); if (d && m_editing) *d = m_before; m_editing = false; invalidate(); }
void PianoRollView::previewNote(int pitch, bool on) { if (on) { m_app.selectChannel(m_channel); m_app.noteOn(pitch, m_lastVelocity); } else m_app.noteOff(pitch); }

// ---------------------------------------------------------------- mouse
void PianoRollView::mousePressEvent(QMouseEvent* e) {
    setFocus(); Layout L = layout(); m_pressPos = m_lastPos = e->pos(); m_altHeld = e->modifiers() & Qt::AltModifier;
    if (e->button() == Qt::MiddleButton) { m_drag = Pan; return; }
    if (L.ruler.contains(e->pos())) { Tick t = snap(xToTick(e->pos().x())); if (!m_app.project().transport.songMode) m_app.seek(std::max<Tick>(0, t)); return; }
    if (L.keys.contains(e->pos())) { int pitch = yToPitch(e->pos().y()); m_previewPitch = pitch; previewNote(pitch, true); if (m_noteRepeat) m_repeatTimer.start((int)(ticksToSeconds(snapTicks(), m_app.project().transport.bpm) * 1000)); m_drag = KeyPreview; invalidate(); return; }
    if (L.vel.contains(e->pos())) { beginEdit(); m_drag = Velocity; mouseMoveEvent(e); return; }
    if (!L.grid.contains(e->pos())) return;
    PatternChannelData* d = data(); if (!d) return;
    bool edge = false; Note* n = noteAt(e->pos(), &edge);
    if (e->button() == Qt::RightButton || m_tool == Erase) { beginEdit(); m_drag = Erasing; m_erasedAny = false; if (n) { std::erase_if(d->notes, [id = n->id](const Note& x) { return x.id == id; }); m_selected.erase(n->id); m_erasedAny = true; invalidate(); } return; }
    if (n) {
        if (e->modifiers() & Qt::ControlModifier && !(e->modifiers() & Qt::ShiftModifier)) { if (m_selected.count(n->id)) m_selected.erase(n->id); else m_selected.insert(n->id); invalidate(); }
        else if (!m_selected.count(n->id)) { m_selected.clear(); m_selected.insert(n->id); }
        beginEdit(); m_dragNote = n->id; m_pressTick = xToTick(e->pos().x()); m_pressPitch = yToPitch(e->pos().y()); m_duplicated = false;
        if (e->modifiers() & Qt::ShiftModifier && e->modifiers() & Qt::ControlModifier) { // ctrl+shift drag = duplicate
            std::vector<Note> copies; for (auto& x : d->notes) if (m_selected.count(x.id)) { Note c = x; c.id = m_app.project().newId(); copies.push_back(c); } m_selected.clear(); for (auto& c : copies) { m_selected.insert(c.id); d->notes.push_back(c); } m_duplicated = true; }
        m_drag = edge ? Resizing : Moving; previewNote(n->pitch, true); m_previewPitch = n->pitch; invalidate(); return;
    }
    if (m_tool == Select || (e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))) { if (!(e->modifiers() & Qt::ControlModifier)) m_selected.clear(); m_drag = Rubber; m_rubber = QRect(e->pos(), QSize(0, 0)); invalidate(); return; }
    // draw: create note (+ chord)
    beginEdit(); int pitch = clamp(yToPitch(e->pos().y()), 0, 127); Tick t = std::max<Tick>(0, snap(xToTick(e->pos().x()))); m_selected.clear();
    std::vector<int> intervals = m_chord > 0 && m_chord < (int)kChords.size() ? kChords[m_chord] : std::vector<int>{0};
    for (int iv : intervals) { Note nn; nn.id = m_app.project().newId(); nn.pitch = clamp(pitch + iv, 0, 127); nn.start = t; nn.duration = m_lastLength; nn.velocity = m_lastVelocity; d->notes.push_back(nn); m_selected.insert(nn.id); if (iv == 0) m_dragNote = nn.id; }
    m_pressTick = xToTick(e->pos().x()); m_pressPitch = pitch; m_drag = Creating; previewNote(pitch, true); m_previewPitch = pitch; invalidate();
}
void PianoRollView::mouseMoveEvent(QMouseEvent* e) {
    m_hoverPos = e->pos(); m_altHeld = e->modifiers() & Qt::AltModifier; Layout L = layout(); PatternChannelData* d = data();
    if (m_drag == NoDrag) { bool edge = false; Note* n = L.grid.contains(e->pos()) ? noteAt(e->pos(), &edge) : nullptr; if (m_tool != Erase) setCursor(n && edge ? Qt::SizeHorCursor : (n ? Qt::SizeAllCursor : (m_tool == Select ? Qt::CrossCursor : Qt::ArrowCursor))); if (m_tool == Draw) update(); return; }
    if (m_drag == Pan) { int dx = e->pos().x() - m_lastPos.x(), dy = e->pos().y() - m_lastPos.y(); m_scrollTick = std::max<Tick>(0, m_scrollTick - (Tick)(dx / m_ppt)); m_topPitch = clamp(m_topPitch + dy / m_rowH, 10, 127); m_lastPos = e->pos(); updateScrollbars(); invalidate(); return; }
    if (m_drag == KeyPreview) { int pitch = yToPitch(e->pos().y()); if (pitch != m_previewPitch && L.keys.contains(e->pos())) { previewNote(m_previewPitch, false); m_previewPitch = pitch; previewNote(pitch, true); invalidate(); } return; }
    if (!d) return;
    if (m_drag == Velocity) { float v = clamp((float)(L.vel.bottom() - e->pos().y()) / (L.vel.height() - 6), 0.f, 1.f); Tick t = xToTick(e->pos().x()); bool any = !m_selected.empty(); for (auto& n : d->notes) { int x = tickToX(n.start); if (any ? m_selected.count(n.id) : (std::abs(x - e->pos().x()) <= 4)) n.velocity = v; } m_lastVelocity = v; (void)t; invalidate(); return; }
    if (m_drag == Erasing) { if (Note* n = noteAt(e->pos())) { std::erase_if(d->notes, [id = n->id](const Note& x) { return x.id == id; }); m_erasedAny = true; invalidate(); } return; }
    if (m_drag == Rubber) { m_rubber = QRect(m_pressPos, e->pos()); QRect r = m_rubber.normalized(); for (auto& n : d->notes) { QRect nr(tickToX(n.start), pitchToY(n.pitch), std::max(3, (int)(n.duration * m_ppt)), m_rowH); if (r.intersects(nr)) m_selected.insert(n.id); else if (!(e->modifiers() & Qt::ControlModifier)) m_selected.erase(n.id); } invalidate(); return; }
    if (m_drag == Moving || m_drag == Creating) {
        Tick dt = xToTick(e->pos().x()) - m_pressTick; int dp = clamp(yToPitch(e->pos().y()), 0, 127) - m_pressPitch;
        if (m_drag == Creating) dp = 0;
        // snap the dragged note's new start, apply the same delta to all selected
        const Note* anchorBefore = nullptr; for (auto& n : m_before.notes) if (n.id == m_dragNote) anchorBefore = &n; Note anchorNew; if (!anchorBefore) { for (auto& n : d->notes) if (n.id == m_dragNote) anchorNew = n; }
        Tick anchorStart = anchorBefore ? anchorBefore->start : anchorNew.start - 0; if (!anchorBefore) { anchorStart = snap(xToTick(m_pressPos.x())); }
        Tick target = snap(anchorStart + dt); Tick delta = target - anchorStart; int minPitch = 127, maxPitch = 0; for (auto& n : m_before.notes) if (m_selected.count(n.id)) { minPitch = std::min(minPitch, n.pitch); maxPitch = std::max(maxPitch, n.pitch); }
        if (m_duplicated || m_drag == Creating) { for (auto& n : d->notes) if (m_selected.count(n.id)) { minPitch = std::min(minPitch, n.pitch); maxPitch = std::max(maxPitch, n.pitch); } }
        dp = clamp(dp, -minPitch, 127 - maxPitch);
        Tick minStart = LLONG_MAX; for (auto& n : m_before.notes) if (m_selected.count(n.id)) minStart = std::min(minStart, n.start); if (minStart != LLONG_MAX && minStart + delta < 0) delta = -minStart;
        for (auto& n : d->notes) { if (!m_selected.count(n.id)) continue; const Note* b = nullptr; for (auto& x : m_before.notes) if (x.id == n.id) b = &x; if (b) { n.start = b->start + delta; n.pitch = b->pitch + dp; } else if (m_drag == Creating) { n.start = std::max<Tick>(0, snap(xToTick(m_pressPos.x())) + delta); } else { /* duplicated notes: relative to press */ n.start = std::max<Tick>(0, n.start); } }
        if (m_drag == Creating) { for (auto& n : d->notes) if (n.id == m_dragNote) { Tick len = snap(xToTick(e->pos().x()) + snapTicks(), true) - n.start; if (e->pos().x() > m_pressPos.x() + 8 && len > 0) { for (auto& m : d->notes) if (m_selected.count(m.id)) m.duration = std::max<Tick>(len, snapTicks() > 1 ? snapTicks() : PPQ / 32); m_lastLength = len; } } }
        if (dp != 0) { int np = clamp(m_pressPitch + dp, 0, 127); for (auto& n : d->notes) if (n.id == m_dragNote) np = n.pitch; if (np != m_previewPitch) { previewNote(m_previewPitch, false); m_previewPitch = np; previewNote(np, true); } }
        invalidate(); return;
    }
    if (m_drag == Resizing) {
        Tick end = snap(xToTick(e->pos().x()) + (m_altHeld ? 0 : snapTicks() / 2)); const Note* anchor = nullptr; for (auto& n : m_before.notes) if (n.id == m_dragNote) anchor = &n; if (!anchor) return; Tick newLen = std::max<Tick>(end - anchor->start, m_altHeld ? PPQ / 64 : snapTicks()); Tick dl = newLen - anchor->duration;
        for (auto& n : d->notes) if (m_selected.count(n.id)) { const Note* b = nullptr; for (auto& x : m_before.notes) if (x.id == n.id) b = &x; if (b) n.duration = std::max<Tick>(PPQ / 64, b->duration + dl); } m_lastLength = newLen; invalidate(); return;
    }
}
void PianoRollView::mouseReleaseEvent(QMouseEvent* e) {
    if (m_drag == KeyPreview || m_drag == Moving || m_drag == Creating) { if (m_previewPitch >= 0) previewNote(m_previewPitch, false); m_previewPitch = -1; m_repeatTimer.stop(); }
    bool showMenu = m_drag == Erasing && !m_erasedAny && e->button() == Qt::RightButton && (e->pos() - m_pressPos).manhattanLength() < 4;
    switch (m_drag) { case Moving: commitEdit(m_duplicated ? "Duplicate notes" : "Move notes"); break; case Creating: commitEdit("Draw note"); break; case Resizing: commitEdit("Resize notes"); break; case Velocity: commitEdit("Edit velocity"); break; case Erasing: commitEdit("Erase notes"); break; default: break; }
    if (showMenu) { m_drag = NoDrag; showAreaMenu(e->pos(), e->globalPosition().toPoint()); return; }
    if (m_drag == Rubber) { if (m_rubber.normalized().width() < 3 && m_rubber.normalized().height() < 3 && !(e->modifiers() & Qt::ControlModifier)) m_selected.clear(); }
    m_drag = NoDrag; m_dragNote = 0; invalidate();
}
void PianoRollView::mouseDoubleClickEvent(QMouseEvent* e) { Layout L = layout(); if (L.grid.contains(e->pos())) { if (Note* n = noteAt(e->pos())) { bool ok; int v = QInputDialog::getInt(this, "Note", "Velocity (0-127):", (int)(n->velocity * 127), 1, 127, 1, &ok); if (ok) { beginEdit(); n->velocity = v / 127.f; commitEdit("Note velocity"); } } } else if (L.ruler.contains(e->pos())) zoomToFit(); }
void PianoRollView::wheelEvent(QWheelEvent* e) {
    int dy = e->angleDelta().y(); Layout L = layout();
    if (e->modifiers() & Qt::ControlModifier) { if (e->modifiers() & Qt::ShiftModifier) { m_rowH = clamp(m_rowH + (dy > 0 ? 1 : -1), 5, 30); } else { Tick anchor = xToTick(e->position().toPoint().x()); m_ppt = clamp(m_ppt * (dy > 0 ? 1.15 : 1 / 1.15), 0.005, 2.0); m_scrollTick = std::max<Tick>(0, anchor - (Tick)((e->position().toPoint().x() - L.keyW) / m_ppt)); } updateScrollbars(); invalidate(); }
    else if (e->modifiers() & Qt::ShiftModifier || e->angleDelta().x() != 0) { int d = e->angleDelta().x() != 0 ? e->angleDelta().x() : dy; m_scrollTick = std::max<Tick>(0, m_scrollTick - (Tick)(d / m_ppt * 0.5)); updateScrollbars(); invalidate(); }
    else { m_topPitch = clamp(m_topPitch + (dy > 0 ? 3 : -3), 10, 127); updateScrollbars(); invalidate(); }
    e->accept();
}
void PianoRollView::leaveEvent(QEvent*) { update(); }
void PianoRollView::keyPressEvent(QKeyEvent* e) {
    PatternChannelData* d = data(); if (!d) { QWidget::keyPressEvent(e); return; } bool ctrl = e->modifiers() & Qt::ControlModifier;
    switch (e->key()) {
    case Qt::Key_Delete: case Qt::Key_Backspace: deleteSelected(); return;
    case Qt::Key_C: if (ctrl) { copySelected(); return; } break;
    case Qt::Key_X: if (ctrl) { cutSelected(); return; } break;
    case Qt::Key_V: if (ctrl) { paste(); return; } break;
    case Qt::Key_D: if (ctrl) { duplicateSelected(); return; } setTool(Erase); Q_EMIT toolChanged(Erase); return;
    case Qt::Key_B: if (ctrl) { duplicateSelected(); return; } setTool(Draw); Q_EMIT toolChanged(Draw); return;
    case Qt::Key_P: if (!ctrl) { setTool(Draw); Q_EMIT toolChanged(Draw); return; } break;
    case Qt::Key_E: if (!ctrl) { setTool(Select); Q_EMIT toolChanged(Select); return; } break;
    case Qt::Key_A: if (e->modifiers() & Qt::AltModifier) { arpeggiate(); return; } if (ctrl) { selectAll(); return; } break;
    case Qt::Key_S: if (e->modifiers() & Qt::AltModifier) { strum(); return; } break;
    case Qt::Key_R: if (e->modifiers() & Qt::AltModifier) { randomize(0.2f, 0.2f); return; } break;
    case Qt::Key_Q: if (ctrl) { quantize(e->modifiers() & Qt::ShiftModifier); return; } break;
    case Qt::Key_L: if (ctrl) { legato(); return; } break;
    case Qt::Key_Up: transpose((ctrl || (e->modifiers() & Qt::ShiftModifier)) ? 12 : 1); return;
    case Qt::Key_Down: transpose((ctrl || (e->modifiers() & Qt::ShiftModifier)) ? -12 : -1); return;
    case Qt::Key_Left: case Qt::Key_Right: { if (m_selected.empty()) break; Tick dt = (e->key() == Qt::Key_Left ? -1 : 1) * (ctrl ? PPQ / 32 : snapTicks()); beginEdit(); Tick minS = LLONG_MAX; for (auto& n : d->notes) if (m_selected.count(n.id)) minS = std::min(minS, n.start); if (minS + dt < 0) dt = -minS; for (auto& n : d->notes) if (m_selected.count(n.id)) n.start += dt; commitEdit("Move notes"); return; }
    case Qt::Key_Escape: m_selected.clear(); invalidate(); return;
    case Qt::Key_1: setTool(Draw); Q_EMIT toolChanged(Draw); return; case Qt::Key_2: setTool(Select); Q_EMIT toolChanged(Select); return; case Qt::Key_3: setTool(Erase); Q_EMIT toolChanged(Erase); return;
    default: break;
    }
    QWidget::keyPressEvent(e);
}
void PianoRollView::keyReleaseEvent(QKeyEvent* e) { QWidget::keyReleaseEvent(e); }
void PianoRollView::contextMenuEvent(QContextMenuEvent* e) { e->accept(); }   // menus open on right-button release (right-drag erases)
void PianoRollView::showAreaMenu(const QPoint& pos, const QPoint& globalPos) {
    Layout L = layout(); if (!L.grid.contains(pos)) return; QContextMenuEvent ev(QContextMenuEvent::Mouse, pos, globalPos); QContextMenuEvent* e = &ev; QMenu m(this); m.addAction("Select all", [this] { selectAll(); }); m.addAction("Quantize (Ctrl+Q)", [this] { quantize(false); }); m.addAction("Quantize starts and lengths", [this] { quantize(true); }); m.addSeparator();
    m.addAction("Arpeggiate", [this] { arpeggiate(); }); m.addAction("Strum", [this] { strum(); }); m.addAction("Legato (Ctrl+L)", [this] { legato(); }); m.addAction("Randomize...", [this] { bool ok; int a = QInputDialog::getInt(this, "Randomize", "Velocity / pan amount (%):", 20, 0, 100, 1, &ok); if (ok) randomize(a / 100.f, a / 100.f); });
    m.addAction("Transpose +12", [this] { transpose(12); }); m.addAction("Transpose -12", [this] { transpose(-12); }); m.addSeparator(); m.addAction("Zoom to fit", [this] { zoomToFit(); });
    m.exec(e->globalPos());
}

// ---------------------------------------------------------------- tools
void PianoRollView::selectAll() { PatternChannelData* d = data(); if (!d) return; for (auto& n : d->notes) m_selected.insert(n.id); invalidate(); }
void PianoRollView::deleteSelected() { PatternChannelData* d = data(); if (!d || m_selected.empty()) return; beginEdit(); std::erase_if(d->notes, [this](const Note& n) { return m_selected.count(n.id) > 0; }); m_selected.clear(); commitEdit("Delete notes"); }
void PianoRollView::copySelected() { PatternChannelData* d = data(); if (!d) return; m_clipboard.clear(); Tick minS = LLONG_MAX; for (auto& n : d->notes) if (m_selected.count(n.id)) { m_clipboard.push_back(n); minS = std::min(minS, n.start); } for (auto& n : m_clipboard) n.start -= minS; }
void PianoRollView::cutSelected() { copySelected(); deleteSelected(); }
void PianoRollView::paste() { PatternChannelData* d = data(); if (!d || m_clipboard.empty()) return; beginEdit(); Tick at = m_playTick >= 0 ? snap((Tick)m_playTick, true) : (m_hoverPos.x() > 0 ? snap(xToTick(m_hoverPos.x()), true) : m_scrollTick); m_selected.clear(); for (auto n : m_clipboard) { n.id = m_app.project().newId(); n.start += at; d->notes.push_back(n); m_selected.insert(n.id); } commitEdit("Paste notes"); }
void PianoRollView::duplicateSelected() { PatternChannelData* d = data(); if (!d || m_selected.empty()) return; beginEdit(); Tick minS = LLONG_MAX, maxE = 0; for (auto& n : d->notes) if (m_selected.count(n.id)) { minS = std::min(minS, n.start); maxE = std::max(maxE, n.start + n.duration); } Tick span = snap(maxE - minS + snapTicks() - 1, true); if (span <= 0) span = maxE - minS; std::vector<Note> copies; for (auto& n : d->notes) if (m_selected.count(n.id)) { Note c = n; c.id = m_app.project().newId(); c.start += span; copies.push_back(c); } m_selected.clear(); for (auto& c : copies) { d->notes.push_back(c); m_selected.insert(c.id); } commitEdit("Duplicate notes"); }
void PianoRollView::quantize(bool lengths) { PatternChannelData* d = data(); if (!d) return; beginEdit(); Tick s = snapTicks(); if (s <= 1) s = PPQ / 4; bool any = !m_selected.empty(); for (auto& n : d->notes) { if (any && !m_selected.count(n.id)) continue; n.start = ((n.start + s / 2) / s) * s; if (lengths) n.duration = std::max<Tick>(s, ((n.duration + s / 2) / s) * s); } commitEdit("Quantize"); }
void PianoRollView::transpose(int semis) { PatternChannelData* d = data(); if (!d || m_selected.empty()) return; beginEdit(); for (auto& n : d->notes) if (m_selected.count(n.id)) n.pitch = clamp(n.pitch + semis, 0, 127); commitEdit("Transpose"); }
void PianoRollView::legato() { PatternChannelData* d = data(); if (!d) return; beginEdit(); std::vector<Note*> sel; for (auto& n : d->notes) if (m_selected.empty() || m_selected.count(n.id)) sel.push_back(&n); std::sort(sel.begin(), sel.end(), [](Note* a, Note* b) { return a->start < b->start; }); for (size_t i = 0; i + 1 < sel.size(); ++i) { Tick next = sel[i + 1]->start; if (next > sel[i]->start) sel[i]->duration = next - sel[i]->start; } commitEdit("Legato"); }
void PianoRollView::strum() { PatternChannelData* d = data(); if (!d || m_selected.size() < 2) return; beginEdit(); std::vector<Note*> sel; for (auto& n : d->notes) if (m_selected.count(n.id)) sel.push_back(&n); std::sort(sel.begin(), sel.end(), [](Note* a, Note* b) { return a->pitch < b->pitch; }); Tick step = PPQ / 16; Tick base = sel.front()->start; for (size_t i = 0; i < sel.size(); ++i) { Tick off = (Tick)i * step; Tick end = sel[i]->start + sel[i]->duration; sel[i]->start = base + off; sel[i]->duration = std::max<Tick>(PPQ / 32, end - sel[i]->start); } commitEdit("Strum"); }
void PianoRollView::arpeggiate() { PatternChannelData* d = data(); if (!d || m_selected.size() < 2) return; beginEdit(); std::vector<Note> sel; for (auto& n : d->notes) if (m_selected.count(n.id)) sel.push_back(n); std::sort(sel.begin(), sel.end(), [](const Note& a, const Note& b) { return a.pitch < b.pitch; }); Tick start = LLONG_MAX, end = 0; for (auto& n : sel) { start = std::min(start, n.start); end = std::max(end, n.start + n.duration); } Tick rate = snapTicks() > 1 ? snapTicks() : PPQ / 4; std::erase_if(d->notes, [this](const Note& n) { return m_selected.count(n.id) > 0; }); m_selected.clear(); size_t k = 0; for (Tick t = start; t + rate <= end; t += rate, ++k) { Note n = sel[k % sel.size()]; n.id = m_app.project().newId(); n.start = t; n.duration = rate; d->notes.push_back(n); m_selected.insert(n.id); } commitEdit("Arpeggiate"); }
void PianoRollView::randomize(float velAmt, float panAmt) { PatternChannelData* d = data(); if (!d) return; beginEdit(); dsp::Rng r; r.s = (uint32_t)QDateTime::currentMSecsSinceEpoch(); for (auto& n : d->notes) { if (!m_selected.empty() && !m_selected.count(n.id)) continue; n.velocity = clamp(n.velocity + r.bipolar() * velAmt, 0.05f, 1.f); n.pan = clamp(n.pan + r.bipolar() * panAmt, -1.f, 1.f); } commitEdit("Randomize"); }
void PianoRollView::setSelectedLength(Tick len) { PatternChannelData* d = data(); if (!d) return; beginEdit(); for (auto& n : d->notes) if (m_selected.count(n.id)) n.duration = len; commitEdit("Note length"); }

// ---------------------------------------------------------------- panel
PianoRollPanel::PianoRollPanel(App& app, QWidget* parent) : QWidget(parent), m_app(app) {
    auto* lay = new QVBoxLayout(this); lay->setContentsMargins(2, 2, 2, 2); lay->setSpacing(2); auto* top = new QHBoxLayout(); top->setSpacing(4);
    auto mkTool = [&](const QString& t, const QString& tip) { auto* b = new QToolButton(this); b->setText(t); b->setToolTip(tip); b->setCheckable(true); top->addWidget(b); return b; };
    m_draw = mkTool("✎", "Draw tool (1): click to add notes, drag to move, drag right edge to resize, right-click to erase"); m_select = mkTool("⬚", "Select tool (2): drag a rectangle to select notes"); m_erase = mkTool("✖", "Erase tool (3)"); m_draw->setChecked(true);
    top->addSpacing(8); auto* l = new QLabel("Channel", this); l->setStyleSheet("color:#8a919a;"); top->addWidget(l); m_channel = new QComboBox(this); m_channel->setMinimumWidth(120); top->addWidget(m_channel);
    auto* l2 = new QLabel("Snap", this); l2->setStyleSheet("color:#8a919a;"); top->addWidget(l2); m_snap = TransportBar::makeSnapCombo(this); top->addWidget(m_snap);
    auto* l3 = new QLabel("Scale", this); l3->setStyleSheet("color:#8a919a;"); top->addWidget(l3); m_root = new QComboBox(this); for (int i = 0; i < 12; ++i) m_root->addItem(theme::noteName(i).left(theme::noteName(i).size() - 2)); top->addWidget(m_root); m_scale = new QComboBox(this); m_scale->addItems(PianoRollView::scaleNames()); top->addWidget(m_scale);
    auto* l4 = new QLabel("Chord", this); l4->setStyleSheet("color:#8a919a;"); top->addWidget(l4); m_chord = new QComboBox(this); m_chord->addItems(PianoRollView::chordNames()); m_chord->setToolTip("Chord helper: drawing a note adds the chord intervals"); top->addWidget(m_chord);
    m_ghost = mkTool("Ghost", "Show notes of other channels"); m_ghost->setChecked(true); m_repeat = mkTool("Repeat", "Note repeat: holding a key on the keyboard retriggers at the snap rate");
    auto* tools = new QToolButton(this); tools->setText("Tools ▾"); tools->setPopupMode(QToolButton::InstantPopup); auto* tm = new QMenu(tools);
    tm->addAction("Quantize starts\tCtrl+Q", [this] { m_view->quantize(false); }); tm->addAction("Quantize starts + lengths", [this] { m_view->quantize(true); }); tm->addSeparator(); tm->addAction("Arpeggiate selection", [this] { m_view->arpeggiate(); }); tm->addAction("Strum selection", [this] { m_view->strum(); }); tm->addAction("Legato\tCtrl+L", [this] { m_view->legato(); }); tm->addAction("Randomize velocity/pan...", [this] { bool ok; int a = QInputDialog::getInt(this, "Randomize", "Amount (%):", 20, 0, 100, 1, &ok); if (ok) m_view->randomize(a / 100.f, a / 100.f); });
    tm->addSeparator(); tm->addAction("Transpose +1", [this] { m_view->transpose(1); }); tm->addAction("Transpose -1", [this] { m_view->transpose(-1); }); tm->addAction("Transpose +12", [this] { m_view->transpose(12); }); tm->addAction("Transpose -12", [this] { m_view->transpose(-12); }); tm->addSeparator();
    tm->addAction("Select all\tCtrl+A", [this] { m_view->selectAll(); }); tm->addAction("Duplicate\tCtrl+D", [this] { m_view->duplicateSelected(); }); tm->addAction("Copy\tCtrl+C", [this] { m_view->copySelected(); }); tm->addAction("Paste\tCtrl+V", [this] { m_view->paste(); }); tm->addAction("Delete\tDel", [this] { m_view->deleteSelected(); }); tm->addSeparator(); tm->addAction("Zoom to fit", [this] { m_view->zoomToFit(); });
    tools->setMenu(tm); top->addWidget(tools); top->addStretch(1); lay->addLayout(top);
    m_view = new PianoRollView(app, this); lay->addWidget(m_view, 1);
    connect(m_view, &PianoRollView::toolChanged, this, [this](int t) { m_draw->setChecked(t == 0); m_select->setChecked(t == 1); m_erase->setChecked(t == 2); });
    connect(m_draw, &QToolButton::clicked, this, [this] { m_view->setTool(PianoRollView::Draw); m_draw->setChecked(true); m_select->setChecked(false); m_erase->setChecked(false); });
    connect(m_select, &QToolButton::clicked, this, [this] { m_view->setTool(PianoRollView::Select); m_draw->setChecked(false); m_select->setChecked(true); m_erase->setChecked(false); });
    connect(m_erase, &QToolButton::clicked, this, [this] { m_view->setTool(PianoRollView::Erase); m_draw->setChecked(false); m_select->setChecked(false); m_erase->setChecked(true); });
    connect(m_channel, &QComboBox::currentIndexChanged, this, [this](int i) { if (!m_updating && i >= 0) { m_view->setChannel((Id)m_channel->itemData(i).toULongLong()); } });
    connect(m_snap, &QComboBox::currentIndexChanged, this, [this](int i) { m_view->setSnapDivision(TransportBar::snapDivisionFromIndex(i)); m_view->refresh(); });
    connect(m_scale, &QComboBox::currentIndexChanged, this, [this](int) { m_view->setScale(m_root->currentIndex(), m_scale->currentIndex()); });
    connect(m_root, &QComboBox::currentIndexChanged, this, [this](int) { m_view->setScale(m_root->currentIndex(), m_scale->currentIndex()); });
    connect(m_chord, &QComboBox::currentIndexChanged, this, [this](int i) { m_view->setChord(i); });
    connect(m_ghost, &QToolButton::toggled, this, [this](bool b) { m_view->setGhost(b); }); connect(m_repeat, &QToolButton::toggled, this, [this](bool b) { m_view->setNoteRepeat(b); });
    refresh(ChangeAll);
}
void PianoRollPanel::showChannel(Id id) { for (int i = 0; i < m_channel->count(); ++i) if ((Id)m_channel->itemData(i).toULongLong() == id) { m_channel->setCurrentIndex(i); break; } m_view->setChannel(id); m_view->zoomToFit(); }
void PianoRollPanel::refresh(uint32_t flags) {
    if (flags & (ChangeChannels | ChangeStructure | ChangeAll)) { m_updating = true; Id cur = m_view->channel(); m_channel->clear(); for (auto& c : m_app.project().channels) { m_channel->addItem(c.name, (qulonglong)c.id); m_channel->setItemData(m_channel->count() - 1, c.color, Qt::DecorationRole); } bool found = false; for (int i = 0; i < m_channel->count(); ++i) if ((Id)m_channel->itemData(i).toULongLong() == cur) { m_channel->setCurrentIndex(i); found = true; } if (!found && m_channel->count()) { m_channel->setCurrentIndex(0); m_view->setChannel((Id)m_channel->itemData(0).toULongLong()); } m_updating = false; }
    m_view->refresh();
}
void PianoRollPanel::uiTick() { m_view->uiTick(); }

} // namespace mt
