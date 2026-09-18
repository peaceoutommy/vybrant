#include "ui/ChannelRackPanel.h"
#include <QCoreApplication>
#include "ui/Theme.h"
#include "dsp/NativeRegistry.h"
#include <QPainter>
#include <QPainterPath>
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
RackView::RackView(App& app, QWidget* parent)
    : QWidget(parent)
    , m_app(app) {
    setAcceptDrops(true);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    m_ticker = new anim::Ticker(this);
}
Pattern* RackView::pattern() const { return m_app.project().pattern(m_app.project().selectedPatternId); }
const Step* RackView::stepAt(Id ch, int step) const {
    Pattern* p = pattern();
    if (!p) return nullptr;
    auto* d = p->channelData(ch);
    if (!d || step < 0 || step >= (int)d->steps.size()) return nullptr;
    return &d->steps[step];
}
int RackView::stepsPerBar() const {
    Pattern* p = pattern();
    const auto& ts = m_app.project().transport.timeSig;
    return std::max(1, (int)(ts.ticksPerBar() / (p ? p->stepTicks() : PPQ / 4)));
}
int RackView::patternSteps() const {
    Pattern* p = pattern();
    if (!p) return 16;
    return (int)(p->effectiveLength(m_app.project().transport.timeSig) / p->stepTicks());
}
// The rack shows the whole pattern plus enough extra (dimmed) steps to fill the window; only the pattern's own steps play
int RackView::visibleSteps() const { return std::max(patternSteps(), m_fitSteps); }
int RackView::stepW() const { return 18; }
int RackView::stepX(int step) const { return m_leftW + 6 + step * stepW(); }
QRect RackView::rowRect(int row) const { return QRect(0, row * m_rowH, width(), m_rowH); }
QSize RackView::sizeHint() const {
    int steps = visibleSteps();
    int rows = (int)m_app.project().channels.size();
    return QSize(stepX(steps) + 12, std::max(rows + 1, 3) * m_rowH + (m_graphMode >= 0 ? m_graphH : 0) + 4);
}   // channel rows + the '+' row (3 rows minimum: room for the empty-project hint)
void RackView::refresh() {
    for (auto it = m_multi.begin(); it != m_multi.end();) {
        if (!m_app.project().channel(*it)) it = m_multi.erase(it);
        else ++it;
    }
    updateGeometry();
    resize(sizeHint());
    update();
}
void RackView::uiTick() {
    auto tv = m_app.engine().transport();
    int ps = -1;
    Pattern* p = pattern();
    if (p && tv.playing && !tv.countingIn) {
        Tick local = (Tick)tv.positionTicks;
        if (tv.songMode) {
            local = -1;
            for (auto& c : m_app.project().clips)
                if (c.type == ClipType::Pattern && c.sourceId == p->id && tv.positionTicks >= c.start &&
                    tv.positionTicks < c.start + c.length) {
                    local = ((Tick)tv.positionTicks - c.start + c.sourceOffset) %
                        std::max<Tick>(p->effectiveLength(m_app.project().transport.timeSig), 1);
                    break;
                }
        }
        if (local >= 0) ps = (int)(local / p->stepTicks());
    }
    if (ps != m_playStep) {
        m_playStep = ps;
        update();
    }
}

RackView::HitInfo RackView::hitTest(const QPoint& pt) const {
    HitInfo h;
    int rows = (int)m_app.project().channels.size();
    int row = pt.y() / m_rowH;
    if (row < 0 || row >= rows) {
        if (row == rows && pt.x() < m_leftW) {
            h.hit = AddButton;
            return h;
        }
        if (m_graphMode >= 0 && pt.y() >= graphTop() && pt.y() < graphTop() + m_graphH) {
            h.hit = Graph;
            int steps = visibleSteps();
            for (int s = 0; s < steps; ++s)
                if (pt.x() >= stepX(s) && pt.x() < stepX(s) + stepW()) h.step = s;
        }
        return h;
    }
    h.row = row;
    int x = pt.x();
    if (x < 12) h.hit = Grip;
    else if (x < 28) h.hit = Enable;
    else if (x < 134) h.hit = Name;
    else if (x < 152) h.hit = Mute;
    else if (x < 170) h.hit = Solo;
    else if (x < 196) h.hit = Pan;
    else if (x < 222) h.hit = Vol;
    else if (x < m_leftW) h.hit = Mixer;
    else {
        int steps = visibleSteps();
        for (int s = 0; s < steps; ++s)
            if (x >= stepX(s) && x < stepX(s) + stepW()) {
                h.hit = StepCell;
                h.step = s;
                break;
            }
    }
    return h;
}

void RackView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), theme::bg());
    p.setRenderHint(QPainter::Antialiasing);
    Pattern* pat = pattern();
    int steps = visibleSteps();
    int patSteps = patternSteps();
    auto& proj = m_app.project();
    const auto& chans = proj.channels;
    {   // '+' row under the channels (FL style): drawn like a ghost channel so it reads as "the next slot"
        int y = (int)chans.size() * m_rowH;
        QRectF rr(0, y, width(), m_rowH);
        bool hot = QRectF(0, y, m_leftW, m_rowH).contains(mapFromGlobal(QCursor::pos()));
        p.setPen(Qt::NoPen);
        p.setBrush(theme::withAlpha(theme::raised(), hot ? 110 : 60));
        p.drawRoundedRect(rr.adjusted(2, 1, -2, -1), theme::kRadius, theme::kRadius);
        QRectF nb(30, y + 3, 102, m_rowH - 6);
        p.setPen(QPen(hot ? theme::accent() : theme::border(), 1, Qt::DashLine));
        p.setBrush(hot ? theme::withAlpha(theme::accent(), 25) : Qt::NoBrush);
        p.drawRoundedRect(nb, 3, 3);
        p.setPen(hot ? theme::accent() : theme::textDim());
        QFont bf = theme::boldFont();
        p.setFont(bf);
        p.drawText(nb.adjusted(8, 0, -3, 0), Qt::AlignVCenter | Qt::AlignLeft, "+  Add channel");
        p.setPen(QPen(theme::border(), 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(15, y + 10, 8, 8));
        p.drawRoundedRect(QRectF(136, y + 6, 15, 16), theme::kRadius, theme::kRadius);
        p.drawRoundedRect(QRectF(154, y + 6, 15, 16), theme::kRadius, theme::kRadius);
        p.drawEllipse(QRectF(174, y + 4, 20, 20));
        p.drawEllipse(QRectF(200, y + 4, 20, 20));
        p.drawRoundedRect(QRectF(226, y + 5, 28, m_rowH - 10), theme::kRadius, theme::kRadius);
    }
    if (chans.empty()) {   // empty project: explain how to get a first channel in
        QRect r = QRect(8, m_rowH + 2, width() - 16, height() - m_rowH - 8);
        p.setPen(QPen(theme::textDim(), 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, 6, 6);
        p.setPen(theme::textDim());
        p.setFont(theme::boldFont());
        p.drawText(r.adjusted(0, 0, 0, -18), Qt::AlignCenter, "No channels yet");
        p.setFont(theme::smallFont());
        p.drawText(
            r.adjusted(0, 18, 0, 0), Qt::AlignCenter, "Drag a sample, instrument or plugin here from the Browser, or click the + above");
    }
    for (int r = 0; r < (int)chans.size(); ++r) {
        const Channel& c = chans[r];
        QRect rr = rowRect(r);
        bool selected = c.id == proj.selectedChannelId;
        bool multi = m_multi.count(c.id) > 0;
        // rows are rounded bands (like the mixer's strip cards), inset 2px from the view edges
        QRectF band = QRectF(rr).adjusted(2, 0, -2, -1);
        p.setPen(Qt::NoPen);
        p.setBrush((selected || multi) ? QColor(0x27, 0x2c, 0x33) : (r % 2 ? theme::panel() : theme::altRow()));
        p.drawRoundedRect(band, theme::kRadius, theme::kRadius);
        if (multi) {
            p.save();
            QPainterPath clip;
            clip.addRoundedRect(band, theme::kRadius, theme::kRadius);
            p.setClipPath(clip);
            p.fillRect(QRectF(band.left(), band.top(), 3, band.height()), theme::accent());
            p.restore();
        }
        if (m_reorderTarget == r && m_dragging && m_press.hit == Grip) {
            p.setPen(QPen(theme::accent(), 2));
            p.drawLine(rr.left(), rr.top(), rr.right(), rr.top());
        }
        p.setPen(QColor(0x3a, 0x40, 0x48));
        for (int i = 0; i < 3; ++i) p.drawLine(4, rr.top() + 10 + i * 4, 8, rr.top() + 10 + i * 4);
        QRectF led(15, rr.top() + 10, 8, 8);
        p.setPen(Qt::NoPen);
        p.setBrush(c.enabled ? theme::green() : theme::border());
        p.drawEllipse(led);
        // name tag: flat panel with a colour bar, accent outline when selected
        QRectF nb(30, rr.top() + 3, 102, m_rowH - 6);
        p.setPen(Qt::NoPen);
        p.setBrush(selected ? QColor(0x2c, 0x31, 0x39) : theme::raised());
        p.drawRoundedRect(nb, 3, 3);
        p.setBrush(c.color);
        p.drawRoundedRect(QRectF(nb.left(), nb.top(), 4, nb.height()), 2, 2);
        if (selected) {
            p.setPen(QPen(theme::accent(), 1));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(nb, 3, 3);
        }
        p.setPen(c.enabled ? theme::text() : theme::textDim());
        p.setFont(theme::uiFont());
        p.drawText(nb.adjusted(8, 0, -3, 0), Qt::AlignVCenter | Qt::AlignLeft,
            p.fontMetrics().elidedText(c.name, Qt::ElideRight, (int)nb.width() - 11));
        auto smallBtn = [&](QRectF r, const QString& t, bool on, QColor col) {
            p.setPen(QPen(on ? col.darker(120) : theme::border(), 1));
            p.setBrush(on ? col : theme::panel());
            p.drawRoundedRect(r, theme::kRadius, theme::kRadius);
            p.setPen(on ? theme::bg() : theme::textDim());
            p.setFont(theme::smallFont());
            p.drawText(r, Qt::AlignCenter, t);
        };
        smallBtn(QRectF(136, rr.top() + 6, 15, 16), "M", c.mute, theme::red());
        smallBtn(QRectF(154, rr.top() + 6, 15, 16), "S", c.solo, theme::accent());
        auto knob = [&](QRectF r, float v, bool bipolar) {
            p.setPen(QPen(theme::border(), 1));
            p.setBrush(theme::panel());
            p.drawEllipse(r);
            QRectF a = r.adjusted(1.5, 1.5, -1.5, -1.5);
            p.setPen(QPen(theme::accent2(), 1.5, Qt::SolidLine, Qt::RoundCap));
            if (bipolar) p.drawArc(a, 90 * 16, (int)(-270 * (v - 0.5) * 16));
            else p.drawArc(a, 225 * 16, (int)(-270 * v * 16));
        };
        float pan = m_app.engine().registry().channelIfExists(c.id) ? m_app.engine().registry().channelIfExists(c.id)->pan.load() : c.pan;
        float vol = m_app.engine().registry().channelIfExists(c.id) ? m_app.engine().registry().channelIfExists(c.id)->volume.load()
                                                                    : c.volume;
        knob(QRectF(174, rr.top() + 4, 20, 20), (pan + 1) * 0.5f, true);
        knob(QRectF(200, rr.top() + 4, 20, 20), vol, false);
        QRectF mb(226, rr.top() + 5, 28, m_rowH - 10);
        p.setPen(QPen(theme::border(), 1));
        p.setBrush(theme::surface());
        p.drawRoundedRect(mb, theme::kRadius, theme::kRadius);
        int mi = proj.mixerIndex(c.mixerTrackId);
        p.setPen(theme::textDim());
        p.setFont(theme::smallFont());
        p.drawText(mb, Qt::AlignCenter, mi <= 0 ? "M" : QString::number(mi));
        const PatternChannelData* d = pat ? pat->channelData(c.id) : nullptr;
        if (d && !d->notes.empty()) {   // channel has piano-roll notes: show them instead of the step buttons
            QRectF area(stepX(0), rr.top() + 3, stepX(steps) - stepX(0) - 2, m_rowH - 6);
            p.setPen(QPen(theme::border(), 1));
            p.setBrush(theme::surface());
            p.drawRoundedRect(area, 3, 3);
            Tick plen = std::max<Tick>(pat->effectiveLength(proj.transport.timeSig), 1);
            int lo = 127, hi = 0;
            for (auto& n : d->notes) {
                lo = std::min(lo, n.pitch);
                hi = std::max(hi, n.pitch);
            }
            int range = std::max(hi - lo + 1, 5);
            p.setClipRect(area.adjusted(1, 1, -1, -1));
            Tick bar = proj.transport.timeSig.ticksPerBar();
            p.setPen(theme::grid());
            for (Tick t = bar; t < plen; t += bar) {
                double x = area.left() + (double)t / plen * area.width();
                p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
            }
            for (auto& n : d->notes) {
                double x1 = area.left() + (double)n.start / plen * area.width(),
                       x2 = area.left() + (double)(n.start + n.duration) / plen * area.width();
                double y = area.bottom() - 2 - (double)(n.pitch - lo + 0.5) / range * (area.height() - 4);
                p.setPen(QPen(c.color.lighter(120), 2));
                p.drawLine(QPointF(x1, y), QPointF(std::max(x2, x1 + 2), y));
            }
            if (m_playStep >= 0 && pat) {
                double x = area.left() + (double)(m_playStep * pat->stepTicks()) / plen * area.width();
                p.setPen(theme::playhead());
                p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
            }
            p.setClipping(false);
            p.setPen(theme::textDim());
            p.setFont(theme::smallFont());
            p.drawText(QRect(stepX(steps) + 2, rr.top(), 60, m_rowH), Qt::AlignVCenter, QString("\u266A %1").arg(d->notes.size()));
            continue;
        }
        for (int s = 0; s < steps; ++s) {
            QRectF sr(stepX(s) + 1, rr.top() + 5, stepW() - 3, m_rowH - 10);
            bool beatA = (s / 4) % 2 == 0;
            const Step* st = (d && s < (int)d->steps.size()) ? &d->steps[s] : nullptr;
            bool on = st && st->enabled;
            QColor base = beatA ? QColor(0x2a, 0x2f, 0x37) : theme::raised();
            if (s >= patSteps) base = base.darker(150);
            if (m_playStep == s) base = base.lighter(140);
            double pulse = m_pulses.value(r * 4096 + s);
            if (pulse > 0) {
                double g = pulse * 3.0;
                sr.adjust(-g, -g, g, g);
            }
            QColor lit = on ? c.color.lighter(st->velocity > 0.9f ? 115 : (int)(70 + st->velocity * 50)) : base;
            if (on && !beatA) lit = lit.darker(132);   // lit steps alternate per beat too, so a full row still shows the beats
            p.setPen(Qt::NoPen);
            p.setBrush(lit);
            p.drawRoundedRect(sr, theme::kRadius, theme::kRadius);
            if (on) {
                p.setPen(QPen(theme::withAlpha(Qt::white, 40), 1));
                p.setBrush(Qt::NoBrush);
                p.drawLine(QPointF(sr.left() + 2, sr.top() + 1), QPointF(sr.right() - 2, sr.top() + 1));
            }
            if (on && st->probability < 1.f) {
                p.setPen(QPen(theme::bg(), 1, Qt::DashLine));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(sr.adjusted(1, 1, -1, -1), 2, 2);
            }
            if (on && std::fabs(st->pitch) > 0.01f) {
                p.setPen(theme::bg());
                p.setFont(theme::smallFont());
                p.drawText(sr, Qt::AlignCenter, QString::number((int)st->pitch));
            }
        }
    }
    m_ticker->keepAlive(m_pulses.active());
    // graph editor
    if (m_graphMode >= 0) {
        int top = graphTop();
        QRect g(0, top, width(), m_graphH);
        p.fillRect(g, theme::surface());
        p.setPen(theme::border());
        p.drawLine(0, top, width(), top);
        static const char* names[] = {"Velocity", "Pan", "Pitch", "Probability", "Shift"};
        p.setPen(theme::textDim());
        p.setFont(theme::smallFont());
        p.drawText(QRect(6, top + 4, m_leftW, 16), Qt::AlignLeft,
            QString("Graph: %1  (selected channel; drag bars, right-click resets)").arg(names[m_graphMode]));
        const Channel* sel = proj.channel(proj.selectedChannelId);
        const PatternChannelData* d = (pat && sel) ? pat->channelData(sel->id) : nullptr;
        bool bipolar = m_graphMode == 1 || m_graphMode == 2 || m_graphMode == 4;
        int mid = top + m_graphH / 2;
        int bottom = top + m_graphH - 4;
        if (bipolar) {
            p.setPen(theme::gridStrong());
            p.drawLine(m_leftW, mid, width(), mid);
        }
        for (int s = 0; s < steps; ++s) {
            const Step* st = (d && s < (int)d->steps.size()) ? &d->steps[s] : nullptr;
            float v = 0;
            if (st) switch (m_graphMode) {
                case 0: v = st->velocity; break;
                case 1: v = st->pan; break;
                case 2: v = st->pitch / 24.f; break;
                case 3: v = st->probability; break;
                case 4: v = st->offset * 2.f; break;
                }
            QRectF bar;
            if (bipolar) {
                int h = (int)(v * (m_graphH / 2 - 8));
                bar = QRectF(stepX(s), h >= 0 ? mid - h : mid, stepW() - 2, std::abs(h));
            } else {
                int h = (int)(v * (m_graphH - 24));
                bar = QRectF(stepX(s), bottom - h, stepW() - 2, h);
            }
            p.setPen(Qt::NoPen);
            p.setBrush(st && st->enabled ? (sel ? sel->color : theme::accent()) : theme::gridStrong());
            p.drawRect(bar);
            p.setPen(theme::border());
            p.drawLine(stepX(s), bottom, stepX(s) + stepW() - 2, bottom);
        }
    }
}

void RackView::mousePressEvent(QMouseEvent* e) {
    m_press = hitTest(e->pos());
    m_lastPos = e->pos();
    m_dragging = true;
    auto& proj = m_app.project();
    setFocus();
    if (m_press.hit == AddButton) {
        if (e->button() == Qt::LeftButton) Q_EMIT addChannelRequested(mapToGlobal(QPoint(30, ((int)proj.channels.size() + 1) * m_rowH)));
        return;
    }
    if (m_press.row >= 0 && m_press.row < (int)proj.channels.size()) {
        const Channel& c = proj.channels[m_press.row];
        bool nameArea = m_press.hit == Name || m_press.hit == Grip || m_press.hit == Enable;
        if (nameArea && (e->modifiers() & Qt::ControlModifier)) {
            if (m_multi.count(c.id)) m_multi.erase(c.id);
            else m_multi.insert(c.id);
            if (m_multi.count(c.id)) m_app.selectChannel(c.id);
            update();
            if (m_press.hit == Enable) {
                m_dragging = false;
                return;
            }
        } else if (nameArea && (e->modifiers() & Qt::ShiftModifier)) {
            int a = -1, b = m_press.row;
            for (size_t i = 0; i < proj.channels.size(); ++i)
                if (proj.channels[i].id == m_anchor) a = (int)i;
            if (a < 0) a = b;
            for (int i = std::min(a, b); i <= std::max(a, b); ++i) m_multi.insert(proj.channels[i].id);
            m_app.selectChannel(c.id);
            update();
            return;
        } else {
            if (!m_multi.count(c.id) || !nameArea) { m_multi.clear(); }
            m_anchor = c.id;
            m_app.selectChannel(c.id);
        }
    }
    if (m_press.hit == Grip) {
        m_dragRow = m_press.row;
        m_reorderTarget = m_press.row;
        return;
    }
    if (m_press.hit == Graph) {
        if (e->button() == Qt::RightButton) {
            Pattern* pat = pattern();
            const Channel* sel = proj.channel(proj.selectedChannelId);
            if (pat && sel && m_press.step >= 0)
                m_app.setStepProperty(
                    pat->id, sel->id, m_press.step, m_graphMode, m_graphMode == 0 ? 0.8f : (m_graphMode == 3 ? 1.f : 0.f), true);
        } else applyGraphAt(e->pos(), false);
        return;
    }
    if (m_press.row < 0) return;
    const Channel& c = proj.channels[m_press.row];
    switch (m_press.hit) {
    case Enable: m_app.setChannelEnabled(c.id, !c.enabled); break;
    case Mute:
        if (e->button() == Qt::LeftButton) m_app.setChannelMute(c.id, !c.mute);
        break;
    case Solo: m_app.setChannelSolo(c.id, !c.solo); break;
    case Pan:
        m_knobStart = c.pan;
        if (e->button() == Qt::RightButton) {
            QMenu m;
            m.addAction("Create automation clip",
                [this, id = c.id] { m_app.createAutomationLane({ParamTarget::ChannelPan, id, 0, 0}, QString()); });
            m.addAction("Link to MIDI controller...",
                [this, id = c.id, n = c.name] { m_app.startMidiLearn({ParamTarget::ChannelPan, id, 0, 0}, n + " Pan"); });
            m.addAction("Reset", [this, id = c.id] { m_app.setChannelPan(id, 0, true); });
            m.exec(e->globalPosition().toPoint());
            m_dragging = false;
        }
        break;
    case Vol:
        m_knobStart = c.volume;
        if (e->button() == Qt::RightButton) {
            QMenu m;
            m.addAction("Create automation clip",
                [this, id = c.id] { m_app.createAutomationLane({ParamTarget::ChannelVolume, id, 0, 0}, QString()); });
            m.addAction("Link to MIDI controller...",
                [this, id = c.id, n = c.name] { m_app.startMidiLearn({ParamTarget::ChannelVolume, id, 0, 0}, n + " Volume"); });
            m.addAction("Reset", [this, id = c.id] { m_app.setChannelVolume(id, 0.8f, true); });
            m.exec(e->globalPosition().toPoint());
            m_dragging = false;
        }
        break;
    case Mixer: {
        if (e->button() == Qt::RightButton) {
            QMenu m;
            for (auto& t : proj.mixerTracks) {
                auto* a = m.addAction(t.name, [this, id = c.id, tid = t.id] { m_app.setChannelMixerTrack(id, tid); });
                a->setCheckable(true);
                a->setChecked(t.id == c.mixerTrackId);
            }
            m.exec(e->globalPosition().toPoint());
            m_dragging = false;
        }
        break;
    }
    case StepCell: {
        Pattern* pat = pattern();
        if (!pat) break;
        if (auto* pd = pat->channelData(c.id); pd && !pd->notes.empty()) {
            if (e->button() == Qt::LeftButton) Q_EMIT openPianoRoll(c.id);
            m_dragging = false;
            break;
        }
        const Step* st = stepAt(c.id, m_press.step);
        bool on = st && st->enabled;
        if (e->button() == Qt::RightButton) {
            m_erasing = true;
            if (on) m_app.setStep(pat->id, c.id, m_press.step, false);
        } else if (e->modifiers() & Qt::ShiftModifier) { /* shift+click: audition */
            m_app.noteOn(c.rootNote);
            m_app.noteOff(c.rootNote);
        } else {
            m_erasing = false;
            m_paintValue = !on;
            m_pulses.trigger(m_press.row * 4096 + m_press.step);
            m_app.setStep(pat->id, c.id, m_press.step, m_paintValue);
        }
        break;
    }
    case Name:
        if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::AltModifier)) Q_EMIT openPianoRoll(c.id);
        break;
    default: break;
    }
}
void RackView::mouseMoveEvent(QMouseEvent* e) {
    {
        static bool wasHot = false;
        int y = (int)m_app.project().channels.size() * m_rowH;
        bool hot = QRect(0, y, m_leftW, m_rowH).contains(e->pos());
        if (hot != wasHot) {
            wasHot = hot;
            update(QRect(0, y, m_leftW, m_rowH));
        }
    }
    if (!m_dragging) {
        HitInfo h = hitTest(e->pos());
        if (h.hit == StepCell) {
            auto& proj = m_app.project();
            if (h.row >= 0) {
                const Step* st = stepAt(proj.channels[h.row].id, h.step);
                if (st && st->enabled)
                    QToolTip::showText(e->globalPosition().toPoint(),
                        QString("Step %1  vel %2  pan %3  pitch %4  prob %5%  shift %6")
                            .arg(h.step + 1)
                            .arg(st->velocity, 0, 'f', 2)
                            .arg(st->pan, 0, 'f', 2)
                            .arg(st->pitch, 0, 'f', 0)
                            .arg((int)(st->probability * 100))
                            .arg(st->offset, 0, 'f', 2),
                        this);
            }
        }
        return;
    }
    auto& proj = m_app.project();
    int dy = m_lastPos.y() - e->pos().y();
    if (m_press.hit == Grip) {
        int row = clamp(e->pos().y() / m_rowH, 0, (int)proj.channels.size() - 1);
        m_reorderTarget = row;
        update();
        return;
    }
    if (m_press.hit == Graph) {
        applyGraphAt(e->pos(), false);
        return;
    }
    if (m_press.row < 0 || m_press.row >= (int)proj.channels.size()) return;
    const Channel& c = proj.channels[m_press.row];
    float sens = (e->modifiers() & Qt::ShiftModifier) ? 0.001f : 0.01f;
    if (m_press.hit == Pan) {
        float v = clamp(c.pan + dy * sens * 2, -1.f, 1.f);
        m_app.setChannelPan(c.id, v, false);
        update();
        QToolTip::showText(e->globalPosition().toPoint(), QString("Pan %1").arg(v, 0, 'f', 2), this);
    } else if (m_press.hit == Vol) {
        float v = clamp(c.volume + dy * sens, 0.f, 1.f);
        m_app.setChannelVolume(c.id, v, false);
        update();
        QToolTip::showText(e->globalPosition().toPoint(), QString("Volume %1 dB").arg(gainToDb(v / 0.8f), 0, 'f', 1), this);
    } else if (m_press.hit == Mixer) {
        if (std::abs(dy) >= 4) {
            int mi = proj.mixerIndex(c.mixerTrackId);
            mi = clamp(mi + (dy > 0 ? 1 : -1), 0, (int)proj.mixerTracks.size() - 1);
            m_app.setChannelMixerTrack(c.id, proj.mixerTracks[mi].id);
            m_lastPos = e->pos();
        }
        return;
    } else if (m_press.hit == StepCell) {
        HitInfo h = hitTest(e->pos());
        if (h.hit == StepCell && h.row >= 0) {
            Pattern* pat = pattern();
            if (!pat) return;
            const Channel& rc = proj.channels[h.row];
            const Step* st = stepAt(rc.id, h.step);
            bool on = st && st->enabled;
            if (m_erasing) {
                if (on) {
                    m_pulses.trigger(h.row * 4096 + h.step);
                    m_app.setStep(pat->id, rc.id, h.step, false);
                }
            } else if (on != m_paintValue) {
                m_pulses.trigger(h.row * 4096 + h.step);
                m_app.setStep(pat->id, rc.id, h.step, m_paintValue);
            }
        }
    }
    m_lastPos = e->pos();
}
void RackView::mouseReleaseEvent(QMouseEvent*) {
    if (!m_dragging) return;
    m_dragging = false;
    auto& proj = m_app.project();
    if (m_press.hit == Grip && m_dragRow >= 0 && m_reorderTarget != m_dragRow && m_dragRow < (int)proj.channels.size())
        m_app.moveChannel(proj.channels[m_dragRow].id, m_reorderTarget);
    m_dragRow = -1;
    m_reorderTarget = -1;
    if (m_press.row >= 0 && m_press.row < (int)proj.channels.size()) {
        const Channel& c = proj.channels[m_press.row];
        if (m_press.hit == Pan && std::fabs(c.pan - m_knobStart) > 1e-6f) {
            float v = c.pan;
            const_cast<Channel&>(c).pan = m_knobStart;
            m_app.setChannelPan(c.id, v, true);
        }
        if (m_press.hit == Vol && std::fabs(c.volume - m_knobStart) > 1e-6f) {
            float v = c.volume;
            const_cast<Channel&>(c).volume = m_knobStart;
            m_app.setChannelVolume(c.id, v, true);
        }
    }
    if (m_press.hit == Graph) applyGraphAt(m_lastPos, true);
    m_press = HitInfo();
    update();
}
void RackView::applyGraphAt(const QPoint& pt, bool commit) {
    auto& proj = m_app.project();
    Pattern* pat = pattern();
    const Channel* sel = proj.channel(proj.selectedChannelId);
    if (!pat || !sel) return;
    m_lastPos = pt;
    HitInfo h = hitTest(pt);
    int step = h.step;
    if (step < 0) {
        int steps = visibleSteps();
        for (int s = 0; s < steps; ++s)
            if (pt.x() < stepX(s) + stepW()) {
                step = s;
                break;
            }
        if (step < 0) step = steps - 1;
    }
    int top = graphTop();
    int bottom = top + m_graphH - 4;
    int mid = top + m_graphH / 2;
    bool bipolar = m_graphMode == 1 || m_graphMode == 2 || m_graphMode == 4;
    float v;
    if (bipolar) v = clamp((float)(mid - pt.y()) / (m_graphH / 2 - 8), -1.f, 1.f);
    else v = clamp((float)(bottom - pt.y()) / (m_graphH - 24), 0.f, 1.f);
    float value = v;
    if (m_graphMode == 2) value = std::round(v * 24.f);
    if (m_graphMode == 4) value = v * 0.5f;
    if (!commit && !(stepAt(sel->id, step) && stepAt(sel->id, step)->enabled) && m_graphMode != 0) return;
    m_app.setStepProperty(pat->id, sel->id, step, m_graphMode, value, commit);
    update();
}
void RackView::mouseDoubleClickEvent(QMouseEvent* e) {
    HitInfo h = hitTest(e->pos());
    auto& proj = m_app.project();
    if (h.row < 0 || h.row >= (int)proj.channels.size()) return;
    const Channel& c = proj.channels[h.row];
    if (h.hit == Name) Q_EMIT openEditor(c.id);
    if (h.hit == Pan) m_app.setChannelPan(c.id, 0, true);
    if (h.hit == Vol) m_app.setChannelVolume(c.id, 0.8f, true);
}
void RackView::wheelEvent(QWheelEvent* e) {
    HitInfo h = hitTest(e->position().toPoint());
    auto& proj = m_app.project();
    if (h.row < 0 || h.row >= (int)proj.channels.size()) {
        e->ignore();
        return;
    }
    const Channel& c = proj.channels[h.row];
    int d = e->angleDelta().y() > 0 ? 1 : -1;
    if (h.hit == StepCell) {
        Pattern* pat = pattern();
        const Step* st = stepAt(c.id, h.step);
        if (pat && st && st->enabled) {
            if (e->modifiers() & Qt::ControlModifier) m_app.setStepProperty(pat->id, c.id, h.step, 2, st->pitch + d, true);
            else m_app.setStepProperty(pat->id, c.id, h.step, 0, st->velocity + d * 0.05f, true);
        }
        e->accept();
        return;
    }
    if (h.hit == Vol) {
        m_app.setChannelVolume(c.id, clamp(c.volume + d * 0.02f, 0.f, 1.f), true);
        e->accept();
        return;
    }
    if (h.hit == Pan) {
        m_app.setChannelPan(c.id, clamp(c.pan + d * 0.05f, -1.f, 1.f), true);
        e->accept();
        return;
    }
    if (h.hit == Mixer) {
        int mi = clamp(proj.mixerIndex(c.mixerTrackId) + d, 0, (int)proj.mixerTracks.size() - 1);
        m_app.setChannelMixerTrack(c.id, proj.mixerTracks[mi].id);
        e->accept();
        return;
    }
    e->ignore();
}
void RackView::contextMenuEvent(QContextMenuEvent* e) {
    HitInfo h = hitTest(e->pos());
    if (h.hit == Name || h.hit == Enable || h.hit == Grip) showChannelMenu(h.row, e->globalPos());
    else if (h.row < 0 && h.hit == None) {
        QMenu m;
        auto* add = m.addMenu("Add channel");
        for (auto* i : NativeRegistry::instance().byKind(ProcessorKind::Instrument))
            add->addAction(i->name, [this, k = i->key] {
                m_app.addChannel(k == "native.drumsampler" ? ChannelType::DrumSampler
                        : k == "native.sampler"            ? ChannelType::Sampler
                        : k == "native.midiout"            ? ChannelType::MidiOut
                        : k == "native.tonegen"            ? ChannelType::AudioGenerator
                                                           : ChannelType::Synth,
                    k);
            });
        m.exec(e->globalPos());
    }
}
std::vector<Id> RackView::selectedIds() const {
    std::vector<Id> ids;
    for (auto& c : m_app.project().channels)
        if (m_multi.count(c.id) || (m_multi.empty() && c.id == m_app.project().selectedChannelId)) ids.push_back(c.id);
    return ids;
}
void RackView::deleteSelected() {
    auto ids = selectedIds();
    if (ids.empty()) return;
    if (QMessageBox::question(this, "Delete channels",
            QString("Delete %1 channel%2 and their notes/steps in every pattern?").arg(ids.size()).arg(ids.size() > 1 ? "s" : ""),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;
    for (Id id : ids) m_app.deleteChannel(id);
    m_multi.clear();
    update();
}
// FL Studio default keybinds (typing keyboard mode intercepts unmodified letters/digits before this, see MainWindow)
void RackView::keyPressEvent(QKeyEvent* e) {
    auto& proj = m_app.project();
    bool ctrl = e->modifiers() & Qt::ControlModifier, alt = e->modifiers() & Qt::AltModifier, shift = e->modifiers() & Qt::ShiftModifier;
    int k = e->key();
    int selIdx = -1;
    for (size_t i = 0; i < proj.channels.size(); ++i)
        if (proj.channels[i].id == proj.selectedChannelId) selIdx = (int)i;
    auto selectedIds = [&] {
        std::vector<Id> ids;
        for (auto& c : proj.channels)
            if (m_multi.count(c.id) || (m_multi.empty() && c.id == proj.selectedChannelId)) ids.push_back(c.id);
        return ids;
    };
    if (k == Qt::Key_Delete) {
        if (alt) {
            for (Id id : selectedIds()) m_app.deleteChannel(id);
            m_multi.clear();
            update();
        } else deleteSelected();
        return;
    }
    if (k == Qt::Key_A && ctrl) {
        for (auto& c : proj.channels) m_multi.insert(c.id);
        update();
        return;
    }
    if (k == Qt::Key_Escape) {
        m_multi.clear();
        update();
        return;
    }
    if (k == Qt::Key_C && alt) {
        std::vector<Id> ids = selectedIds();
        for (Id id : ids) m_app.duplicateChannel(id);
        return;
    }
    if ((k == Qt::Key_Up || k == Qt::Key_Down) && selIdx >= 0) {
        int dir = k == Qt::Key_Up ? -1 : 1;
        int j = selIdx + dir;
        if (j < 0 || j >= (int)proj.channels.size()) return;
        if (alt) m_app.moveChannel(proj.selectedChannelId, j);
        else {
            Id id = proj.channels[j].id;
            m_app.selectChannel(id);
            if (shift) m_multi.insert(id);
            else m_multi.clear();
            update();
        }
        return;
    }
    if (k >= Qt::Key_0 && k <= Qt::Key_9 && !alt) {
        int idx = k == Qt::Key_0 ? 9 : k - Qt::Key_1;
        if (idx < (int)proj.channels.size()) {
            auto& c = proj.channels[idx];
            if (ctrl) m_app.setChannelSolo(c.id, !c.solo);
            else m_app.setChannelMute(c.id, !c.mute);
        }
        return;
    }
    if ((k == Qt::Key_Left || k == Qt::Key_Right) && ctrl && shift) {
        Pattern* pat = proj.pattern(proj.selectedPatternId);
        if (!pat) return;
        int dir = k == Qt::Key_Left ? -1 : 1;
        std::vector<Id> ids = selectedIds();
        m_app.editPattern(pat->id, "Shift steps", [ids, dir](Pattern& pp) {
            for (Id id : ids) {
                auto* d = pp.channelData(id);
                if (!d || d->steps.empty()) continue;
                int n = (int)d->steps.size();
                if (dir > 0) std::rotate(d->steps.rbegin(), d->steps.rbegin() + 1, d->steps.rend());
                else std::rotate(d->steps.begin(), d->steps.begin() + 1, d->steps.end());
                (void)n;
            }
        });
        return;
    }
    QWidget::keyPressEvent(e);
}
void RackView::showChannelMenu(int row, const QPoint& global) {
    auto& proj = m_app.project();
    if (row < 0 || row >= (int)proj.channels.size()) return;
    Channel c = proj.channels[row];
    Id id = c.id;
    QMenu m(this);
    if (m_multi.size() > 1 && m_multi.count(id)) {   // actions on the whole selection
        auto ids = selectedIds();
        int n = (int)ids.size();
        m.addSection(QString("%1 channels selected").arg(n));
        auto* route = m.addMenu("Route all to mixer track");
        for (auto& t : proj.mixerTracks)
            route->addAction(t.name, [this, ids, tid = t.id] {
                for (Id i : ids) m_app.setChannelMixerTrack(i, tid);
            });
        m.addAction("Route each to its own free insert", [this, ids] {
            std::set<Id> used;
            for (auto& ch : m_app.project().channels) used.insert(ch.mixerTrackId);
            for (Id i : ids)
                for (auto& t : m_app.project().mixerTracks)
                    if (!t.isMaster && !used.count(t.id)) {
                        m_app.setChannelMixerTrack(i, t.id);
                        used.insert(t.id);
                        break;
                    }
        });
        m.addAction("Mute all", [this, ids] {
            for (Id i : ids) m_app.setChannelMute(i, true);
        });
        m.addAction("Unmute all", [this, ids] {
            for (Id i : ids) m_app.setChannelMute(i, false);
        });
        m.addAction("Solo all", [this, ids] {
            for (Id i : ids) m_app.setChannelSolo(i, true);
        });
        m.addAction("Unsolo all", [this, ids] {
            for (Id i : ids) m_app.setChannelSolo(i, false);
        });
        m.addAction("Color all...", [this, ids, c] {
            QColor col = QColorDialog::getColor(c.color, this, "Channel color");
            if (col.isValid())
                for (Id i : ids) m_app.setChannelColor(i, col);
        });
        m.addAction("Clear all (steps and notes)", [this, ids] {
            Pattern* p = pattern();
            if (!p) return;
            m_app.editPattern(p->id, "Clear channels in pattern", [ids](Pattern& pp) {
                for (Id i : ids) {
                    auto& d = pp.ensureChannel(i);
                    for (auto& s : d.steps) s.enabled = false;
                    d.notes.clear();
                }
            });
        });
        m.addAction("Duplicate all", [this, ids] {
            for (Id i : ids) m_app.duplicateChannel(i);
        });
        m.addAction(QString("Delete %1 channels\tDel").arg(n), [this] { deleteSelected(); });
        m.addSeparator();
        m.addAction("Clear selection\tEsc", [this] {
            m_multi.clear();
            update();
        });
        m.exec(global);
        return;
    }
    m.addAction("Piano roll", [this, id] { Q_EMIT openPianoRoll(id); });
    {
        const Channel* ch = m_app.project().channel(id);
        auto* cut = m.addAction("Cut itself");
        cut->setCheckable(true);
        cut->setChecked(ch && ch->cutItself);
        cut->setToolTip("A new note on this channel cuts the notes it is still playing (808s, hi-hats)");
        QObject::connect(cut, &QAction::toggled, this, [this, id](bool on) { m_app.setChannelCutItself(id, on); });
    }
    m.addAction("Open editor", [this, id] { Q_EMIT openEditor(id); });
    m.addAction("Rename...", [this, id, c] {
        bool ok;
        QString n = QInputDialog::getText(this, "Rename channel", "Name:", QLineEdit::Normal, c.name, &ok);
        if (ok && !n.isEmpty()) m_app.renameChannel(id, n);
    });
    m.addAction("Color...", [this, id, c] {
        QColor col = QColorDialog::getColor(c.color, this, "Channel color");
        if (col.isValid()) m_app.setChannelColor(id, col);
    });
    m.addAction("Duplicate", [this, id] { m_app.duplicateChannel(id); });
    m.addAction("Delete\tDel", [this, id] {
        if (QMessageBox::question(this, "Delete channel", "Delete this channel and its notes/steps in every pattern?",
                QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes)
            m_app.deleteChannel(id);
    });
    m.addAction("Select all channels\tCtrl+A", [this] {
        for (auto& ch : m_app.project().channels) m_multi.insert(ch.id);
        update();
    });
    m.addSeparator();
    auto* inst = m.addMenu("Change instrument");
    for (auto* i : NativeRegistry::instance().byKind(ProcessorKind::Instrument))
        inst->addAction(i->name, [this, id, k = i->key] { m_app.changeChannelInstrument(id, k); });
    auto* plug = inst->addMenu("Plugins");
    for (auto& p : m_app.pluginDb().plugins())
        if (!p.failed && !p.hidden && p.category == "Instrument")
            plug->addAction(p.name, [this, id, p] {
                PluginInstanceData d = PluginHost::makeInstanceData(p, m_app.project().newId());
                m_app.model().insert<PluginInstanceData>(d, "Add plugin");
                m_app.changeChannelInstrument(id, "plugin", d.id);
            });
    auto* route = m.addMenu("Route to mixer track");
    for (auto& t : proj.mixerTracks) {
        auto* a = route->addAction(t.name, [this, id, tid = t.id] { m_app.setChannelMixerTrack(id, tid); });
        a->setCheckable(true);
        a->setChecked(t.id == c.mixerTrackId);
    }
    m.addSeparator();
    m.addAction("Move up", [this, id, row] { m_app.moveChannel(id, std::max(0, row - 1)); });
    m.addAction("Move down", [this, id, row] { m_app.moveChannel(id, row + 1); });
    m.addSeparator();
    m.addAction("Fill every 2 steps", [this, id] {
        Pattern* p = pattern();
        if (!p) return;
        m_app.editPattern(p->id, "Fill steps", [id](Pattern& pp) {
            auto& d = pp.ensureChannel(id);
            for (int i = 0; i < (int)d.steps.size(); ++i) d.steps[i].enabled = (i % 2 == 0);
        });
    });
    m.addAction("Fill every 4 steps", [this, id] {
        Pattern* p = pattern();
        if (!p) return;
        m_app.editPattern(p->id, "Fill steps", [id](Pattern& pp) {
            auto& d = pp.ensureChannel(id);
            for (int i = 0; i < (int)d.steps.size(); ++i) d.steps[i].enabled = (i % 4 == 0);
        });
    });
    m.addAction("Clear (steps and notes)", [this, id] {
        Pattern* p = pattern();
        if (!p) return;
        m_app.editPattern(p->id, "Clear channel in pattern", [id](Pattern& pp) {
            auto& d = pp.ensureChannel(id);
            for (auto& s : d.steps) s.enabled = false;
            d.notes.clear();
        });
    });
    m.exec(global);
}
void RackView::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasUrls() || e->mimeData()->hasFormat("application/x-musicthingy-item")) {
        e->setDropAction(Qt::CopyAction);
        e->accept();
    }
}
void RackView::dragMoveEvent(QDragMoveEvent* e) {
    e->setDropAction(Qt::CopyAction);
    e->accept();
}
void RackView::dropEvent(QDropEvent* e) {
    handleDrop(e->mimeData(), e->position().toPoint());
    e->setDropAction(Qt::CopyAction);
    e->accept();
}
// Drop logic shared with the panel (a drop outside the rows lands here with a point below the last row = new channel)
void RackView::handleDrop(const QMimeData* mime, const QPoint& at) {
    HitInfo h = hitTest(at);
    auto& proj = m_app.project();
    Id target = (h.row >= 0 && h.row < (int)proj.channels.size()) ? proj.channels[h.row].id : 0;
    QString payload = QString::fromUtf8(mime->data("application/x-musicthingy-item"));
    QString kind = payload.section('|', 0, 0), val = payload.section('|', 1);
    if (mime->hasUrls()) {
        for (auto& u : mime->urls()) {
            QString p = u.toLocalFile();
            if (!AudioFile::isAudioFile(p)) continue;
            if (target) {
                m_app.setChannelSample(target, p);
                target = 0;
            } else m_app.addChannel(ChannelType::Sampler, "native.sampler", QString(), p);
        }
        return;
    }
    if (kind == "native") {
        auto* info = NativeRegistry::instance().info(val);
        if (info && info->kind == ProcessorKind::Instrument) {
            if (target) m_app.changeChannelInstrument(target, val);
            else
                m_app.addChannel(val == "native.drumsampler" ? ChannelType::DrumSampler
                        : val == "native.sampler"            ? ChannelType::Sampler
                        : val == "native.midiout"            ? ChannelType::MidiOut
                        : val == "native.tonegen"            ? ChannelType::AudioGenerator
                                                             : ChannelType::Synth,
                    val);
        } else if (info) {
            Id tr = target ? proj.channel(target)->mixerTrackId : proj.masterId();
            m_app.addEffect(tr, val);
        }
    } else if (kind == "plugin") {
        for (auto& p : m_app.pluginDb().plugins())
            if (p.key() == val) {
                if (p.category == "Instrument") {
                    if (target) {
                        PluginInstanceData d = PluginHost::makeInstanceData(p, proj.newId());
                        m_app.model().insert<PluginInstanceData>(d, "Add plugin");
                        m_app.changeChannelInstrument(target, "plugin", d.id);
                    } else m_app.addPluginChannel(p);
                } else {
                    Id tr = target ? proj.channel(target)->mixerTrackId : proj.masterId();
                    m_app.addPluginEffect(tr, p);
                }
            }
    } else if (kind == "preset" && target) m_app.loadPreset(target, val);
}

// ---------------------------------------------------------------- panel
ChannelRackPanel::ChannelRackPanel(App& app, QWidget* parent)
    : QWidget(parent)
    , m_app(app) {
    setAcceptDrops(true);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(2, 2, 2, 2);
    lay->setSpacing(2);
    auto* top = new QHBoxLayout();
    top->setSpacing(5);
    top->setContentsMargins(2, 0, 2, 0);
    m_patName = new QLabel(this);
    theme::styled(m_patName, "font-weight:bold; color:@accent@; padding-left:4px;");
    m_patName->setToolTip("Current pattern - right-click for rename, colour and step resolution");
    m_patName->setContextMenuPolicy(Qt::CustomContextMenu);
    top->addWidget(m_patName);
    connect(m_patName, &QLabel::customContextMenuRequested, this, [this](const QPoint& pos) {
        auto* p = m_app.project().pattern(m_app.project().selectedPatternId);
        if (!p) return;
        Id pid = p->id;
        QMenu m(this);
        m.addAction("Rename...", [this, pid] {
            bool ok;
            QString n = QInputDialog::getText(this, "Rename pattern", "Name:", QLineEdit::Normal, m_app.project().pattern(pid)->name, &ok);
            if (ok && !n.isEmpty()) m_app.renamePattern(pid, n);
        });
        m.addAction("Color...", [this, pid] {
            QColor c = QColorDialog::getColor(m_app.project().pattern(pid)->color, this);
            if (c.isValid()) m_app.setPatternColor(pid, c);
        });
        m.addAction("Split by channel", [this, pid] {
             m_app.splitPatternByChannel(pid);
         })->setToolTip("Every channel with content becomes its own pattern, named after the channel");
        auto* res = m.addMenu("Step resolution");
        static const int spb[] = {1, 2, 3, 4, 8};
        const char* names[] = {"1/4 (quarter notes)", "1/8", "1/8 triplets", "1/16 (default)", "1/32"};
        for (int i = 0; i < 5; ++i) {
            auto* a = res->addAction(names[i],
                [this, pid, v = spb[i]] { m_app.editPattern(pid, "Step resolution", [v](Pattern& pp) { pp.stepsPerBeat = v; }); });
            a->setCheckable(true);
            a->setChecked(p->stepsPerBeat == spb[i]);
        }
        m.addAction("Clone pattern", [this, pid] { m_app.clonePattern(pid); });
        m.exec(m_patName->mapToGlobal(pos));
    });
    auto* addMenu = new QMenu(this);
    m_addMenu = addMenu;
    for (auto* i : NativeRegistry::instance().byKind(ProcessorKind::Instrument))
        addMenu->addAction(i->name, [this, k = i->key] {
            m_app.addChannel(k == "native.drumsampler" ? ChannelType::DrumSampler
                    : k == "native.sampler"            ? ChannelType::Sampler
                    : k == "native.midiout"            ? ChannelType::MidiOut
                    : k == "native.tonegen"            ? ChannelType::AudioGenerator
                                                       : ChannelType::Synth,
                k);
        });
    auto* plugMenu = addMenu->addMenu("Plugin instruments");
    connect(plugMenu, &QMenu::aboutToShow, this, [this, plugMenu] {
        plugMenu->clear();
        for (auto& p : m_app.pluginDb().plugins())
            if (!p.failed && !p.hidden && p.category == "Instrument")
                plugMenu->addAction(p.name + " [" + p.format.toUpper() + "]", [this, p] { m_app.addPluginChannel(p); });
        if (plugMenu->isEmpty()) plugMenu->addAction("(none scanned)")->setEnabled(false);
    });
    auto* l1 = new QLabel("Steps", this);
    theme::styled(l1, "color:@textDim@;");
    m_steps = new DragValue(this);
    m_steps->setRange(4, 128, 1, 0);
    m_steps->setMinimumWidth(48);
    m_resolution = new QComboBox(this);
    m_resolution->addItems({"1/4 (quarter)", "1/8", "1/8T", "1/16", "1/32"});
    m_resolution->hide();
    m_swing = new Knob(this, 22);
    m_swing->setDefault(0.f);
    m_swing->setLabel("Swing");
    m_swing->setShowLabel(false);
    m_swing->setToolTip("Swing");
    m_swing->valueText = [](float v) { return QString("Swing %1%").arg((int)(v * 100)); };
    auto* l3 = new QLabel("Swing", this);
    theme::styled(l3, "color:@textDim@;");
    top->addSpacing(10);
    auto* l4 = new QLabel("Graph", this);
    theme::styled(l4, "color:@textDim@;");
    top->addWidget(l4);
    m_graph = new QComboBox(this);
    m_graph->addItems({"Off", "Velocity", "Pan", "Pitch", "Probability", "Shift"});
    m_graph->setToolTip("Graph editor: edit per-step values of the selected channel");
    top->addWidget(m_graph);
    // graph selector on the left; swing and the step count on the right (FL puts them there)
    top->addStretch(1);
    top->addWidget(l3);
    top->addWidget(m_swing);
    top->addSpacing(10);
    top->addWidget(l1);
    top->addWidget(m_steps);
    lay->addLayout(top);
    m_scroll = new QScrollArea(this);
    m_view = new RackView(app, this);
    m_scroll->setWidget(m_view);
    m_scroll->setWidgetResizable(false);
    m_scroll->setFrameShape(QFrame::NoFrame);
    theme::roundedFrame(m_scroll);
    lay->addWidget(m_scroll);
    connect(m_view, &RackView::addChannelRequested, this, [this](const QPoint& g) { m_addMenu->popup(g); });
    connect(m_view, &RackView::openEditor, this, &ChannelRackPanel::openEditor);
    connect(m_view, &RackView::openPianoRoll, this, &ChannelRackPanel::openPianoRoll);
    connect(m_steps, &DragValue::editingFinished, this, [this] {
        if (m_updating) return;
        if (auto* p = m_app.project().pattern(m_app.project().selectedPatternId)) {
            int n = (int)m_steps->value();
            m_app.editPattern(p->id, "Pattern steps", [n](Pattern& pp) {
                pp.stepCount = n;
                for (auto& c : pp.channels)
                    if ((int)c.steps.size() < n) c.steps.resize(n);
            });
        }
    });
    connect(m_resolution, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (m_updating) return;
        static const int spb[] = {1, 2, 3, 4, 8};
        if (auto* p = m_app.project().pattern(m_app.project().selectedPatternId))
            m_app.editPattern(p->id, "Step resolution", [v = spb[i]](Pattern& pp) { pp.stepsPerBeat = v; });
    });
    connect(m_swing, &Knob::dragFinished, this, [this] {
        if (auto* p = m_app.project().pattern(m_app.project().selectedPatternId))
            m_app.editPattern(p->id, "Swing", [v = m_swing->value()](Pattern& pp) { pp.swing = v; }, 3100);
    });
    connect(m_graph, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_view->setGraphMode(i - 1);
        m_view->refresh();
    });
    m_fitTimer = new QTimer(this);
    m_fitTimer->setSingleShot(true);
    m_fitTimer->setInterval(120);
    connect(m_fitTimer, &QTimer::timeout, this, &ChannelRackPanel::fitStepsToWidth);
    m_scroll->viewport()->installEventFilter(this);
    refresh(ChangeAll);
}
bool ChannelRackPanel::eventFilter(QObject* o, QEvent* e) {
    if (o == m_scroll->viewport() && e->type() == QEvent::Resize) m_fitTimer->start();
    return QWidget::eventFilter(o, e);
}
// FL behaviour: the number of steps shown (and the pattern's step count) follows the rack width, 4 steps at a time
void ChannelRackPanel::fitStepsToWidth() {
    int n = std::max(4, m_view->stepsThatFit(m_scroll->viewport()->width()));   // stepsThatFit already rounds down to groups of 4 steps
    m_view->setFitSteps(n);   // view only: the pattern length is never changed by the window size
}
// Anything dropped on the rack window outside the channel rows (header, empty space) becomes a new channel
void ChannelRackPanel::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasUrls() || e->mimeData()->hasFormat("application/x-musicthingy-item")) {
        e->setDropAction(Qt::CopyAction);
        e->accept();
    }
}
void ChannelRackPanel::dragMoveEvent(QDragMoveEvent* e) {
    e->setDropAction(Qt::CopyAction);
    e->accept();
}
void ChannelRackPanel::dropEvent(QDropEvent* e) {
    m_view->handleDrop(e->mimeData(), QPoint(0, 1000000));
    e->setDropAction(Qt::CopyAction);
    e->accept();
}   // never re-send the event: Qt routes a Drop back to the current drag target (this panel) -> infinite recursion
int ChannelRackPanel::heightForContent() const {
    int rows = std::max(3, (int)m_app.project().channels.size() + 1);
    return 38 + rows * 28 + (m_view->graphMode() >= 0 ? 100 : 0) + 22;
}
void ChannelRackPanel::refresh(uint32_t) {
    m_updating = true;
    if (auto* p = m_app.project().pattern(m_app.project().selectedPatternId)) {
        m_patName->setText(p->name);
        m_steps->setValue(p->stepCount);
        static const int spb[] = {1, 2, 3, 4, 8};
        for (int i = 0; i < 5; ++i)
            if (spb[i] == p->stepsPerBeat) m_resolution->setCurrentIndex(i);
        m_swing->setValue(p->swing);
    } else m_patName->setText("(no pattern)");
    m_updating = false;
    m_view->refresh();
}
void ChannelRackPanel::uiTick() { m_view->uiTick(); }

}   // namespace mt
