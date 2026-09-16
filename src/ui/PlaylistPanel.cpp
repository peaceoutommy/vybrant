#include "ui/PlaylistPanel.h"
#include "ui/TransportBar.h"
#include "ui/Theme.h"
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMenu>
#include <QInputDialog>
#include <QColorDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QUrl>
#include <climits>

namespace mt {

PlaylistView::PlaylistView(App& app, QWidget* parent) : QWidget(parent), m_app(app) {
    setFocusPolicy(Qt::StrongFocus); setMouseTracking(true); setAcceptDrops(true);
    m_hbar = new QScrollBar(Qt::Horizontal, this); m_vbar = new QScrollBar(Qt::Vertical, this);
    connect(m_hbar, &QScrollBar::valueChanged, this, [this](int v) { if (!m_updatingBars) { m_scrollTick = (Tick)v * PPQ / 4; invalidate(); } });
    connect(m_vbar, &QScrollBar::valueChanged, this, [this](int v) { if (!m_updatingBars) { m_scrollY = v; invalidate(); } });
}
void PlaylistView::setTool(Tool t) { m_tool = t; setCursor(t == Erase ? Qt::ForbiddenCursor : t == Slice ? Qt::SplitHCursor : t == Select ? Qt::CrossCursor : Qt::ArrowCursor); }
PlaylistView::Layout PlaylistView::layout() const { Layout l; int w = width() - 14, h = height() - 14; l.ruler = QRect(l.headerW, 0, w - l.headerW, l.rulerH); l.headers = QRect(0, l.rulerH, l.headerW, h - l.rulerH); l.area = QRect(l.headerW, l.rulerH, w - l.headerW, h - l.rulerH); return l; }
void PlaylistView::resizeEvent(QResizeEvent*) { Layout L = layout(); m_hbar->setGeometry(L.headerW, height() - 14, width() - L.headerW - 14, 14); m_vbar->setGeometry(width() - 14, L.rulerH, 14, height() - L.rulerH - 14); updateScrollbars(); invalidate(); }
Tick PlaylistView::xToTick(int x) const { return m_scrollTick + (Tick)((x - layout().headerW) / m_ppt); }
int PlaylistView::tickToX(Tick t) const { return layout().headerW + (int)((t - m_scrollTick) * m_ppt); }
int PlaylistView::trackH(int idx) const { auto& tr = m_app.project().playlistTracks; return (idx >= 0 && idx < (int)tr.size()) ? tr[idx].height : 60; }
int PlaylistView::trackTop(int idx) const { int y = layout().rulerH - m_scrollY; for (int i = 0; i < idx; ++i) y += trackH(i); return y; }
int PlaylistView::trackAtY(int y) const { auto& tr = m_app.project().playlistTracks; int top = layout().rulerH - m_scrollY; for (int i = 0; i < (int)tr.size(); ++i) { if (y >= top && y < top + tr[i].height) return i; top += tr[i].height; } return -1; }
Tick PlaylistView::snapTicks() const { if (m_snapDiv == 0) return 1; if (m_snapDiv < 0) return m_app.project().transport.timeSig.ticksPerBar() * -m_snapDiv; return PPQ / m_snapDiv; }
Tick PlaylistView::snap(Tick t, bool force) const { if (m_altHeld && !force) return t; Tick s = snapTicks(); if (s <= 1) return t; return (t / s) * s; }
QRect PlaylistView::clipRect(const Clip& c) const { auto& tr = m_app.project().playlistTracks; int idx = -1; for (size_t i = 0; i < tr.size(); ++i) if (tr[i].id == c.trackId) idx = (int)i; if (idx < 0) return QRect(); return QRect(tickToX(c.start), trackTop(idx) + 1, std::max(3, (int)(c.length * m_ppt)), trackH(idx) - 2); }
Clip* PlaylistView::clipAt(const QPoint& p, int* edge) const { auto& clips = m_app.project().clips; for (auto it = clips.rbegin(); it != clips.rend(); ++it) { QRect r = clipRect(*it); if (r.contains(p)) { if (edge) { *edge = 0; if (r.width() > 14) { if (p.x() >= r.right() - 6) *edge = 1; else if (p.x() <= r.left() + 5) *edge = -1; } } return const_cast<Clip*>(&*it); } } return nullptr; }
void PlaylistView::updateScrollbars() { m_updatingBars = true; Tick len = std::max<Tick>(m_app.project().songLength(), PPQ * 4) + PPQ * 32; Tick visible = (Tick)(layout().area.width() / m_ppt); m_hbar->setRange(0, (int)std::max<Tick>(0, (len - visible) * 4 / PPQ)); m_hbar->setPageStep((int)(visible * 4 / PPQ)); m_hbar->setValue((int)(m_scrollTick * 4 / PPQ)); int total = 0; for (auto& t : m_app.project().playlistTracks) total += t.height; m_vbar->setRange(0, std::max(0, total + 60 - layout().area.height())); m_vbar->setPageStep(layout().area.height()); m_vbar->setValue(m_scrollY); m_updatingBars = false; }
void PlaylistView::refresh() { updateScrollbars(); invalidate(); }
void PlaylistView::zoomToFit() { Tick len = std::max<Tick>(m_app.project().songLength(), PPQ * 16); m_ppt = clamp((double)layout().area.width() / (len + PPQ * 4), 0.002, 1.0); m_scrollTick = 0; refresh(); }
void PlaylistView::uiTick() { auto tv = m_app.engine().transport(); double pt = tv.playing && !tv.countingIn && tv.songMode ? tv.positionTicks : (tv.songMode ? tv.positionTicks : -1); if (pt != m_playTick) { m_playTick = pt; update(); } }

// ---------------------------------------------------------------- automation helpers
int PlaylistView::autoPointAt(const Clip& c, const QPoint& p) const { const AutomationLane* l = m_app.project().lane(c.sourceId); if (!l) return -1; QRect r = clipRect(c); for (int i = 0; i < (int)l->points.size(); ++i) { int x = tickToX(c.start + l->points[i].time - c.sourceOffset); int y = r.bottom() - 4 - (int)(l->points[i].value * (r.height() - 8)); if (std::abs(x - p.x()) <= 5 && std::abs(y - p.y()) <= 5) return i; } return -1; }
float PlaylistView::autoValueAtY(const Clip& c, int y) const { QRect r = clipRect(c); return clamp((float)(r.bottom() - 4 - y) / (r.height() - 8), 0.f, 1.f); }
Tick PlaylistView::autoTickAtX(const Clip& c, int x) const { return clamp<Tick>(xToTick(x) - c.start + c.sourceOffset, 0, c.length + c.sourceOffset); }

// ---------------------------------------------------------------- painting
void PlaylistView::drawClip(QPainter& p, const Clip& c) {
    QRect r = clipRect(c); Layout L = layout(); if (r.isNull() || r.right() < L.area.left() || r.left() > L.area.right() || r.bottom() < L.area.top() || r.top() > L.area.bottom()) return;
    auto& proj = m_app.project(); bool sel = m_selected.count(c.id); QColor col = theme::accent2();
    if (c.type == ClipType::Pattern) { if (auto* pat = proj.pattern(c.sourceId)) col = pat->color; } else if (c.type == ClipType::Audio) col = QColor("#5ad1c9"); else if (c.type == ClipType::Automation) { if (auto* l = proj.lane(c.sourceId)) col = l->color; }
    if (c.muted) col = col.darker(220);
    p.setPen(sel ? QPen(Qt::white, 1.5) : QPen(col.darker(140), 1)); p.setBrush(col.darker(sel ? 120 : 160)); p.drawRoundedRect(r, 3, 3);
    p.setClipRect(r.adjusted(1, 1, -1, -1));
    // header strip
    p.fillRect(QRect(r.left(), r.top(), r.width(), 14), col.darker(sel ? 90 : 110)); p.setPen(theme::bg()); p.setFont(theme::smallFont()); p.drawText(QRect(r.left() + 4, r.top(), r.width() - 8, 14), Qt::AlignVCenter | Qt::AlignLeft, c.name + (c.muted ? " (muted)" : ""));
    QRect body = r.adjusted(1, 15, -1, -1);
    if (c.type == ClipType::Pattern) {
        if (auto* pat = proj.pattern(c.sourceId)) {
            Tick plen = std::max<Tick>(pat->effectiveLength(proj.transport.timeSig), 1); int lo = 127, hi = 0; for (auto& ch : pat->channels) { for (auto& n : ch.notes) { lo = std::min(lo, n.pitch); hi = std::max(hi, n.pitch); } for (size_t i = 0; i < ch.steps.size(); ++i) if (ch.steps[i].enabled) { auto* cc = proj.channel(ch.id); int pp = cc ? cc->rootNote : 60; lo = std::min(lo, pp); hi = std::max(hi, pp); } }
            if (lo > hi) { lo = 48; hi = 72; } int range = std::max(hi - lo + 1, 6);
            for (Tick k = 0; k * plen < c.length + c.sourceOffset; ++k) {
                Tick off = c.start - c.sourceOffset + k * plen; if (off + plen <= c.start) continue; if (k > 0) { int x = tickToX(off); p.setPen(QPen(col.darker(90), 1, Qt::DashLine)); p.drawLine(x, body.top(), x, body.bottom()); }
                for (auto& ch : pat->channels) { auto* cc = proj.channel(ch.id); QColor nc = cc ? cc->color.lighter(130) : theme::text(); p.setPen(nc);
                    for (auto& n : ch.notes) { int x1 = tickToX(off + n.start), x2 = tickToX(off + n.start + n.duration); int y = body.bottom() - (int)((n.pitch - lo + 0.5) / range * body.height()); p.drawLine(std::max(x1, r.left()), y, std::max(x2, x1 + 1), y); }
                    Tick st = pat->stepTicks(); for (size_t i = 0; i < ch.steps.size() && (int)i < pat->stepCount; ++i) if (ch.steps[i].enabled) { int x1 = tickToX(off + i * st); int y = body.bottom() - (int)(((cc ? cc->rootNote : 60) - lo + 0.5) / range * body.height()); p.drawLine(x1, y, x1 + std::max(1, (int)(st * m_ppt) - 1), y); } }
            }
        }
    } else if (c.type == ClipType::Audio) {
        const AudioSource* src = proj.audioSource(c.sourceId); AudioAssetPtr a = src ? m_app.cache().get(src->filePath) : nullptr;
        if (a && a->frames > 0) {
            double sr = a->sampleRate; double bpm = proj.transport.bpm; double framesPerTick = sr * 60.0 / (bpm * PPQ) * (std::fabs(c.timeStretch - 1.0) > 1e-4 ? 1.0 / c.timeStretch : std::pow(2.0, c.pitch / 12.0)); double spp = framesPerTick / m_ppt; const WaveformLevel& lv = a->levelFor(spp); int mid = body.center().y(); int hh = body.height() / 2 - 1;
            p.setPen(col.lighter(150)); int64_t srcStart = src->trimStart; int64_t srcLen = (src->trimEnd > 0 ? src->trimEnd : a->frames) - srcStart; bool rev = c.reverse != src->reverse; float g = c.gain * src->gain * (src->normalize && a->peakValue > 0 ? 1.f / a->peakValue : 1.f);
            int x0 = std::max(r.left() + 1, L.area.left()), x1 = std::min(r.right() - 1, L.area.right());
            for (int x = x0; x <= x1; ++x) { double tick = xToTick(x) - c.start + c.sourceOffset; double frame = tick * framesPerTick; if (frame >= srcLen && !c.loopSource) break; if (c.loopSource && srcLen > 0) frame = std::fmod(frame, (double)srcLen); if (rev) frame = srcLen - 1 - frame; int64_t bin = (int64_t)((srcStart + frame) / lv.samplesPerBin); if (bin < 0 || bin >= (int64_t)lv.maxL.size()) continue; float mx = std::max(lv.maxL[bin], lv.maxR[bin]) * g, mn = std::min(lv.minL[bin], lv.minR[bin]) * g; p.drawLine(x, mid - (int)(clamp(mx, -1.f, 1.f) * hh), x, mid - (int)(clamp(mn, -1.f, 1.f) * hh)); }
            // fades
            p.setPen(QPen(Qt::white, 1)); if (c.fadeIn > 0) { int fx = tickToX(c.start + c.fadeIn); p.drawLine(r.left(), body.bottom(), fx, body.top()); p.setBrush(QColor(255, 255, 255, 30)); p.setPen(Qt::NoPen); p.drawPolygon(QPolygon({QPoint(r.left(), body.top()), QPoint(fx, body.top()), QPoint(r.left(), body.bottom())})); }
            p.setPen(QPen(Qt::white, 1)); if (c.fadeOut > 0) { int fx = tickToX(c.start + c.length - c.fadeOut); p.drawLine(fx, body.top(), r.right(), body.bottom()); p.setBrush(QColor(255, 255, 255, 30)); p.setPen(Qt::NoPen); p.drawPolygon(QPolygon({QPoint(fx, body.top()), QPoint(r.right(), body.top()), QPoint(r.right(), body.bottom())})); }
            p.setPen(Qt::white); p.setBrush(Qt::white); p.drawRect(r.left() + 2, r.top() + 15, 5, 5); p.drawRect(r.right() - 7, r.top() + 15, 5, 5);   // fade handles
        } else { p.setPen(theme::textDim()); p.drawText(body, Qt::AlignCenter, src && src->missing ? "MISSING FILE" : "loading..."); }
    } else if (c.type == ClipType::Automation) {
        if (auto* l = proj.lane(c.sourceId)) {
            p.setPen(QPen(col.lighter(160), 1.5)); int x0 = std::max(r.left() + 1, L.area.left()), x1 = std::min(r.right() - 1, L.area.right()); QPolygon poly; Tick len = std::max<Tick>(l->length, 1);
            for (int x = x0; x <= x1; x += 2) { Tick t = xToTick(x) - c.start + c.sourceOffset; if (c.loopSource) t %= len; float v = l->valueAt(t); poly << QPoint(x, r.bottom() - 4 - (int)(v * (r.height() - 8))); }
            p.drawPolyline(poly);
            p.setBrush(col.lighter(180)); p.setPen(theme::bg()); for (auto& pt : l->points) { int x = tickToX(c.start + pt.time - c.sourceOffset); if (x < r.left() || x > r.right()) continue; int y = r.bottom() - 4 - (int)(pt.value * (r.height() - 8)); p.drawEllipse(QPoint(x, y), 3, 3); }
        }
    }
    p.setClipping(false);
}
void PlaylistView::renderCache() {
    Layout L = layout(); m_cache = QPixmap(size()); m_cache.fill(theme::bg()); QPainter p(&m_cache); auto& proj = m_app.project(); const auto& ts = proj.transport.timeSig; Tick beat = ts.ticksPerBeat(), bar = ts.ticksPerBar();
    // tracks background
    p.setClipRect(L.area); int y = L.rulerH - m_scrollY;
    for (size_t i = 0; i < proj.playlistTracks.size(); ++i) { auto& t = proj.playlistTracks[i]; QRect tr(L.area.left(), y, L.area.width(), t.height); p.fillRect(tr, i % 2 ? QColor(0x1f, 0x22, 0x28) : QColor(0x22, 0x26, 0x2c)); if (t.mute) p.fillRect(tr, QColor(0, 0, 0, 80)); p.setPen(theme::grid()); p.drawLine(tr.left(), tr.bottom(), tr.right(), tr.bottom()); y += t.height; }
    Tick step = snapTicks(); if (step * m_ppt < 8) step = beat; if (beat * m_ppt < 8) step = bar; Tick last = xToTick(L.area.right());
    for (Tick t = (m_scrollTick / step) * step; t <= last; t += step) { int x = tickToX(t); p.setPen(t % bar == 0 ? theme::gridStrong().lighter(130) : t % beat == 0 ? theme::gridStrong() : theme::grid()); p.drawLine(x, L.area.top(), x, L.area.bottom()); }
    // loop / punch region
    if (proj.transport.loopEnabled) { int x1 = tickToX(proj.transport.loopStart), x2 = tickToX(proj.transport.loopEnd); p.fillRect(QRect(x1, L.area.top(), x2 - x1, L.area.height()), QColor(0xf2, 0xa9, 0x3b, 18)); }
    for (auto& c : proj.clips) drawClip(p, c);
    p.setClipping(false);
    // ruler
    p.fillRect(L.ruler, theme::panel()); p.setClipRect(L.ruler); p.setFont(theme::smallFont()); Tick lstep = bar; if (bar * m_ppt < 30) lstep = bar * 4; if (bar * m_ppt < 8) lstep = bar * 16;
    for (Tick t = (m_scrollTick / beat) * beat; t <= last; t += beat) { int x = tickToX(t); bool isBar = t % bar == 0; if (!isBar && beat * m_ppt < 6) continue; p.setPen(isBar ? theme::text() : theme::textDim()); p.drawLine(x, isBar ? L.ruler.top() + 8 : L.ruler.top() + 16, x, L.ruler.bottom()); if (isBar && t % lstep == 0) p.drawText(x + 3, L.ruler.top() + 12, QString::number(t / bar + 1)); }
    if (proj.transport.loopEnabled || true) { int x1 = tickToX(proj.transport.loopStart), x2 = tickToX(proj.transport.loopEnd); p.fillRect(QRect(x1, L.ruler.top(), x2 - x1, 5), proj.transport.loopEnabled ? theme::accent() : theme::textDim()); }
    if (proj.transport.punchEnabled) { int x1 = tickToX(proj.transport.punchIn), x2 = tickToX(proj.transport.punchOut); p.fillRect(QRect(x1, L.ruler.top() + 5, x2 - x1, 3), theme::red()); }
    for (auto& m : proj.markers) { int x = tickToX(m.position); p.setPen(theme::green()); p.setBrush(theme::green()); p.drawPolygon(QPolygon({QPoint(x, L.ruler.bottom()), QPoint(x - 5, L.ruler.bottom() - 7), QPoint(x + 5, L.ruler.bottom() - 7)})); p.drawText(x + 7, L.ruler.bottom() - 2, m.name); }
    p.setClipping(false);
    // headers
    p.fillRect(L.headers, theme::panel()); p.setClipRect(L.headers); y = L.rulerH - m_scrollY;
    for (size_t i = 0; i < proj.playlistTracks.size(); ++i) {
        auto& t = proj.playlistTracks[i]; QRect hr(0, y, L.headerW, t.height); p.fillRect(hr, i % 2 ? theme::panel() : theme::panelLight().darker(110)); p.fillRect(QRect(0, y, 5, t.height), t.color); p.setPen(theme::border()); p.drawLine(0, hr.bottom(), L.headerW, hr.bottom());
        p.setPen(theme::text()); p.setFont(theme::uiFont()); p.drawText(QRect(10, y + 3, L.headerW - 14, 16), Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(t.name, Qt::ElideRight, L.headerW - 16));
        auto btn = [&](QRect r, const QString& s, bool on, QColor c) { p.setPen(QPen(theme::border(), 1)); p.setBrush(on ? c : theme::bg()); p.drawRoundedRect(r, 2, 2); p.setPen(on ? theme::bg() : theme::textDim()); p.setFont(theme::smallFont()); p.drawText(r, Qt::AlignCenter, s); };
        if (t.height >= 40) { btn(QRect(10, y + 22, 18, 14), "M", t.mute, theme::red()); btn(QRect(31, y + 22, 18, 14), "S", t.solo, theme::accent()); btn(QRect(52, y + 22, 18, 14), "L", t.locked, theme::textDim()); int mi = proj.mixerIndex(t.mixerTrackId); btn(QRect(73, y + 22, 40, 14), mi <= 0 ? "> Master" : QString("> Ins %1").arg(mi), false, theme::bg()); }
        y += t.height;
    }
    p.setPen(theme::textDim()); p.setFont(theme::smallFont()); p.drawText(QRect(6, y + 4, L.headerW - 12, 14), Qt::AlignLeft, "+ double-click to add track");
    p.setClipping(false);
    p.fillRect(QRect(0, 0, L.headerW, L.rulerH), theme::panelLight()); p.setPen(theme::textDim()); p.drawText(QRect(0, 0, L.headerW, L.rulerH), Qt::AlignCenter, "Playlist");
    m_cacheValid = true;
}
void PlaylistView::paintEvent(QPaintEvent*) {
    if (!m_cacheValid) renderCache(); QPainter p(this); p.drawPixmap(0, 0, m_cache); Layout L = layout();
    if (m_playTick >= 0) { int x = tickToX((Tick)m_playTick); if (x >= L.area.left() && x <= L.area.right()) { p.setPen(QPen(theme::playhead(), 1.5)); p.drawLine(x, L.ruler.top(), x, L.area.bottom()); } }
    if (m_drag == Rubber) { p.setPen(QPen(theme::accent(), 1, Qt::DashLine)); p.setBrush(theme::selection()); p.drawRect(m_rubber.normalized()); }
    if (m_tool == Draw && m_drag == NoDrag && L.area.contains(m_hoverPos) && underMouse() && !clipAt(m_hoverPos)) { int tr = trackAtY(m_hoverPos.y()); if (tr >= 0) { if (auto* pat = m_app.project().pattern(m_app.project().selectedPatternId)) { Tick t = snap(xToTick(m_hoverPos.x())); QRect r(tickToX(t), trackTop(tr) + 1, std::max(3, (int)(pat->effectiveLength(m_app.project().transport.timeSig) * m_ppt)), trackH(tr) - 2); p.setPen(QPen(pat->color, 1, Qt::DotLine)); p.setBrush(Qt::NoBrush); p.drawRoundedRect(r, 3, 3); } } }
    p.setPen(theme::border()); p.drawRect(rect().adjusted(0, 0, -1, -1));
}

// ---------------------------------------------------------------- edit transactions
void PlaylistView::beginEdit() { m_before.clear(); for (auto& c : m_app.project().clips) if (m_selected.count(c.id)) m_before[c.id] = c; m_editing = true; }
void PlaylistView::commitEdit(const QString& desc) {
    if (!m_editing) return; m_editing = false; auto& proj = m_app.project(); std::vector<std::unique_ptr<Command>> cmds; std::vector<Clip> inserted;
    for (auto& c : proj.clips) { auto it = m_before.find(c.id); if (it == m_before.end()) { if (m_selected.count(c.id) && m_duplicated) inserted.push_back(c); continue; } Clip after = c; if (after.start == it->second.start && after.length == it->second.length && after.trackId == it->second.trackId && after.sourceOffset == it->second.sourceOffset && after.fadeIn == it->second.fadeIn && after.fadeOut == it->second.fadeOut && after.muted == it->second.muted) continue; c = it->second; cmds.push_back(std::make_unique<EditObjectCommand<Clip>>(m_app.model(), c.id, "", [after](Clip& x) { x = after; }, 0)); }
    if (m_duplicated) { for (auto& ins : inserted) { std::erase_if(proj.clips, [&](const Clip& x) { return x.id == ins.id; }); cmds.push_back(std::make_unique<InsertObjectCommand<Clip>>(m_app.model(), ins, "", -1)); } }
    m_duplicated = false; m_before.clear();
    if (cmds.empty()) { invalidate(); return; } m_app.model().macro(desc, std::move(cmds));
}

// ---------------------------------------------------------------- mouse
void PlaylistView::mousePressEvent(QMouseEvent* e) {
    setFocus(); Layout L = layout(); m_pressPos = m_lastPos = e->pos(); m_altHeld = e->modifiers() & Qt::AltModifier; auto& proj = m_app.project();
    if (e->button() == Qt::MiddleButton) { m_drag = Pan; return; }
    if (L.ruler.contains(e->pos())) { Tick t = std::max<Tick>(0, snap(xToTick(e->pos().x()), true)); if (e->button() == Qt::RightButton) { QMenu m; m.addAction("Add marker here", [this, t] { bool ok; QString n = QInputDialog::getText(this, "Marker", "Name:", QLineEdit::Normal, QString("Marker %1").arg(m_app.project().markers.size() + 1), &ok); if (ok) m_app.addMarker(t, n); }); for (auto& mk : proj.markers) if (std::abs(tickToX(mk.position) - e->pos().x()) < 8) { m.addAction("Rename marker '" + mk.name + "'", [this, id = mk.id, n = mk.name] { bool ok; QString nn = QInputDialog::getText(this, "Marker", "Name:", QLineEdit::Normal, n, &ok); if (ok) m_app.model().edit<Marker>(id, "Rename marker", [nn](Marker& x) { x.name = nn; }); }); m.addAction("Delete marker '" + mk.name + "'", [this, id = mk.id] { m_app.model().remove<Marker>(id, "Delete marker"); }); } m.addAction("Set punch in/out to loop", [this] { auto& t = m_app.project().transport; m_app.setPunch(true, t.loopStart, t.loopEnd); }); m.exec(e->globalPosition().toPoint()); return; } m_drag = LoopRange; m_loopDragStart = t; m_pressTick = t; return; }
    if (L.headers.contains(e->pos())) {
        int idx = trackAtY(e->pos().y()); if (idx < 0) return; auto& t = proj.playlistTracks[idx]; int y = trackTop(idx); int ly = e->pos().y() - y;
        if (ly >= t.height - 5) { m_drag = TrackHeight; m_heightTrack = idx; m_heightStart = t.height; return; }
        if (e->button() == Qt::RightButton) { showTrackMenu(idx, e->globalPosition().toPoint()); return; }
        if (t.height >= 40 && ly >= 22 && ly <= 36) { int x = e->pos().x(); if (x >= 10 && x < 28) m_app.editTrack(t.id, t.mute ? "Unmute track" : "Mute track", [](PlaylistTrack& x) { x.mute = !x.mute; }); else if (x >= 31 && x < 49) m_app.editTrack(t.id, "Solo track", [](PlaylistTrack& x) { x.solo = !x.solo; }); else if (x >= 52 && x < 70) m_app.editTrack(t.id, "Lock track", [](PlaylistTrack& x) { x.locked = !x.locked; }); else if (x >= 73 && x < 113) { QMenu m; for (auto& mt : proj.mixerTracks) { auto* a = m.addAction(mt.name, [this, tid = t.id, mid = mt.id] { m_app.editTrack(tid, "Track mixer routing", [mid](PlaylistTrack& x) { x.mixerTrackId = mid; }); }); a->setCheckable(true); a->setChecked(mt.id == t.mixerTrackId); } m.exec(e->globalPosition().toPoint()); } }
        return;
    }
    if (!L.area.contains(e->pos())) return;
    int edge = 0; Clip* c = clipAt(e->pos(), &edge); int trackIdx = trackAtY(e->pos().y());
    if (c && proj.track(c->trackId) && proj.track(c->trackId)->locked && m_tool != MuteTool) return;
    if (e->button() == Qt::RightButton) { if (c) { if (m_tool == Draw || m_tool == Erase) { m_selected.clear(); m_selected.insert(c->id); m_drag = Erasing; m_app.deleteClips({c->id}); m_selected.clear(); } else showClipMenu(c, e->globalPosition().toPoint()); } return; }
    if (m_tool == Erase) { if (c) { m_drag = Erasing; m_app.deleteClips({c->id}); } return; }
    if (m_tool == Slice) { if (c) m_app.splitClip(c->id, snap(xToTick(e->pos().x()))); return; }
    if (m_tool == MuteTool) { if (c) m_app.editClip(c->id, c->muted ? "Unmute clip" : "Mute clip", [](Clip& x) { x.muted = !x.muted; }); return; }
    if (c) {
        // automation point editing
        if (c->type == ClipType::Automation && !(e->modifiers() & Qt::ControlModifier)) { int pi = autoPointAt(*c, e->pos()); if (pi >= 0) { m_autoLane = c->sourceId; m_autoPoint = pi; m_dragClip = c->id; m_laneBefore = *proj.lane(m_autoLane); m_drag = AutoPoint; return; } }
        if (c->type == ClipType::Audio) { QRect r = clipRect(*c); if (e->pos().y() < r.top() + 22 && e->pos().y() >= r.top() + 14) { if (e->pos().x() <= r.left() + 9) { m_selected = {c->id}; beginEdit(); m_dragClip = c->id; m_drag = FadeIn; return; } if (e->pos().x() >= r.right() - 9) { m_selected = {c->id}; beginEdit(); m_dragClip = c->id; m_drag = FadeOut; return; } } }
        if (e->modifiers() & Qt::ControlModifier && !(e->modifiers() & Qt::ShiftModifier)) { if (m_selected.count(c->id)) m_selected.erase(c->id); else m_selected.insert(c->id); invalidate(); return; }
        if (!m_selected.count(c->id)) { if (!(e->modifiers() & Qt::ShiftModifier)) m_selected.clear(); m_selected.insert(c->id); }
        beginEdit(); m_dragClip = c->id; m_pressTick = xToTick(e->pos().x()); m_pressTrack = trackIdx; m_duplicated = false;
        if (e->modifiers() & Qt::ShiftModifier && e->modifiers() & Qt::ControlModifier) { std::vector<Clip> copies; for (auto& x : proj.clips) if (m_selected.count(x.id)) { Clip d = x; d.id = proj.newId(); copies.push_back(d); } m_selected.clear(); for (auto& d : copies) { proj.clips.push_back(d); m_selected.insert(d.id); } m_duplicated = true; m_dragClip = copies.empty() ? 0 : copies.front().id; }
        m_drag = edge == 1 ? ResizingR : edge == -1 ? ResizingL : Moving; invalidate(); return;
    }
    if (m_tool == Select || (e->modifiers() & Qt::ShiftModifier)) { if (!(e->modifiers() & Qt::ControlModifier)) m_selected.clear(); m_drag = Rubber; m_rubber = QRect(e->pos(), QSize(0, 0)); invalidate(); return; }
    // draw tool on empty: place the selected pattern
    if (trackIdx >= 0) { auto& t = proj.playlistTracks[trackIdx]; if (t.locked) return; Tick at = std::max<Tick>(0, snap(xToTick(e->pos().x()))); Id id = m_app.addPatternClip(t.id, proj.selectedPatternId, at); if (id) { m_selected = {id}; beginEdit(); m_dragClip = id; m_pressTick = xToTick(e->pos().x()); m_pressTrack = trackIdx; m_drag = Moving; } }
    else m_selected.clear(); invalidate();
}
void PlaylistView::mouseMoveEvent(QMouseEvent* e) {
    m_hoverPos = e->pos(); m_altHeld = e->modifiers() & Qt::AltModifier; Layout L = layout(); auto& proj = m_app.project();
    if (m_drag == NoDrag) { int edge = 0; Clip* c = L.area.contains(e->pos()) ? clipAt(e->pos(), &edge) : nullptr; if (m_tool == Draw || m_tool == Select) { if (L.headers.contains(e->pos())) { int idx = trackAtY(e->pos().y()); setCursor(idx >= 0 && e->pos().y() - trackTop(idx) >= trackH(idx) - 5 ? Qt::SizeVerCursor : Qt::ArrowCursor); } else setCursor(c ? (edge != 0 ? Qt::SizeHorCursor : Qt::SizeAllCursor) : (m_tool == Select ? Qt::CrossCursor : Qt::ArrowCursor)); } if (m_tool == Draw) update(); return; }
    if (m_drag == Pan) { int dx = e->pos().x() - m_lastPos.x(), dy = e->pos().y() - m_lastPos.y(); m_scrollTick = std::max<Tick>(0, m_scrollTick - (Tick)(dx / m_ppt)); m_scrollY = std::max(0, m_scrollY - dy); m_lastPos = e->pos(); updateScrollbars(); invalidate(); return; }
    if (m_drag == LoopRange) { Tick t = std::max<Tick>(0, snap(xToTick(e->pos().x()), true)); if (std::abs(e->pos().x() - m_pressPos.x()) > 3) { Tick a = std::min(m_loopDragStart, t), b = std::max(m_loopDragStart, t); if (b > a) { proj.transport.loopStart = a; proj.transport.loopEnd = b; proj.transport.loopEnabled = true; m_app.engine().setLoop(true, a, b); invalidate(); } } return; }
    if (m_drag == TrackHeight) { int h = clamp(m_heightStart + (e->pos().y() - m_pressPos.y()), 24, 300); proj.playlistTracks[m_heightTrack].height = h; updateScrollbars(); invalidate(); return; }
    if (m_drag == Rubber) { m_rubber = QRect(m_pressPos, e->pos()); QRect r = m_rubber.normalized(); for (auto& c : proj.clips) { if (r.intersects(clipRect(c))) m_selected.insert(c.id); else if (!(e->modifiers() & Qt::ControlModifier)) m_selected.erase(c.id); } invalidate(); return; }
    if (m_drag == Erasing) { if (Clip* c = clipAt(e->pos())) m_app.deleteClips({c->id}); return; }
    if (m_drag == AutoPoint) { auto* l = proj.lane(m_autoLane); const Clip* c = proj.clip(m_dragClip); if (!l || !c || m_autoPoint < 0 || m_autoPoint >= (int)l->points.size()) return; auto& pt = l->points[m_autoPoint]; pt.value = autoValueAtY(*c, e->pos().y()); Tick lo = m_autoPoint > 0 ? l->points[m_autoPoint - 1].time : 0, hi = m_autoPoint + 1 < (int)l->points.size() ? l->points[m_autoPoint + 1].time : LLONG_MAX; if (m_autoPoint > 0 && m_autoPoint + 1 < (int)l->points.size()) pt.time = clamp(snap(autoTickAtX(*c, e->pos().x())), lo, hi); invalidate(); return; }
    if (m_drag == FadeIn || m_drag == FadeOut) { Clip* c = proj.clip(m_dragClip); if (!c) return; Tick t = xToTick(e->pos().x()); if (m_drag == FadeIn) c->fadeIn = clamp<Tick>(t - c->start, 0, c->length); else c->fadeOut = clamp<Tick>(c->start + c->length - t, 0, c->length); invalidate(); return; }
    if (m_drag == Moving) {
        Tick dt = xToTick(e->pos().x()) - m_pressTick; int dtrack = trackAtY(e->pos().y()) >= 0 ? trackAtY(e->pos().y()) - m_pressTrack : 0;
        auto ab = m_before.find(m_dragClip); Tick anchorStart = ab != m_before.end() ? ab->second.start : (proj.clip(m_dragClip) ? proj.clip(m_dragClip)->start : 0); Tick delta = snap(anchorStart + dt) - anchorStart;
        Tick minStart = LLONG_MAX; int minTrack = INT_MAX, maxTrack = 0; std::map<Id, int> trackIndex; for (size_t i = 0; i < proj.playlistTracks.size(); ++i) trackIndex[proj.playlistTracks[i].id] = (int)i;
        auto origOf = [&](const Clip& c) { auto it = m_before.find(c.id); return it != m_before.end() ? it->second : c; };
        for (auto& c : proj.clips) if (m_selected.count(c.id)) { Clip o = origOf(c); minStart = std::min(minStart, o.start); int ti = trackIndex[o.trackId]; minTrack = std::min(minTrack, ti); maxTrack = std::max(maxTrack, ti); }
        if (minStart != LLONG_MAX && minStart + delta < 0) delta = -minStart; dtrack = clamp(dtrack, -minTrack, (int)proj.playlistTracks.size() - 1 - maxTrack);
        for (auto& c : proj.clips) if (m_selected.count(c.id)) { Clip o = origOf(c); if (m_before.find(c.id) == m_before.end() && m_duplicated) { /* duplicates: keep original position + delta relative to press */ } c.start = o.start + delta; int ti = trackIndex[o.trackId] + dtrack; if (ti >= 0 && ti < (int)proj.playlistTracks.size()) c.trackId = proj.playlistTracks[ti].id; }
        invalidate(); return;
    }
    if (m_drag == ResizingR || m_drag == ResizingL) {
        auto ab = m_before.find(m_dragClip); if (ab == m_before.end()) return; const Clip& o = ab->second; Tick t = snap(xToTick(e->pos().x()));
        for (auto& c : proj.clips) if (m_selected.count(c.id)) { auto it = m_before.find(c.id); if (it == m_before.end()) continue; const Clip& b = it->second; if (m_drag == ResizingR) { Tick newLen = std::max<Tick>(snapTicks() > 1 ? snapTicks() : PPQ / 16, t - o.start + (b.length - o.length)); c.length = newLen; } else { Tick newStart = clamp<Tick>(t, 0, b.start + b.length - PPQ / 16); Tick d = newStart - b.start; c.start = newStart; c.length = b.length - d; c.sourceOffset = std::max<Tick>(0, b.sourceOffset + d); } }
        invalidate(); return;
    }
}
void PlaylistView::mouseReleaseEvent(QMouseEvent* e) {
    auto& proj = m_app.project();
    switch (m_drag) {
    case Moving: commitEdit(m_duplicated ? "Duplicate clips" : "Move clips"); break; case ResizingR: case ResizingL: commitEdit("Resize clips"); break; case FadeIn: case FadeOut: commitEdit("Edit fade"); break;
    case LoopRange: { if (std::abs(e->pos().x() - m_pressPos.x()) <= 3) { m_app.seek(m_pressTick); } else { Tick a = proj.transport.loopStart, b = proj.transport.loopEnd; m_app.setLoopRange(a, b); if (!proj.transport.loopEnabled) m_app.setLoop(true); } break; }
    case TrackHeight: { int h = proj.playlistTracks[m_heightTrack].height; proj.playlistTracks[m_heightTrack].height = m_heightStart; m_app.editTrack(proj.playlistTracks[m_heightTrack].id, "Track height", [h](PlaylistTrack& t) { t.height = h; }); break; }
    case AutoPoint: { auto* l = proj.lane(m_autoLane); if (l) { AutomationLane after = *l; *l = m_laneBefore; m_app.editLane(m_autoLane, "Move automation point", [after](AutomationLane& x) { x = after; }); } break; }
    case Rubber: if (m_rubber.normalized().width() < 3 && m_rubber.normalized().height() < 3 && !(e->modifiers() & Qt::ControlModifier)) m_selected.clear(); break;
    default: break;
    }
    m_drag = NoDrag; m_dragClip = 0; m_autoPoint = -1; invalidate();
}
void PlaylistView::mouseDoubleClickEvent(QMouseEvent* e) {
    Layout L = layout(); auto& proj = m_app.project();
    if (L.headers.contains(e->pos())) { int idx = trackAtY(e->pos().y()); if (idx < 0) { m_app.addPlaylistTrack(); return; } auto& t = proj.playlistTracks[idx]; bool ok; QString n = QInputDialog::getText(this, "Rename track", "Name:", QLineEdit::Normal, t.name, &ok); if (ok && !n.isEmpty()) m_app.editTrack(t.id, "Rename track", [n](PlaylistTrack& x) { x.name = n; }); return; }
    if (L.ruler.contains(e->pos())) { zoomToFit(); return; }
    if (Clip* c = clipAt(e->pos())) {
        if (c->type == ClipType::Automation) { auto* l = proj.lane(c->sourceId); if (!l) return; AutomationPoint pt; pt.time = snap(autoTickAtX(*c, e->pos().x())); pt.value = autoValueAtY(*c, e->pos().y()); m_app.editLane(l->id, "Add automation point", [pt](AutomationLane& x) { x.points.push_back(pt); std::sort(x.points.begin(), x.points.end(), [](const AutomationPoint& a, const AutomationPoint& b) { return a.time < b.time; }); if (x.length < pt.time) x.length = pt.time; }); return; }
        if (c->type == ClipType::Pattern) { m_app.selectPattern(c->sourceId); Q_EMIT openPattern(c->sourceId); return; }
        if (c->type == ClipType::Audio) { Q_EMIT openAudioClip(c->id); return; }
    }
}
void PlaylistView::wheelEvent(QWheelEvent* e) {
    int dy = e->angleDelta().y(); Layout L = layout();
    if (e->modifiers() & Qt::ControlModifier) { Tick anchor = xToTick(e->position().toPoint().x()); m_ppt = clamp(m_ppt * (dy > 0 ? 1.15 : 1 / 1.15), 0.002, 1.0); m_scrollTick = std::max<Tick>(0, anchor - (Tick)((e->position().toPoint().x() - L.headerW) / m_ppt)); }
    else if (e->modifiers() & Qt::ShiftModifier || e->angleDelta().x() != 0) { int d = e->angleDelta().x() != 0 ? e->angleDelta().x() : dy; m_scrollTick = std::max<Tick>(0, m_scrollTick - (Tick)(d / m_ppt * 0.5)); }
    else m_scrollY = clamp(m_scrollY - dy / 2, 0, m_vbar->maximum());
    updateScrollbars(); invalidate(); e->accept();
}
void PlaylistView::keyPressEvent(QKeyEvent* e) {
    bool ctrl = e->modifiers() & Qt::ControlModifier;
    switch (e->key()) { case Qt::Key_Delete: case Qt::Key_Backspace: deleteSelected(); return; case Qt::Key_A: if (ctrl) { selectAll(); return; } break; case Qt::Key_D: if (ctrl) { duplicateSelected(); return; } break; case Qt::Key_M: if (!m_selected.empty()) { bool any = false; for (auto& c : m_app.project().clips) if (m_selected.count(c.id) && !c.muted) any = true; muteSelected(any); return; } break; case Qt::Key_Escape: m_selected.clear(); invalidate(); return; case Qt::Key_1: setTool(Draw); return; case Qt::Key_2: setTool(Select); return; case Qt::Key_3: setTool(Erase); return; case Qt::Key_4: setTool(Slice); return; case Qt::Key_5: setTool(MuteTool); return; default: break; }
    QWidget::keyPressEvent(e);
}
void PlaylistView::contextMenuEvent(QContextMenuEvent* e) { Layout L = layout(); if (!L.area.contains(e->pos()) || m_drag != NoDrag) return; if (clipAt(e->pos())) return; QMenu m(this); m.addAction("Select all", [this] { selectAll(); }); m.addAction("Add track", [this] { m_app.addPlaylistTrack(); }); m.addAction("Zoom to fit", [this] { zoomToFit(); }); int tr = trackAtY(e->pos().y()); Tick at = snap(xToTick(e->pos().x())); if (tr >= 0) { Id tid = m_app.project().playlistTracks[tr].id; auto* pm = m.addMenu("Insert pattern"); for (auto& p : m_app.project().patterns) pm->addAction(p.name, [this, tid, pid = p.id, at] { m_app.addPatternClip(tid, pid, at); }); auto* am = m.addMenu("Insert automation clip"); for (auto& l : m_app.project().automationLanes) am->addAction(l.name, [this, tid, lid = l.id, at] { m_app.addAutomationClip(tid, lid, at, 0); }); } m.exec(e->globalPos()); }
void PlaylistView::showTrackMenu(int idx, const QPoint& global) {
    auto& proj = m_app.project(); if (idx < 0 || idx >= (int)proj.playlistTracks.size()) return; PlaylistTrack t = proj.playlistTracks[idx]; Id id = t.id; QMenu m(this);
    m.addAction("Rename...", [this, id, t] { bool ok; QString n = QInputDialog::getText(this, "Rename track", "Name:", QLineEdit::Normal, t.name, &ok); if (ok && !n.isEmpty()) m_app.editTrack(id, "Rename track", [n](PlaylistTrack& x) { x.name = n; }); });
    m.addAction("Color...", [this, id, t] { QColor c = QColorDialog::getColor(t.color, this); if (c.isValid()) m_app.editTrack(id, "Track color", [c](PlaylistTrack& x) { x.color = c; }); });
    m.addAction("Insert track above", [this, idx] { Id nid = m_app.addPlaylistTrack(); m_app.model().reorder<PlaylistTrack>(nid, idx, "Move track"); }); m.addAction("Add track", [this] { m_app.addPlaylistTrack(); }); m.addAction("Delete track", [this, id] { m_app.deletePlaylistTrack(id); });
    m.addSeparator(); auto* route = m.addMenu("Audio clips to mixer track"); for (auto& mt : proj.mixerTracks) { auto* a = route->addAction(mt.name, [this, id, mid = mt.id] { m_app.editTrack(id, "Track mixer routing", [mid](PlaylistTrack& x) { x.mixerTrackId = mid; }); }); a->setCheckable(true); a->setChecked(mt.id == t.mixerTrackId); }
    auto* h = m.addMenu("Height"); for (int hh : {30, 60, 100, 160}) h->addAction(QString::number(hh), [this, id, hh] { m_app.editTrack(id, "Track height", [hh](PlaylistTrack& x) { x.height = hh; }); });
    m.exec(global);
}
void PlaylistView::showClipMenu(Clip* c, const QPoint& global) {
    if (!c) return; if (!m_selected.count(c->id)) { m_selected = {c->id}; invalidate(); } Id id = c->id; ClipType type = c->type; QMenu m(this); auto& proj = m_app.project();
    m.addAction(c->muted ? "Unmute" : "Mute", [this, id] { m_app.editClip(id, "Mute clip", [](Clip& x) { x.muted = !x.muted; }); }); m.addAction("Delete", [this] { deleteSelected(); }); m.addAction("Duplicate", [this] { duplicateSelected(); });
    m.addAction("Split at playhead", [this, id] { m_app.splitClip(id, (Tick)m_app.engine().positionTicks()); }); m.addAction("Rename...", [this, id, n = c->name] { bool ok; QString nn = QInputDialog::getText(this, "Rename clip", "Name:", QLineEdit::Normal, n, &ok); if (ok) m_app.editClip(id, "Rename clip", [nn](Clip& x) { x.name = nn; }); });
    m.addSeparator();
    if (type == ClipType::Pattern) { m.addAction("Make unique (clone pattern)", [this, id] { m_app.makeClipUnique(id); }); m.addAction("Open in piano roll", [this, pid = c->sourceId] { m_app.selectPattern(pid); Q_EMIT openPattern(pid); }); m.addAction(c->loopSource ? "Don't repeat pattern" : "Repeat pattern", [this, id] { m_app.editClip(id, "Repeat pattern", [](Clip& x) { x.loopSource = !x.loopSource; }); }); }
    if (type == ClipType::Audio) {
        m.addAction("Edit audio clip...", [this, id] { Q_EMIT openAudioClip(id); });
        m.addAction(c->reverse ? "Un-reverse" : "Reverse", [this, id] { m_app.editClip(id, "Reverse clip", [](Clip& x) { x.reverse = !x.reverse; }); });
        m.addAction("Normalize (source)", [this, sid = c->sourceId] { m_app.editAudioSource(sid, "Normalize", [](AudioSource& a) { a.normalize = !a.normalize; }); });
        m.addAction("Gain...", [this, id, g = c->gain] { bool ok; double db = QInputDialog::getDouble(this, "Clip gain", "dB:", gainToDb(g), -60, 24, 1, &ok); if (ok) m_app.editClip(id, "Clip gain", [db](Clip& x) { x.gain = dbToGain((float)db); }); });
        m.addAction("Fade in...", [this, id, c] { bool ok; double beats = QInputDialog::getDouble(this, "Fade in", "Beats:", ticksToBeats(c->fadeIn), 0, 64, 2, &ok); if (ok) m_app.editClip(id, "Fade in", [beats](Clip& x) { x.fadeIn = beatsToTicks(beats); }); });
        m.addAction("Fade out...", [this, id, c] { bool ok; double beats = QInputDialog::getDouble(this, "Fade out", "Beats:", ticksToBeats(c->fadeOut), 0, 64, 2, &ok); if (ok) m_app.editClip(id, "Fade out", [beats](Clip& x) { x.fadeOut = beatsToTicks(beats); }); });
        m.addAction("Crossfade with overlapping clip", [this] { crossfadeSelected(); });
        m.addAction("Time stretch / pitch...", [this, id] { Q_EMIT openAudioClip(id); });
        m.addAction("Fit to tempo (stretch to whole bars)", [this, id, c] { const AudioSource* s = m_app.project().audioSource(c->sourceId); if (!s || s->sampleRate <= 0) return; double secs = s->frames / s->sampleRate; Tick nat = secondsToTicks(secs, m_app.project().transport.bpm); Tick bar = m_app.project().transport.timeSig.ticksPerBar(); Tick target = std::max<Tick>(bar, ((nat + bar / 2) / bar) * bar); double ratio = (double)target / nat; m_app.editClip(id, "Fit to tempo", [ratio, target](Clip& x) { x.timeStretch = ratio; x.length = target; }); });
        m.addAction("Slice at transients", [this, id, c] { const AudioSource* s = m_app.project().audioSource(c->sourceId); auto a = s ? m_app.cache().get(s->filePath) : nullptr; if (!a) return; std::vector<Tick> cuts; float env = 0; int64_t last = -100000; for (int64_t i = 0; i < a->frames; ++i) { float x = std::fabs(a->L()[i]); if (x > env * 3.f && x > 0.15f && i - last > a->sampleRate * 0.08) { last = i; cuts.push_back(secondsToTicks(i / a->sampleRate, m_app.project().transport.bpm)); } env = env * 0.999f + x * 0.001f; } Tick start = c->start; for (auto it = cuts.rbegin(); it != cuts.rend(); ++it) { if (*it <= 0) continue; m_app.splitClip(id, start + *it); } });
        m.addAction(c->loopSource ? "Don't loop audio" : "Loop audio", [this, id] { m_app.editClip(id, "Loop audio", [](Clip& x) { x.loopSource = !x.loopSource; }); });
    }
    if (type == ClipType::Automation) { m.addAction("Delete automation lane", [this, lid = c->sourceId] { m_app.deleteLane(lid); }); auto* cm = m.addMenu("Curve type (all points)"); const char* names[] = {"Linear", "Stepped", "Smooth", "Bezier"}; for (int i = 0; i < 4; ++i) cm->addAction(names[i], [this, lid = c->sourceId, i] { m_app.editLane(lid, "Curve type", [i](AutomationLane& l) { for (auto& p : l.points) p.curve = (CurveType)i; }); }); int pi = autoPointAt(*c, mapFromGlobal(global)); if (pi >= 0) { m.addAction("Delete point", [this, lid = c->sourceId, pi] { m_app.editLane(lid, "Delete point", [pi](AutomationLane& l) { if (l.points.size() > 1 && pi < (int)l.points.size()) l.points.erase(l.points.begin() + pi); }); }); auto* pcm = m.addMenu("Point curve"); for (int i = 0; i < 4; ++i) pcm->addAction(names[i], [this, lid = c->sourceId, pi, i] { m_app.editLane(lid, "Curve type", [pi, i](AutomationLane& l) { if (pi < (int)l.points.size()) l.points[pi].curve = (CurveType)i; }); }); m.addAction("Tension...", [this, lid = c->sourceId, pi] { bool ok; double t = QInputDialog::getDouble(this, "Tension", "-1..1:", 0, -1, 1, 2, &ok); if (ok) m_app.editLane(lid, "Tension", [pi, t](AutomationLane& l) { if (pi < (int)l.points.size()) { l.points[pi].tension = (float)t; l.points[pi].curve = CurveType::Bezier; } }); }); } }
    (void)proj; m.exec(global);
}
void PlaylistView::deleteSelected() { std::vector<Id> ids(m_selected.begin(), m_selected.end()); m_selected.clear(); m_app.deleteClips(ids); }
void PlaylistView::selectAll() { for (auto& c : m_app.project().clips) m_selected.insert(c.id); invalidate(); }
void PlaylistView::muteSelected(bool m) { std::vector<std::unique_ptr<Command>> cmds; for (Id id : m_selected) cmds.push_back(std::make_unique<EditObjectCommand<Clip>>(m_app.model(), id, "", [m](Clip& x) { x.muted = m; }, 0)); if (!cmds.empty()) m_app.model().macro(m ? "Mute clips" : "Unmute clips", std::move(cmds)); }
void PlaylistView::duplicateSelected() { auto& proj = m_app.project(); if (m_selected.empty()) return; Tick minS = LLONG_MAX, maxE = 0; for (auto& c : proj.clips) if (m_selected.count(c.id)) { minS = std::min(minS, c.start); maxE = std::max(maxE, c.start + c.length); } Tick span = maxE - minS; std::vector<std::unique_ptr<Command>> cmds; std::set<Id> newSel; for (auto& c : proj.clips) if (m_selected.count(c.id)) { Clip d = c; d.id = proj.newId(); d.start += span; newSel.insert(d.id); cmds.push_back(std::make_unique<InsertObjectCommand<Clip>>(m_app.model(), d, "", -1)); } m_app.model().macro("Duplicate clips", std::move(cmds)); m_selected = newSel; invalidate(); }
void PlaylistView::crossfadeSelected() {
    auto& proj = m_app.project(); std::vector<Clip*> sel; for (auto& c : proj.clips) if (m_selected.count(c.id) && c.type == ClipType::Audio) sel.push_back(&c); if (sel.size() != 2) { m_app.statusMessage("Select exactly two overlapping audio clips", 3000); return; }
    std::sort(sel.begin(), sel.end(), [](Clip* a, Clip* b) { return a->start < b->start; }); Clip* a = sel[0]; Clip* b = sel[1]; Tick overlap = (a->start + a->length) - b->start; if (overlap <= 0) { m_app.statusMessage("Clips do not overlap", 3000); return; }
    std::vector<std::unique_ptr<Command>> cmds; cmds.push_back(std::make_unique<EditObjectCommand<Clip>>(m_app.model(), a->id, "", [overlap](Clip& x) { x.fadeOut = overlap; }, 0)); cmds.push_back(std::make_unique<EditObjectCommand<Clip>>(m_app.model(), b->id, "", [overlap](Clip& x) { x.fadeIn = overlap; }, 0)); m_app.model().macro("Crossfade", std::move(cmds));
}
void PlaylistView::dragEnterEvent(QDragEnterEvent* e) { if (e->mimeData()->hasUrls() || e->mimeData()->hasFormat("application/x-musicthingy-item")) e->acceptProposedAction(); }
void PlaylistView::dragMoveEvent(QDragMoveEvent* e) { e->acceptProposedAction(); }
void PlaylistView::dropEvent(QDropEvent* e) {
    QPoint pos = e->position().toPoint(); int tr = trackAtY(pos.y()); auto& proj = m_app.project(); if (tr < 0) { if (proj.playlistTracks.empty()) m_app.addPlaylistTrack(); tr = (int)proj.playlistTracks.size() - 1; } Id tid = proj.playlistTracks[tr].id; Tick at = std::max<Tick>(0, snap(xToTick(pos.x())));
    QString payload = QString::fromUtf8(e->mimeData()->data("application/x-musicthingy-item")); QString kind = payload.section('|', 0, 0), val = payload.section('|', 1);
    if (e->mimeData()->hasUrls()) { for (auto& u : e->mimeData()->urls()) { QString p = u.toLocalFile(); if (AudioFile::isAudioFile(p)) { m_app.addAudioClip(tid, p, at); } else if (p.endsWith(".mtp")) m_app.openProject(p); } }
    else if (kind == "pattern") m_app.addPatternClip(tid, val.toULongLong(), at);
    else if (kind == "lane") m_app.addAutomationClip(tid, val.toULongLong(), at, 0);
    e->acceptProposedAction();
}

// ---------------------------------------------------------------- panel
PlaylistPanel::PlaylistPanel(App& app, QWidget* parent) : QWidget(parent), m_app(app) {
    auto* lay = new QVBoxLayout(this); lay->setContentsMargins(2, 2, 2, 2); lay->setSpacing(2); auto* top = new QHBoxLayout(); top->setSpacing(4);
    const char* names[] = {"✎", "⬚", "✖", "✂", "M"}; const char* tips[] = {"Draw (1): click to place the selected pattern; drag clips to move, edges to resize; right-click deletes", "Select (2)", "Erase (3)", "Slice (4): click a clip to split it", "Mute (5): click a clip to toggle mute"};
    for (int i = 0; i < 5; ++i) { auto* b = new QToolButton(this); b->setText(names[i]); b->setToolTip(tips[i]); b->setCheckable(true); b->setChecked(i == 0); top->addWidget(b); m_tools.push_back(b); connect(b, &QToolButton::clicked, this, [this, i] { m_view->setTool((PlaylistView::Tool)i); for (int k = 0; k < 5; ++k) m_tools[k]->setChecked(k == i); }); }
    top->addSpacing(8); auto* l = new QLabel("Snap", this); l->setStyleSheet("color:#8a919a;"); top->addWidget(l); m_snap = TransportBar::makeSnapCombo(this); m_snap->setCurrentIndex(1); top->addWidget(m_snap);
    auto* addTrack = new QToolButton(this); addTrack->setText("+ Track"); top->addWidget(addTrack); auto* fit = new QToolButton(this); fit->setText("Fit"); fit->setToolTip("Zoom to fit the song"); top->addWidget(fit);
    auto* hint = new QLabel("Drag samples / patterns / automation from the Browser. Ctrl+Shift+drag duplicates. Drag the ruler to set the loop.", this); hint->setStyleSheet("color:#6a717a;"); top->addWidget(hint); top->addStretch(1); lay->addLayout(top);
    m_view = new PlaylistView(app, this); lay->addWidget(m_view, 1); m_view->setSnapDivision(-1);
    connect(m_snap, &QComboBox::currentIndexChanged, this, [this](int i) { m_view->setSnapDivision(TransportBar::snapDivisionFromIndex(i)); m_view->refresh(); });
    connect(addTrack, &QToolButton::clicked, this, [this] { m_app.addPlaylistTrack(); }); connect(fit, &QToolButton::clicked, this, [this] { m_view->zoomToFit(); });
    connect(m_view, &PlaylistView::openPattern, this, &PlaylistPanel::openPattern); connect(m_view, &PlaylistView::openAudioClip, this, &PlaylistPanel::openAudioClip);
    connect(&app.cache(), &SampleCache::assetLoaded, this, [this](const QString&) { m_view->refresh(); });
}
void PlaylistPanel::refresh(uint32_t) { m_view->refresh(); }
void PlaylistPanel::uiTick() { m_view->uiTick(); }

} // namespace mt
