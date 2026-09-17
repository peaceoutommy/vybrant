#include "ui/ChannelRackPanel.h"
#include "ui/Theme.h"
#include "dsp/NativeRegistry.h"
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMenu>
#include <QInputDialog>
#include <QColorDialog>
#include <QKeyEvent>
#include <QMessageBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QUrl>
#include <QToolTip>

namespace mt {

// ---------------------------------------------------------------- RackView
RackView::RackView(App& app, QWidget* parent) : QWidget(parent), m_app(app) { setAcceptDrops(true); setMouseTracking(true); setFocusPolicy(Qt::StrongFocus); }
Pattern* RackView::pattern() const { return m_app.project().pattern(m_app.project().selectedPatternId); }
const Step* RackView::stepAt(Id ch, int step) const { Pattern* p = pattern(); if (!p) return nullptr; auto* d = p->channelData(ch); if (!d || step < 0 || step >= (int)d->steps.size()) return nullptr; return &d->steps[step]; }
int RackView::stepsPerBar() const { Pattern* p = pattern(); const auto& ts = m_app.project().transport.timeSig; return std::max(1, (int)(ts.ticksPerBar() / (p ? p->stepTicks() : PPQ / 4))); }
int RackView::patternSteps() const { Pattern* p = pattern(); if (!p) return 16; return (int)(p->effectiveLength(m_app.project().transport.timeSig) / p->stepTicks()); }
// The rack shows the whole pattern plus enough extra (dimmed) steps to fill the window; only the pattern's own steps play
int RackView::visibleSteps() const { return std::max(patternSteps(), m_fitSteps); }
int RackView::stepW() const { return 23; }
int RackView::stepX(int step) const { return m_leftW + 6 + step * stepW() + (step / 4) * 4; }
QRect RackView::rowRect(int row) const { return QRect(0, row * m_rowH, width(), m_rowH); }
QSize RackView::sizeHint() const { int steps = visibleSteps(); int rows = (int)m_app.project().channels.size(); return QSize(stepX(steps) + 12, rows * m_rowH + (m_graphMode >= 0 ? m_graphH : 0) + 4); }
void RackView::refresh() { for (auto it = m_multi.begin(); it != m_multi.end();) { if (!m_app.project().channel(*it)) it = m_multi.erase(it); else ++it; } updateGeometry(); resize(sizeHint()); update(); }
void RackView::uiTick() { auto tv = m_app.engine().transport(); int ps = -1; Pattern* p = pattern(); if (p && tv.playing && !tv.countingIn) { Tick local = (Tick)tv.positionTicks; if (tv.songMode) { local = -1; for (auto& c : m_app.project().clips) if (c.type == ClipType::Pattern && c.sourceId == p->id && tv.positionTicks >= c.start && tv.positionTicks < c.start + c.length) { local = ((Tick)tv.positionTicks - c.start + c.sourceOffset) % std::max<Tick>(p->effectiveLength(m_app.project().transport.timeSig), 1); break; } } if (local >= 0) ps = (int)(local / p->stepTicks()); } if (ps != m_playStep) { m_playStep = ps; update(); } }

RackView::HitInfo RackView::hitTest(const QPoint& pt) const {
    HitInfo h; int rows = (int)m_app.project().channels.size(); int row = pt.y() / m_rowH; if (row < 0 || row >= rows) { if (m_graphMode >= 0 && pt.y() >= rows * m_rowH && pt.y() < rows * m_rowH + m_graphH) { h.hit = Graph; int steps = visibleSteps(); for (int s = 0; s < steps; ++s) if (pt.x() >= stepX(s) && pt.x() < stepX(s) + stepW()) h.step = s; } return h; }
    h.row = row; int x = pt.x();
    if (x < 12) h.hit = Grip; else if (x < 28) h.hit = Enable; else if (x < 134) h.hit = Name; else if (x < 152) h.hit = Mute; else if (x < 170) h.hit = Solo; else if (x < 196) h.hit = Pan; else if (x < 222) h.hit = Vol; else if (x < m_leftW) h.hit = Mixer;
    else { int steps = visibleSteps(); for (int s = 0; s < steps; ++s) if (x >= stepX(s) && x < stepX(s) + stepW()) { h.hit = StepCell; h.step = s; break; } }
    return h;
}

void RackView::paintEvent(QPaintEvent*) {
    QPainter p(this); p.fillRect(rect(), theme::bg()); p.setRenderHint(QPainter::Antialiasing);
    Pattern* pat = pattern(); int steps = visibleSteps(); int patSteps = patternSteps(); auto& proj = m_app.project();
    const auto& chans = proj.channels;
    for (int r = 0; r < (int)chans.size(); ++r) {
        const Channel& c = chans[r]; QRect rr = rowRect(r); bool selected = c.id == proj.selectedChannelId;
        bool multi = m_multi.count(c.id) > 0; p.fillRect(rr.adjusted(0, 0, 0, -1), (selected || multi) ? QColor(0x27, 0x2c, 0x33) : (r % 2 ? QColor(0x1a, 0x1d, 0x22) : QColor(0x18, 0x1b, 0x20))); if (multi) { p.fillRect(QRect(0, rr.top(), 2, rr.height() - 1), theme::accent()); }
        if (m_reorderTarget == r && m_dragging && m_press.hit == Grip) { p.setPen(QPen(theme::accent(), 2)); p.drawLine(rr.left(), rr.top(), rr.right(), rr.top()); }
        p.setPen(QColor(0x3a, 0x40, 0x48)); for (int i = 0; i < 3; ++i) p.drawLine(4, rr.top() + 10 + i * 4, 8, rr.top() + 10 + i * 4);
        QRectF led(15, rr.top() + 10, 8, 8); p.setPen(Qt::NoPen); p.setBrush(c.enabled ? theme::green() : QColor(0x2b, 0x30, 0x37)); p.drawEllipse(led);
        // name tag: flat panel with a colour bar, accent outline when selected
        QRectF nb(30, rr.top() + 3, 102, m_rowH - 6); p.setPen(Qt::NoPen); p.setBrush(selected ? QColor(0x2c, 0x31, 0x39) : QColor(0x23, 0x27, 0x2d)); p.drawRoundedRect(nb, 3, 3); p.setBrush(c.color); p.drawRoundedRect(QRectF(nb.left(), nb.top(), 4, nb.height()), 2, 2);
        if (selected) { p.setPen(QPen(theme::accent(), 1)); p.setBrush(Qt::NoBrush); p.drawRoundedRect(nb, 3, 3); }
        p.setPen(c.enabled ? theme::text() : theme::textDim()); p.setFont(theme::uiFont()); p.drawText(nb.adjusted(8, 0, -3, 0), Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(c.name, Qt::ElideRight, (int)nb.width() - 11));
        auto smallBtn = [&](QRectF r, const QString& t, bool on, QColor col) { p.setPen(QPen(on ? col.darker(120) : theme::border(), 1)); p.setBrush(on ? col : QColor(0x1c, 0x1f, 0x24)); p.drawRoundedRect(r, 2, 2); p.setPen(on ? theme::bg() : theme::textDim()); p.setFont(theme::smallFont()); p.drawText(r, Qt::AlignCenter, t); };
        smallBtn(QRectF(136, rr.top() + 6, 15, 16), "M", c.mute, theme::red()); smallBtn(QRectF(154, rr.top() + 6, 15, 16), "S", c.solo, theme::accent());
        auto knob = [&](QRectF r, float v, bool bipolar) { p.setPen(QPen(theme::border(), 1)); p.setBrush(QColor(0x1e, 0x21, 0x26)); p.drawEllipse(r); QRectF a = r.adjusted(1.5, 1.5, -1.5, -1.5); p.setPen(QPen(theme::accent2(), 1.5, Qt::SolidLine, Qt::RoundCap)); if (bipolar) p.drawArc(a, 90 * 16, (int)(-270 * (v - 0.5) * 16)); else p.drawArc(a, 225 * 16, (int)(-270 * v * 16)); };
        float pan = m_app.engine().registry().channelIfExists(c.id) ? m_app.engine().registry().channelIfExists(c.id)->pan.load() : c.pan; float vol = m_app.engine().registry().channelIfExists(c.id) ? m_app.engine().registry().channelIfExists(c.id)->volume.load() : c.volume;
        knob(QRectF(174, rr.top() + 4, 20, 20), (pan + 1) * 0.5f, true); knob(QRectF(200, rr.top() + 4, 20, 20), vol, false);
        QRectF mb(226, rr.top() + 5, 28, m_rowH - 10); p.setPen(QPen(theme::border(), 1)); p.setBrush(QColor(0x11, 0x13, 0x16)); p.drawRoundedRect(mb, 2, 2); int mi = proj.mixerIndex(c.mixerTrackId); p.setPen(theme::textDim()); p.setFont(theme::smallFont()); p.drawText(mb, Qt::AlignCenter, mi <= 0 ? "M" : QString::number(mi));
        const PatternChannelData* d = pat ? pat->channelData(c.id) : nullptr;
        if (d && !d->notes.empty()) {   // channel has piano-roll notes: show them instead of the step buttons
            QRectF area(stepX(0), rr.top() + 3, stepX(steps) - stepX(0) - 2, m_rowH - 6); p.setPen(QPen(theme::border(), 1)); p.setBrush(QColor(0x11, 0x13, 0x16)); p.drawRoundedRect(area, 3, 3);
            Tick plen = std::max<Tick>(pat->effectiveLength(proj.transport.timeSig), 1); int lo = 127, hi = 0; for (auto& n : d->notes) { lo = std::min(lo, n.pitch); hi = std::max(hi, n.pitch); } int range = std::max(hi - lo + 1, 5);
            p.setClipRect(area.adjusted(1, 1, -1, -1)); Tick bar = proj.transport.timeSig.ticksPerBar(); p.setPen(theme::grid()); for (Tick t = bar; t < plen; t += bar) { double x = area.left() + (double)t / plen * area.width(); p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom())); }
            for (auto& n : d->notes) { double x1 = area.left() + (double)n.start / plen * area.width(), x2 = area.left() + (double)(n.start + n.duration) / plen * area.width(); double y = area.bottom() - 2 - (double)(n.pitch - lo + 0.5) / range * (area.height() - 4); p.setPen(QPen(c.color.lighter(120), 2)); p.drawLine(QPointF(x1, y), QPointF(std::max(x2, x1 + 2), y)); }
            if (m_playStep >= 0 && pat) { double x = area.left() + (double)(m_playStep * pat->stepTicks()) / plen * area.width(); p.setPen(theme::playhead()); p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom())); }
            p.setClipping(false); p.setPen(theme::textDim()); p.setFont(theme::smallFont()); p.drawText(QRect(stepX(steps) + 2, rr.top(), 60, m_rowH), Qt::AlignVCenter, QString("\u266A %1").arg(d->notes.size()));
            continue;
        }
        for (int s = 0; s < steps; ++s) {
            QRectF sr(stepX(s), rr.top() + 4, stepW() - 2, m_rowH - 8); bool beatA = (s / 4) % 2 == 0; const Step* st = (d && s < (int)d->steps.size()) ? &d->steps[s] : nullptr; bool on = st && st->enabled;
            QColor base = beatA ? QColor(0x2a, 0x2f, 0x37) : QColor(0x23, 0x27, 0x2d); if (s >= patSteps) base = base.darker(150); if (m_playStep == s) base = base.lighter(140);
            p.setPen(Qt::NoPen); p.setBrush(on ? c.color.lighter(st->velocity > 0.9f ? 115 : (int)(70 + st->velocity * 50)) : base); p.drawRoundedRect(sr, 2.5, 2.5);
            if (on) { p.setPen(QPen(theme::withAlpha(Qt::white, 40), 1)); p.setBrush(Qt::NoBrush); p.drawLine(QPointF(sr.left() + 2, sr.top() + 1), QPointF(sr.right() - 2, sr.top() + 1)); }
            if (on && st->probability < 1.f) { p.setPen(QPen(theme::bg(), 1, Qt::DashLine)); p.setBrush(Qt::NoBrush); p.drawRoundedRect(sr.adjusted(1, 1, -1, -1), 2, 2); }
            if (on && std::fabs(st->pitch) > 0.01f) { p.setPen(theme::bg()); p.setFont(theme::smallFont()); p.drawText(sr, Qt::AlignCenter, QString::number((int)st->pitch)); }
        }
    }
    // graph editor
    if (m_graphMode >= 0) {
        int top = (int)chans.size() * m_rowH; QRect g(0, top, width(), m_graphH); p.fillRect(g, QColor(0x15, 0x18, 0x1c)); p.setPen(theme::border()); p.drawLine(0, top, width(), top);
        static const char* names[] = {"Velocity", "Pan", "Pitch", "Probability", "Shift"}; p.setPen(theme::textDim()); p.setFont(theme::smallFont()); p.drawText(QRect(6, top + 4, m_leftW, 16), Qt::AlignLeft, QString("Graph: %1  (selected channel; drag bars, right-click resets)").arg(names[m_graphMode]));
        const Channel* sel = proj.channel(proj.selectedChannelId); const PatternChannelData* d = (pat && sel) ? pat->channelData(sel->id) : nullptr;
        bool bipolar = m_graphMode == 1 || m_graphMode == 2 || m_graphMode == 4; int mid = top + m_graphH / 2; int bottom = top + m_graphH - 4;
        if (bipolar) { p.setPen(theme::gridStrong()); p.drawLine(m_leftW, mid, width(), mid); }
        for (int s = 0; s < steps; ++s) {
            const Step* st = (d && s < (int)d->steps.size()) ? &d->steps[s] : nullptr; float v = 0; if (st) switch (m_graphMode) { case 0: v = st->velocity; break; case 1: v = st->pan; break; case 2: v = st->pitch / 24.f; break; case 3: v = st->probability; break; case 4: v = st->offset * 2.f; break; }
            QRectF bar; if (bipolar) { int h = (int)(v * (m_graphH / 2 - 8)); bar = QRectF(stepX(s), h >= 0 ? mid - h : mid, stepW() - 2, std::abs(h)); } else { int h = (int)(v * (m_graphH - 24)); bar = QRectF(stepX(s), bottom - h, stepW() - 2, h); }
            p.setPen(Qt::NoPen); p.setBrush(st && st->enabled ? (sel ? sel->color : theme::accent()) : theme::gridStrong()); p.drawRect(bar); p.setPen(theme::border()); p.drawLine(stepX(s), bottom, stepX(s) + stepW() - 2, bottom);
        }
    }
}

void RackView::mousePressEvent(QMouseEvent* e) {
    m_press = hitTest(e->pos()); m_lastPos = e->pos(); m_dragging = true; auto& proj = m_app.project(); setFocus();
    if (m_press.row >= 0 && m_press.row < (int)proj.channels.size()) {
        const Channel& c = proj.channels[m_press.row]; bool nameArea = m_press.hit == Name || m_press.hit == Grip || m_press.hit == Enable;
        if (nameArea && (e->modifiers() & Qt::ControlModifier)) { if (m_multi.count(c.id)) m_multi.erase(c.id); else m_multi.insert(c.id); if (m_multi.count(c.id)) m_app.selectChannel(c.id); update(); if (m_press.hit == Enable) { m_dragging = false; return; } }
        else if (nameArea && (e->modifiers() & Qt::ShiftModifier)) { int a = -1, b = m_press.row; for (size_t i = 0; i < proj.channels.size(); ++i) if (proj.channels[i].id == m_anchor) a = (int)i; if (a < 0) a = b; for (int i = std::min(a, b); i <= std::max(a, b); ++i) m_multi.insert(proj.channels[i].id); m_app.selectChannel(c.id); update(); return; }
        else { if (!m_multi.count(c.id) || !nameArea) { m_multi.clear(); } m_anchor = c.id; m_app.selectChannel(c.id); }
    }
    if (m_press.hit == Grip) { m_dragRow = m_press.row; m_reorderTarget = m_press.row; return; }
    if (m_press.hit == Graph) { if (e->button() == Qt::RightButton) { Pattern* pat = pattern(); const Channel* sel = proj.channel(proj.selectedChannelId); if (pat && sel && m_press.step >= 0) m_app.setStepProperty(pat->id, sel->id, m_press.step, m_graphMode, m_graphMode == 0 ? 0.8f : (m_graphMode == 3 ? 1.f : 0.f), true); } else applyGraphAt(e->pos(), false); return; }
    if (m_press.row < 0) return;
    const Channel& c = proj.channels[m_press.row];
    switch (m_press.hit) {
    case Enable: m_app.setChannelEnabled(c.id, !c.enabled); break;
    case Mute: if (e->button() == Qt::LeftButton) m_app.setChannelMute(c.id, !c.mute); break;
    case Solo: m_app.setChannelSolo(c.id, !c.solo); break;
    case Pan: m_knobStart = c.pan; if (e->button() == Qt::RightButton) { QMenu m; m.addAction("Create automation clip", [this, id = c.id] { m_app.createAutomationLane({ParamTarget::ChannelPan, id, 0, 0}, QString()); }); m.addAction("Link to MIDI controller...", [this, id = c.id, n = c.name] { m_app.startMidiLearn({ParamTarget::ChannelPan, id, 0, 0}, n + " Pan"); }); m.addAction("Reset", [this, id = c.id] { m_app.setChannelPan(id, 0, true); }); m.exec(e->globalPosition().toPoint()); m_dragging = false; } break;
    case Vol: m_knobStart = c.volume; if (e->button() == Qt::RightButton) { QMenu m; m.addAction("Create automation clip", [this, id = c.id] { m_app.createAutomationLane({ParamTarget::ChannelVolume, id, 0, 0}, QString()); }); m.addAction("Link to MIDI controller...", [this, id = c.id, n = c.name] { m_app.startMidiLearn({ParamTarget::ChannelVolume, id, 0, 0}, n + " Volume"); }); m.addAction("Reset", [this, id = c.id] { m_app.setChannelVolume(id, 0.8f, true); }); m.exec(e->globalPosition().toPoint()); m_dragging = false; } break;
    case Mixer: { if (e->button() == Qt::RightButton) { QMenu m; for (auto& t : proj.mixerTracks) { auto* a = m.addAction(t.name, [this, id = c.id, tid = t.id] { m_app.setChannelMixerTrack(id, tid); }); a->setCheckable(true); a->setChecked(t.id == c.mixerTrackId); } m.exec(e->globalPosition().toPoint()); m_dragging = false; } break; }
    case StepCell: { Pattern* pat = pattern(); if (!pat) break; if (auto* pd = pat->channelData(c.id); pd && !pd->notes.empty()) { if (e->button() == Qt::LeftButton) Q_EMIT openPianoRoll(c.id); else { QMenu m; m.addAction("Open in piano roll", [this, id = c.id] { Q_EMIT openPianoRoll(id); }); m.addAction("Clear (steps and notes)", [this, id = c.id, pid = pat->id] { m_app.editPattern(pid, "Clear channel in pattern", [id](Pattern& pp) { auto& d = pp.ensureChannel(id); for (auto& s : d.steps) s.enabled = false; d.notes.clear(); }); }); m.exec(e->globalPosition().toPoint()); } m_dragging = false; break; } const Step* st = stepAt(c.id, m_press.step); bool on = st && st->enabled; if (e->button() == Qt::RightButton) { m_erasing = true; if (on) m_app.setStep(pat->id, c.id, m_press.step, false); } else if (e->modifiers() & Qt::ShiftModifier) { /* shift+click: audition */ m_app.noteOn(c.rootNote); m_app.noteOff(c.rootNote); } else { m_erasing = false; m_paintValue = !on; m_app.setStep(pat->id, c.id, m_press.step, m_paintValue); } break; }
    case Name: if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::AltModifier)) Q_EMIT openPianoRoll(c.id); break;
    default: break;
    }
}
void RackView::mouseMoveEvent(QMouseEvent* e) {
    if (!m_dragging) { HitInfo h = hitTest(e->pos()); if (h.hit == StepCell) { auto& proj = m_app.project(); if (h.row >= 0) { const Step* st = stepAt(proj.channels[h.row].id, h.step); if (st && st->enabled) QToolTip::showText(e->globalPosition().toPoint(), QString("Step %1  vel %2  pan %3  pitch %4  prob %5%  shift %6").arg(h.step + 1).arg(st->velocity, 0, 'f', 2).arg(st->pan, 0, 'f', 2).arg(st->pitch, 0, 'f', 0).arg((int)(st->probability * 100)).arg(st->offset, 0, 'f', 2), this); } } return; }
    auto& proj = m_app.project(); int dy = m_lastPos.y() - e->pos().y();
    if (m_press.hit == Grip) { int row = clamp(e->pos().y() / m_rowH, 0, (int)proj.channels.size() - 1); m_reorderTarget = row; update(); return; }
    if (m_press.hit == Graph) { applyGraphAt(e->pos(), false); return; }
    if (m_press.row < 0 || m_press.row >= (int)proj.channels.size()) return;
    const Channel& c = proj.channels[m_press.row]; float sens = (e->modifiers() & Qt::ShiftModifier) ? 0.001f : 0.01f;
    if (m_press.hit == Pan) { float v = clamp(c.pan + dy * sens * 2, -1.f, 1.f); m_app.setChannelPan(c.id, v, false); update(); QToolTip::showText(e->globalPosition().toPoint(), QString("Pan %1").arg(v, 0, 'f', 2), this); }
    else if (m_press.hit == Vol) { float v = clamp(c.volume + dy * sens, 0.f, 1.f); m_app.setChannelVolume(c.id, v, false); update(); QToolTip::showText(e->globalPosition().toPoint(), QString("Volume %1 dB").arg(gainToDb(v / 0.8f), 0, 'f', 1), this); }
    else if (m_press.hit == Mixer) { if (std::abs(dy) >= 4) { int mi = proj.mixerIndex(c.mixerTrackId); mi = clamp(mi + (dy > 0 ? 1 : -1), 0, (int)proj.mixerTracks.size() - 1); m_app.setChannelMixerTrack(c.id, proj.mixerTracks[mi].id); m_lastPos = e->pos(); } return; }
    else if (m_press.hit == StepCell) { HitInfo h = hitTest(e->pos()); if (h.hit == StepCell && h.row >= 0) { Pattern* pat = pattern(); if (!pat) return; const Channel& rc = proj.channels[h.row]; const Step* st = stepAt(rc.id, h.step); bool on = st && st->enabled; if (m_erasing) { if (on) m_app.setStep(pat->id, rc.id, h.step, false); } else if (on != m_paintValue) m_app.setStep(pat->id, rc.id, h.step, m_paintValue); } }
    m_lastPos = e->pos();
}
void RackView::mouseReleaseEvent(QMouseEvent*) {
    if (!m_dragging) return; m_dragging = false; auto& proj = m_app.project();
    if (m_press.hit == Grip && m_dragRow >= 0 && m_reorderTarget != m_dragRow && m_dragRow < (int)proj.channels.size()) m_app.moveChannel(proj.channels[m_dragRow].id, m_reorderTarget);
    m_dragRow = -1; m_reorderTarget = -1;
    if (m_press.row >= 0 && m_press.row < (int)proj.channels.size()) { const Channel& c = proj.channels[m_press.row]; if (m_press.hit == Pan && std::fabs(c.pan - m_knobStart) > 1e-6f) { float v = c.pan; const_cast<Channel&>(c).pan = m_knobStart; m_app.setChannelPan(c.id, v, true); } if (m_press.hit == Vol && std::fabs(c.volume - m_knobStart) > 1e-6f) { float v = c.volume; const_cast<Channel&>(c).volume = m_knobStart; m_app.setChannelVolume(c.id, v, true); } }
    if (m_press.hit == Graph) applyGraphAt(m_lastPos, true);
    m_press = HitInfo(); update();
}
void RackView::applyGraphAt(const QPoint& pt, bool commit) {
    auto& proj = m_app.project(); Pattern* pat = pattern(); const Channel* sel = proj.channel(proj.selectedChannelId); if (!pat || !sel) return; m_lastPos = pt;
    HitInfo h = hitTest(pt); int step = h.step; if (step < 0) { int steps = visibleSteps(); for (int s = 0; s < steps; ++s) if (pt.x() < stepX(s) + stepW()) { step = s; break; } if (step < 0) step = steps - 1; }
    int top = (int)proj.channels.size() * m_rowH; int bottom = top + m_graphH - 4; int mid = top + m_graphH / 2; bool bipolar = m_graphMode == 1 || m_graphMode == 2 || m_graphMode == 4; float v;
    if (bipolar) v = clamp((float)(mid - pt.y()) / (m_graphH / 2 - 8), -1.f, 1.f); else v = clamp((float)(bottom - pt.y()) / (m_graphH - 24), 0.f, 1.f);
    float value = v; if (m_graphMode == 2) value = std::round(v * 24.f); if (m_graphMode == 4) value = v * 0.5f;
    if (!commit && !(stepAt(sel->id, step) && stepAt(sel->id, step)->enabled) && m_graphMode != 0) return;
    m_app.setStepProperty(pat->id, sel->id, step, m_graphMode, value, commit); update();
}
void RackView::mouseDoubleClickEvent(QMouseEvent* e) { HitInfo h = hitTest(e->pos()); auto& proj = m_app.project(); if (h.row < 0 || h.row >= (int)proj.channels.size()) return; const Channel& c = proj.channels[h.row]; if (h.hit == Name) Q_EMIT openEditor(c.id); if (h.hit == Pan) m_app.setChannelPan(c.id, 0, true); if (h.hit == Vol) m_app.setChannelVolume(c.id, 0.8f, true); }
void RackView::wheelEvent(QWheelEvent* e) {
    HitInfo h = hitTest(e->position().toPoint()); auto& proj = m_app.project(); if (h.row < 0 || h.row >= (int)proj.channels.size()) { e->ignore(); return; } const Channel& c = proj.channels[h.row]; int d = e->angleDelta().y() > 0 ? 1 : -1;
    if (h.hit == StepCell) { Pattern* pat = pattern(); const Step* st = stepAt(c.id, h.step); if (pat && st && st->enabled) { if (e->modifiers() & Qt::ControlModifier) m_app.setStepProperty(pat->id, c.id, h.step, 2, st->pitch + d, true); else m_app.setStepProperty(pat->id, c.id, h.step, 0, st->velocity + d * 0.05f, true); } e->accept(); return; }
    if (h.hit == Vol) { m_app.setChannelVolume(c.id, clamp(c.volume + d * 0.02f, 0.f, 1.f), true); e->accept(); return; }
    if (h.hit == Pan) { m_app.setChannelPan(c.id, clamp(c.pan + d * 0.05f, -1.f, 1.f), true); e->accept(); return; }
    if (h.hit == Mixer) { int mi = clamp(proj.mixerIndex(c.mixerTrackId) + d, 0, (int)proj.mixerTracks.size() - 1); m_app.setChannelMixerTrack(c.id, proj.mixerTracks[mi].id); e->accept(); return; }
    e->ignore();
}
void RackView::contextMenuEvent(QContextMenuEvent* e) { HitInfo h = hitTest(e->pos()); if (h.hit == Name || h.hit == Enable || h.hit == Grip) showChannelMenu(h.row, e->globalPos()); else if (h.row < 0 && h.hit == None) { QMenu m; auto* add = m.addMenu("Add channel"); for (auto* i : NativeRegistry::instance().byKind(ProcessorKind::Instrument)) add->addAction(i->name, [this, k = i->key] { m_app.addChannel(k == "native.drumsampler" ? ChannelType::DrumSampler : k == "native.sampler" ? ChannelType::Sampler : k == "native.midiout" ? ChannelType::MidiOut : k == "native.tonegen" ? ChannelType::AudioGenerator : ChannelType::Synth, k); }); m.exec(e->globalPos()); } }
std::vector<Id> RackView::selectedIds() const { std::vector<Id> ids; for (auto& c : m_app.project().channels) if (m_multi.count(c.id) || (m_multi.empty() && c.id == m_app.project().selectedChannelId)) ids.push_back(c.id); return ids; }
void RackView::deleteSelected() {
    auto ids = selectedIds(); if (ids.empty()) return;
    if (QMessageBox::question(this, "Delete channels", QString("Delete %1 channel%2 and their notes/steps in every pattern?").arg(ids.size()).arg(ids.size() > 1 ? "s" : ""), QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) return;
    for (Id id : ids) m_app.deleteChannel(id); m_multi.clear(); update();
}
void RackView::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) { deleteSelected(); return; }
    if (e->key() == Qt::Key_A && (e->modifiers() & Qt::ControlModifier)) { for (auto& c : m_app.project().channels) m_multi.insert(c.id); update(); return; }
    if (e->key() == Qt::Key_Escape) { m_multi.clear(); update(); return; }
    QWidget::keyPressEvent(e);
}
void RackView::showChannelMenu(int row, const QPoint& global) {
    auto& proj = m_app.project(); if (row < 0 || row >= (int)proj.channels.size()) return; Channel c = proj.channels[row]; Id id = c.id; QMenu m(this);
    if (m_multi.size() > 1 && m_multi.count(id)) {   // actions on the whole selection
        auto ids = selectedIds(); int n = (int)ids.size(); m.addSection(QString("%1 channels selected").arg(n));
        auto* route = m.addMenu("Route all to mixer track"); for (auto& t : proj.mixerTracks) route->addAction(t.name, [this, ids, tid = t.id] { for (Id i : ids) m_app.setChannelMixerTrack(i, tid); });
        m.addAction("Route each to its own free insert", [this, ids] { std::set<Id> used; for (auto& ch : m_app.project().channels) used.insert(ch.mixerTrackId); for (Id i : ids) for (auto& t : m_app.project().mixerTracks) if (!t.isMaster && !used.count(t.id)) { m_app.setChannelMixerTrack(i, t.id); used.insert(t.id); break; } });
        m.addAction("Mute all", [this, ids] { for (Id i : ids) m_app.setChannelMute(i, true); }); m.addAction("Unmute all", [this, ids] { for (Id i : ids) m_app.setChannelMute(i, false); }); m.addAction("Solo all", [this, ids] { for (Id i : ids) m_app.setChannelSolo(i, true); }); m.addAction("Unsolo all", [this, ids] { for (Id i : ids) m_app.setChannelSolo(i, false); });
        m.addAction("Color all...", [this, ids, c] { QColor col = QColorDialog::getColor(c.color, this, "Channel color"); if (col.isValid()) for (Id i : ids) m_app.setChannelColor(i, col); });
        m.addAction("Clear all (steps and notes)", [this, ids] { Pattern* p = pattern(); if (!p) return; m_app.editPattern(p->id, "Clear channels in pattern", [ids](Pattern& pp) { for (Id i : ids) { auto& d = pp.ensureChannel(i); for (auto& s : d.steps) s.enabled = false; d.notes.clear(); } }); });
        m.addAction("Duplicate all", [this, ids] { for (Id i : ids) m_app.duplicateChannel(i); }); m.addAction(QString("Delete %1 channels\tDel").arg(n), [this] { deleteSelected(); });
        m.addSeparator(); m.addAction("Clear selection\tEsc", [this] { m_multi.clear(); update(); });
        m.exec(global); return;
    }
    m.addAction("Piano roll", [this, id] { Q_EMIT openPianoRoll(id); });
    m.addAction("Open editor", [this, id] { Q_EMIT openEditor(id); });
    m.addAction("Rename...", [this, id, c] { bool ok; QString n = QInputDialog::getText(this, "Rename channel", "Name:", QLineEdit::Normal, c.name, &ok); if (ok && !n.isEmpty()) m_app.renameChannel(id, n); });
    m.addAction("Color...", [this, id, c] { QColor col = QColorDialog::getColor(c.color, this, "Channel color"); if (col.isValid()) m_app.setChannelColor(id, col); });
    m.addAction("Duplicate", [this, id] { m_app.duplicateChannel(id); }); m.addAction("Delete\tDel", [this, id] { if (QMessageBox::question(this, "Delete channel", "Delete this channel and its notes/steps in every pattern?", QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) m_app.deleteChannel(id); });
    m.addAction("Select all channels\tCtrl+A", [this] { for (auto& ch : m_app.project().channels) m_multi.insert(ch.id); update(); });
    m.addSeparator(); auto* inst = m.addMenu("Change instrument"); for (auto* i : NativeRegistry::instance().byKind(ProcessorKind::Instrument)) inst->addAction(i->name, [this, id, k = i->key] { m_app.changeChannelInstrument(id, k); });
    auto* plug = inst->addMenu("Plugins"); for (auto& p : m_app.pluginDb().plugins()) if (!p.failed && !p.hidden && p.category == "Instrument") plug->addAction(p.name, [this, id, p] { PluginInstanceData d = PluginHost::makeInstanceData(p, m_app.project().newId()); m_app.model().insert<PluginInstanceData>(d, "Add plugin"); m_app.changeChannelInstrument(id, "plugin", d.id); });
    auto* route = m.addMenu("Route to mixer track"); for (auto& t : proj.mixerTracks) { auto* a = route->addAction(t.name, [this, id, tid = t.id] { m_app.setChannelMixerTrack(id, tid); }); a->setCheckable(true); a->setChecked(t.id == c.mixerTrackId); }
    m.addSeparator(); m.addAction("Move up", [this, id, row] { m_app.moveChannel(id, std::max(0, row - 1)); }); m.addAction("Move down", [this, id, row] { m_app.moveChannel(id, row + 1); });
    m.addSeparator(); m.addAction("Fill every 2 steps", [this, id] { Pattern* p = pattern(); if (!p) return; m_app.editPattern(p->id, "Fill steps", [id](Pattern& pp) { auto& d = pp.ensureChannel(id); for (int i = 0; i < (int)d.steps.size(); ++i) d.steps[i].enabled = (i % 2 == 0); }); });
    m.addAction("Fill every 4 steps", [this, id] { Pattern* p = pattern(); if (!p) return; m_app.editPattern(p->id, "Fill steps", [id](Pattern& pp) { auto& d = pp.ensureChannel(id); for (int i = 0; i < (int)d.steps.size(); ++i) d.steps[i].enabled = (i % 4 == 0); }); });
    m.addAction("Clear (steps and notes)", [this, id] { Pattern* p = pattern(); if (!p) return; m_app.editPattern(p->id, "Clear channel in pattern", [id](Pattern& pp) { auto& d = pp.ensureChannel(id); for (auto& s : d.steps) s.enabled = false; d.notes.clear(); }); });
    m.addAction("Send steps to piano roll", [this, id] { Pattern* p = pattern(); if (!p) return; const Channel* ch = m_app.project().channel(id); int root = ch ? ch->rootNote : 60; m_app.editPattern(p->id, "Steps to notes", [id, root](Pattern& pp) { auto& d = pp.ensureChannel(id); Tick st = pp.stepTicks(); for (int i = 0; i < (int)d.steps.size(); ++i) if (d.steps[i].enabled) { Note n; n.id = (Id)(i + 1) * 7919 + (Id)pp.id * 31; n.pitch = root + (int)d.steps[i].pitch; n.start = i * st; n.duration = st; n.velocity = d.steps[i].velocity; n.pan = d.steps[i].pan; d.notes.push_back(n); d.steps[i].enabled = false; } }); });
    m.exec(global);
}
void RackView::dragEnterEvent(QDragEnterEvent* e) { if (e->mimeData()->hasUrls() || e->mimeData()->hasFormat("application/x-musicthingy-item")) e->acceptProposedAction(); }
void RackView::dragMoveEvent(QDragMoveEvent* e) { e->acceptProposedAction(); }
void RackView::dropEvent(QDropEvent* e) {
    HitInfo h = hitTest(e->position().toPoint()); auto& proj = m_app.project(); Id target = (h.row >= 0 && h.row < (int)proj.channels.size()) ? proj.channels[h.row].id : 0;
    QString payload = QString::fromUtf8(e->mimeData()->data("application/x-musicthingy-item")); QString kind = payload.section('|', 0, 0), val = payload.section('|', 1);
    if (e->mimeData()->hasUrls()) { for (auto& u : e->mimeData()->urls()) { QString p = u.toLocalFile(); if (!AudioFile::isAudioFile(p)) continue; if (target) { m_app.setChannelSample(target, p); target = 0; } else m_app.addChannel(ChannelType::Sampler, "native.sampler", QString(), p); } e->acceptProposedAction(); return; }
    if (kind == "native") { auto* info = NativeRegistry::instance().info(val); if (info && info->kind == ProcessorKind::Instrument) { if (target) m_app.changeChannelInstrument(target, val); else m_app.addChannel(val == "native.drumsampler" ? ChannelType::DrumSampler : val == "native.sampler" ? ChannelType::Sampler : val == "native.midiout" ? ChannelType::MidiOut : val == "native.tonegen" ? ChannelType::AudioGenerator : ChannelType::Synth, val); } else if (info) { Id tr = target ? proj.channel(target)->mixerTrackId : proj.masterId(); m_app.addEffect(tr, val); } }
    else if (kind == "plugin") { for (auto& p : m_app.pluginDb().plugins()) if (p.key() == val) { if (p.category == "Instrument") { if (target) { PluginInstanceData d = PluginHost::makeInstanceData(p, proj.newId()); m_app.model().insert<PluginInstanceData>(d, "Add plugin"); m_app.changeChannelInstrument(target, "plugin", d.id); } else m_app.addPluginChannel(p); } else { Id tr = target ? proj.channel(target)->mixerTrackId : proj.masterId(); m_app.addPluginEffect(tr, p); } } }
    else if (kind == "preset" && target) m_app.loadPreset(target, val);
    e->acceptProposedAction();
}

// ---------------------------------------------------------------- panel
ChannelRackPanel::ChannelRackPanel(App& app, QWidget* parent) : QWidget(parent), m_app(app) {
    auto* lay = new QVBoxLayout(this); lay->setContentsMargins(2, 2, 2, 2); lay->setSpacing(2);
    auto* top = new QHBoxLayout(); top->setSpacing(5); top->setContentsMargins(2, 0, 2, 0);
    m_patName = new QLabel(this); m_patName->setStyleSheet("font-weight:bold; color:#f0a13a; padding-left:4px;"); m_patName->setToolTip("Current pattern - right-click for rename, colour and step resolution"); m_patName->setContextMenuPolicy(Qt::CustomContextMenu); top->addWidget(m_patName);
    connect(m_patName, &QLabel::customContextMenuRequested, this, [this](const QPoint& pos) { auto* p = m_app.project().pattern(m_app.project().selectedPatternId); if (!p) return; Id pid = p->id; QMenu m(this); m.addAction("Rename...", [this, pid] { bool ok; QString n = QInputDialog::getText(this, "Rename pattern", "Name:", QLineEdit::Normal, m_app.project().pattern(pid)->name, &ok); if (ok && !n.isEmpty()) m_app.renamePattern(pid, n); }); m.addAction("Color...", [this, pid] { QColor c = QColorDialog::getColor(m_app.project().pattern(pid)->color, this); if (c.isValid()) m_app.setPatternColor(pid, c); }); auto* res = m.addMenu("Step resolution"); static const int spb[] = {1, 2, 3, 4, 8}; const char* names[] = {"1/4 (quarter notes)", "1/8", "1/8 triplets", "1/16 (default)", "1/32"}; for (int i = 0; i < 5; ++i) { auto* a = res->addAction(names[i], [this, pid, v = spb[i]] { m_app.editPattern(pid, "Step resolution", [v](Pattern& pp) { pp.stepsPerBeat = v; }); }); a->setCheckable(true); a->setChecked(p->stepsPerBeat == spb[i]); } m.addAction("Clone pattern", [this, pid] { m_app.clonePattern(pid); }); m.exec(m_patName->mapToGlobal(pos)); });
    auto* add = new QToolButton(this); add->setText("+ Channel"); add->setPopupMode(QToolButton::InstantPopup); auto* addMenu = new QMenu(add); for (auto* i : NativeRegistry::instance().byKind(ProcessorKind::Instrument)) addMenu->addAction(i->name, [this, k = i->key] { m_app.addChannel(k == "native.drumsampler" ? ChannelType::DrumSampler : k == "native.sampler" ? ChannelType::Sampler : k == "native.midiout" ? ChannelType::MidiOut : k == "native.tonegen" ? ChannelType::AudioGenerator : ChannelType::Synth, k); }); auto* plugMenu = addMenu->addMenu("Plugin instruments"); connect(plugMenu, &QMenu::aboutToShow, this, [this, plugMenu] { plugMenu->clear(); for (auto& p : m_app.pluginDb().plugins()) if (!p.failed && !p.hidden && p.category == "Instrument") plugMenu->addAction(p.name + " [" + p.format.toUpper() + "]", [this, p] { m_app.addPluginChannel(p); }); if (plugMenu->isEmpty()) plugMenu->addAction("(none scanned)")->setEnabled(false); }); add->setMenu(addMenu); top->addWidget(add);
    top->addSpacing(10); auto* l1 = new QLabel("Steps", this); l1->setStyleSheet("color:#8a919a;"); top->addWidget(l1); m_steps = new DragValue(this); m_steps->setRange(4, 128, 1, 0); m_steps->setMinimumWidth(48); top->addWidget(m_steps);
    m_resolution = new QComboBox(this); m_resolution->addItems({"1/4 (quarter)", "1/8", "1/8T", "1/16", "1/32"}); m_resolution->hide();
        m_swing = new Knob(this, 22); m_swing->setDefault(0.f); m_swing->setLabel("Swing"); m_swing->setShowLabel(false); m_swing->setToolTip("Swing"); m_swing->valueText = [](float v) { return QString("Swing %1%").arg((int)(v * 100)); }; top->addWidget(m_swing); auto* l3 = new QLabel("Swing", this); l3->setStyleSheet("color:#8a919a;"); top->addWidget(l3);
    top->addSpacing(10); auto* l4 = new QLabel("Graph", this); l4->setStyleSheet("color:#8a919a;"); top->addWidget(l4); m_graph = new QComboBox(this); m_graph->addItems({"Off", "Velocity", "Pan", "Pitch", "Probability", "Shift"}); m_graph->setToolTip("Graph editor: edit per-step values of the selected channel"); top->addWidget(m_graph);
    top->addStretch(1); lay->addLayout(top);
    m_scroll = new QScrollArea(this); m_view = new RackView(app, this); m_scroll->setWidget(m_view); m_scroll->setWidgetResizable(false); m_scroll->setFrameShape(QFrame::NoFrame); lay->addWidget(m_scroll);
    connect(m_view, &RackView::openEditor, this, &ChannelRackPanel::openEditor); connect(m_view, &RackView::openPianoRoll, this, &ChannelRackPanel::openPianoRoll);
    connect(m_steps, &DragValue::editingFinished, this, [this] { if (m_updating) return; if (auto* p = m_app.project().pattern(m_app.project().selectedPatternId)) { int n = (int)m_steps->value(); m_app.editPattern(p->id, "Pattern steps", [n](Pattern& pp) { pp.stepCount = n; for (auto& c : pp.channels) if ((int)c.steps.size() < n) c.steps.resize(n); }); } });
    connect(m_resolution, &QComboBox::currentIndexChanged, this, [this](int i) { if (m_updating) return; static const int spb[] = {1, 2, 3, 4, 8}; if (auto* p = m_app.project().pattern(m_app.project().selectedPatternId)) m_app.editPattern(p->id, "Step resolution", [v = spb[i]](Pattern& pp) { pp.stepsPerBeat = v; }); });
    connect(m_swing, &Knob::dragFinished, this, [this] { if (auto* p = m_app.project().pattern(m_app.project().selectedPatternId)) m_app.editPattern(p->id, "Swing", [v = m_swing->value()](Pattern& pp) { pp.swing = v; }, 3100); });
    connect(m_graph, &QComboBox::currentIndexChanged, this, [this](int i) { m_view->setGraphMode(i - 1); m_view->refresh(); });
    m_fitTimer = new QTimer(this); m_fitTimer->setSingleShot(true); m_fitTimer->setInterval(120); connect(m_fitTimer, &QTimer::timeout, this, &ChannelRackPanel::fitStepsToWidth);
    m_scroll->viewport()->installEventFilter(this);
    refresh(ChangeAll);
}
bool ChannelRackPanel::eventFilter(QObject* o, QEvent* e) { if (o == m_scroll->viewport() && e->type() == QEvent::Resize) m_fitTimer->start(); return QWidget::eventFilter(o, e); }
// FL behaviour: the number of steps shown (and the pattern's step count) follows the rack width, 4 steps at a time
void ChannelRackPanel::fitStepsToWidth() {
    int n = std::max(4, m_view->stepsThatFit(m_scroll->viewport()->width()));   // stepsThatFit already rounds down to groups of 4 steps
    m_view->setFitSteps(n);   // view only: the pattern length is never changed by the window size
}
int ChannelRackPanel::heightForContent() const { int rows = std::max(1, (int)m_app.project().channels.size()); return 38 + rows * 28 + (m_view->graphMode() >= 0 ? 100 : 0) + 22; }
void ChannelRackPanel::refresh(uint32_t) { m_updating = true; if (auto* p = m_app.project().pattern(m_app.project().selectedPatternId)) { m_patName->setText(p->name); m_steps->setValue(p->stepCount); static const int spb[] = {1, 2, 3, 4, 8}; for (int i = 0; i < 5; ++i) if (spb[i] == p->stepsPerBeat) m_resolution->setCurrentIndex(i); m_swing->setValue(p->swing); } else m_patName->setText("(no pattern)"); m_updating = false; m_view->refresh(); }
void ChannelRackPanel::uiTick() { m_view->uiTick(); }

} // namespace mt
