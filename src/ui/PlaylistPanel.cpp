#include "ui/PlaylistPanel.h"
#include "ui/AudioEditor.h"
#include "project/MidiFile.h"
#include "ui/InAppDrag.h"
#include "dsp/DspUtils.h"
#include <QPainterPath>
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
#include <QDateTime>

namespace mt {

PlaylistView::PlaylistView(App& app, QWidget* parent)
    : TimelineView(app, PPQ / 4, parent) {
    m_snapDiv = -1;
    m_ppt = 0.03;
    setAcceptDrops(true);
}
PlaylistView::Layout PlaylistView::layout() const {
    Layout l;
    int w = width() - 14, h = height() - 14;
    l.ruler = QRect(l.headerW, 0, w - l.headerW, 24);
    l.rulerH = m_tempoOpen ? 44 : 24;
    l.tempo = QRect(l.headerW, 24, w - l.headerW, l.rulerH - 24);
    l.headers = QRect(0, l.rulerH, l.headerW, h - l.rulerH);
    l.area = QRect(l.headerW, l.rulerH, w - l.headerW, h - l.rulerH);
    return l;
}
int PlaylistView::trackH(int idx) const {
    auto& tr = m_app.project().playlistTracks;
    return (int)std::lround(((idx >= 0 && idx < (int)tr.size()) ? tr[idx].height : 60) * m_vzoom);
}
int PlaylistView::trackTop(int idx) const {
    int y = layout().rulerH - m_scrollY;
    for (int i = 0; i < idx; ++i) y += trackH(i);
    return y;
}
int PlaylistView::trackAtY(int y) const {
    auto& tr = m_app.project().playlistTracks;
    int top = layout().rulerH - m_scrollY;
    for (int i = 0; i < (int)tr.size(); ++i) {
        int h = trackH(i);
        if (y >= top && y < top + h) return i;
        top += h;
    }
    return -1;
}
Tick PlaylistView::snapRound(Tick t) const {
    if (snapTicks() > 1) return snapToGrid(t, true);
    return ((t + PPQ / 8) / (PPQ / 4)) * (PPQ / 4);
}   // nearest grid line (rounds up past halfway)
void PlaylistView::selectInRect(const QRect& r) {
    for (auto& c : m_app.project().clips)
        if (r.intersects(clipRect(c))) m_selected.insert(c.id);
}
QRect PlaylistView::clipRect(const Clip& c) const {
    auto& tr = m_app.project().playlistTracks;
    int idx = -1;
    for (size_t i = 0; i < tr.size(); ++i)
        if (tr[i].id == c.trackId) idx = (int)i;
    if (idx < 0) return QRect();
    return QRect(tickToX(c.start), trackTop(idx) + 1, std::max(3, (int)(m_app.project().clipLength(c) * m_ppt)), trackH(idx) - 2);
}
Clip* PlaylistView::clipAt(const QPoint& p, int* edge) const {
    auto& clips = m_app.project().clips;
    for (auto it = clips.rbegin(); it != clips.rend(); ++it) {
        QRect r = clipRect(*it);
        if (r.contains(p)) {
            if (edge) {
                *edge = 0;
                if (r.width() > 14) {
                    if (p.x() >= r.right() - 6) *edge = 1;
                    else if (p.x() <= r.left() + 5) *edge = -1;
                }
            }
            return const_cast<Clip*>(&*it);
        }
    }
    return nullptr;
}
void PlaylistView::updateScrollbars() {
    m_updatingBars = true;
    Tick visible = (Tick)(layout().area.width() / m_ppt);
    Tick len = std::max<Tick>(maxExtent(), m_scrollTick + visible); /* never pull the view back: the current viewport is always reachable */
    int hmax = (int)std::max<Tick>(0, (len - visible) * 4 / PPQ);
    m_hbar->setRange(0, hmax);
    m_hbar->setPageStep((int)(visible * 4 / PPQ));
    if (m_scrollTick * 4 / PPQ > hmax) m_scrollTick = (Tick)hmax * PPQ / 4;
    m_hbar->setValue((int)(m_scrollTick * 4 / PPQ));
    int total = 0;
    for (size_t i = 0; i < m_app.project().playlistTracks.size(); ++i) total += trackH((int)i);
    int vmax = std::max(0, total + 60 - layout().area.height());
    m_vbar->setRange(0, vmax);
    m_vbar->setPageStep(layout().area.height());
    m_scrollY = std::min(m_scrollY, vmax);
    m_vbar->setValue(m_scrollY);
    m_updatingBars = false;
}
void PlaylistView::refresh() {
    updateScrollbars();
    invalidate();
}
// Scrollable / zoomable extent: the end of the content plus a 1000-bar margin (so an empty song shows up to 1000 bars)
Tick PlaylistView::maxExtent() const {
    Tick bar = m_app.project().transport.timeSig.ticksPerBar();
    Tick end = m_app.project().songLength();
    return ((end + bar - 1) / bar) * bar + bar * 1000;
}
void PlaylistView::zoomToFit() {
    Tick len = std::max<Tick>(m_app.project().songLength(), PPQ * 16);
    m_ppt = clamp((double)layout().area.width() / (len + PPQ * 4), 0.0002, 1.0);
    m_scrollTick = 0;
    refresh();
}
void PlaylistView::uiTick() {
    auto tv = m_app.engine().transport();
    double pt = tv.songMode ? tv.positionTicks : -1;
    if (pt != m_playTick) {
        m_playTick = pt;
        update();
    }
}

// ---------------------------------------------------------------- automation helpers
int PlaylistView::autoPointAt(const Clip& c, const QPoint& p) const {
    const AutomationLane* l = m_app.project().lane(c.sourceId);
    if (!l) return -1;
    QRect r = clipRect(c);
    for (int i = 0; i < (int)l->points.size(); ++i) {
        int x = tickToX(c.start + l->points[i].time - c.sourceOffset);
        int y = r.bottom() - 4 - (int)(l->points[i].value * (r.height() - 8));
        if (std::abs(x - p.x()) <= 5 && std::abs(y - p.y()) <= 5) return i;
    }
    return -1;
}
float PlaylistView::autoValueAtY(const Clip& c, int y) const {
    QRect r = clipRect(c);
    return clamp((float)(r.bottom() - 4 - y) / (r.height() - 8), 0.f, 1.f);
}
Tick PlaylistView::autoTickAtX(const Clip& c, int x) const {
    return clamp<Tick>(xToTick(x) - c.start + c.sourceOffset, 0, c.length + c.sourceOffset);
}

// ---------------------------------------------------------------- tempo lane & time signatures
std::pair<double, double> PlaylistView::tempoRange() const {
    if (m_drag == TempoPointDrag) return m_tempoRange;
    const auto& t = m_app.project().transport;
    double lo = t.bpm, hi = t.bpm;
    for (auto& p : t.tempoPoints) {
        lo = std::min(lo, p.bpm);
        hi = std::max(hi, p.bpm);
    }
    double pad = std::max(0.0, 20.0 - (hi - lo)) / 2;   // at least 20 BPM tall
    return {lo - pad, hi + pad};
}
int PlaylistView::tempoY(double bpm) const {
    QRect r = layout().tempo;
    auto [lo, hi] = tempoRange();
    return r.bottom() - 3 - (int)std::lround((bpm - lo) / (hi - lo) * (r.height() - 7));
}
int PlaylistView::tempoPointAt(const QPoint& p) const {
    const auto& pts = m_app.project().transport.tempoPoints;
    for (int i = 0; i < (int)pts.size(); ++i)
        if (std::abs(tickToX(pts[i].tick) - p.x()) <= 5) return i;
    return -1;
}
QRect PlaylistView::tempoToggleRect(const Layout& L) const {
    return m_tempoOpen ? QRect(0, L.tempo.top(), L.headerW, L.tempo.height()) : QRect(0, 0, L.headerW, L.ruler.height());
}
void PlaylistView::paintTempoLane(QPainter& p, const Layout& L) {
    if (!m_tempoOpen) return;
    const auto& tr = m_app.project().transport;
    TempoMap m = tr.tempoMap();
    p.save();
    p.setClipRect(L.tempo);
    p.fillRect(L.tempo, theme::bg());
    p.setPen(theme::border());
    p.drawLine(L.tempo.topLeft(), L.tempo.topRight());
    p.setRenderHint(QPainter::Antialiasing);
    QPolygonF curve;   // sampled per pixel: jumps come out as steps, ramps as slopes
    for (int x = L.tempo.left(); x <= L.tempo.right(); ++x) curve << QPointF(x, tempoY(m.bpmAt((double)xToTick(x))));
    p.setPen(QPen(theme::accent(), 1.5));
    p.drawPolyline(curve);
    p.setFont(theme::smallFont());
    for (auto& pt : tr.tempoPoints) {
        int x = tickToX(pt.tick);
        p.setBrush(pt.ramp ? theme::accent() : theme::bg());   // filled: ramps to the next point
        p.drawEllipse(QPoint(x, tempoY(pt.bpm)), 3, 3);
        p.drawText(x + 6, L.tempo.top() + 10, QString::number(pt.bpm, 'f', pt.bpm == std::round(pt.bpm) ? 0 : 1));
    }
    p.restore();
}
void PlaylistView::showTempoMenu(int idx, const QPoint& global) {
    const auto& pts = m_app.project().transport.tempoPoints;
    if (idx < 0 || idx >= (int)pts.size()) return;
    QMenu m(this);
    QAction* ramp = m.addAction("Ramp to the next point", [this, idx] {
        m_app.editTempo("Tempo ramp", [idx](TransportSettings& t) { t.tempoPoints[idx].ramp = !t.tempoPoints[idx].ramp; });
    });
    ramp->setCheckable(true);
    ramp->setChecked(pts[idx].ramp);
    ramp->setEnabled(idx + 1 < (int)pts.size());
    m.addAction("Delete tempo point", [this, idx] {
        m_app.editTempo("Delete tempo point", [idx](TransportSettings& t) { t.tempoPoints.erase(t.tempoPoints.begin() + idx); });
    });
    m.exec(global);
}
void PlaylistView::addTimeSigActions(QMenu& m, int x) {
    const auto& tr = m_app.project().transport;
    TempoMap tm = tr.tempoMap();
    Tick t = std::max<Tick>(0, xToTick(x));
    int64_t b = tm.barAt(t);
    if (t - tm.barStart(b) > (tm.barStart(b + 1) - tm.barStart(b)) / 2) ++b;   // the nearest bar line
    int bar = (int)std::max<int64_t>(b, 1);   // bar 1's signature is the transport bar's
    m.addAction(QString("Insert time signature change here (bar %1)...").arg(bar + 1), [this, bar, cur = tm.sigAt(tm.barStart(bar))] {
        bool ok;
        QString s = QInputDialog::getText(this, "Time signature", QString("Time signature from bar %1 (beats/unit):").arg(bar + 1),
            QLineEdit::Normal, QString("%1/%2").arg(cur.numerator).arg(cur.denominator), &ok);
        QStringList nd = s.split('/');
        int n = nd.size() == 2 ? nd[0].trimmed().toInt() : 0, d = nd.size() == 2 ? nd[1].trimmed().toInt() : 0;
        if (!ok || n < 1 || n > 32 || d < 1 || d > 32) return;
        m_app.model().editTransport("Time signature change", [bar, n, d](TransportSettings& t) {
            std::erase_if(t.timeSigChanges, [bar](const TimeSigChange& c) { return c.bar == bar; });
            auto it = std::lower_bound(
                t.timeSigChanges.begin(), t.timeSigChanges.end(), bar, [](const TimeSigChange& c, int v) { return c.bar < v; });
            t.timeSigChanges.insert(it, {bar, {n, d}});
        });
    });
    for (auto& c : tr.timeSigChanges)
        if (std::abs(tickToX(tm.barStart(c.bar)) - x) < 8)
            m.addAction(QString("Delete time signature %1/%2 (bar %3)").arg(c.sig.numerator).arg(c.sig.denominator).arg(c.bar + 1),
                [this, bar = c.bar] {
                    m_app.model().editTransport("Delete time signature change", [bar](TransportSettings& t) {
                        std::erase_if(t.timeSigChanges, [bar](const TimeSigChange& x) { return x.bar == bar; });
                    });
                });
}

// ---------------------------------------------------------------- painting
// A clip is coloured by what it points at: its pattern, its automation lane, or a fixed tint for audio.
QColor PlaylistView::clipColor(const Clip& c) const {
    auto& proj = m_app.project();
    if (c.type == ClipType::Pattern) {
        if (auto* pat = proj.pattern(c.sourceId)) return pat->color;
    } else if (c.type == ClipType::Audio) return QColor("#5ad1c9");
    else if (c.type == ClipType::Automation) {
        if (auto* l = proj.lane(c.sourceId)) return l->color;
    }
    return theme::accent2();
}
// Audio clip handles are lines: the fade-in / fade-out curves (grab anywhere on the curve) and the horizontal gain line
QPolygonF PlaylistView::fadeCurve(const Clip& c, int which) const {
    QRect r = clipRect(c);
    QRect body = r.adjusted(1, 14, -1, -1);
    Tick len = m_app.project().clipLength(c);
    bool in = which == 1;
    Tick f = in ? c.fadeIn : c.fadeOut;
    float shape = in ? c.fadeInShape : c.fadeOutShape;
    QPolygonF none;
    if (f <= 0) return none;   // no fade: the corner icon is the handle (see fadeIconRect)
    int xa, xb;
    if (in) {
        xa = r.left();
        xb = tickToX(c.start + f);
    } else {
        xb = r.right();
        xa = tickToX(c.start + len - f);
    }
    QPolygonF poly;
    if (xb <= xa || body.height() < 6) return poly;
    int n = std::max(4, (xb - xa) / 6);
    for (int i = 0; i <= n; ++i) {
        double t = (double)i / n;
        double x = xa + (xb - xa) * t;
        float g = dsp::fadeShape((float)(in ? t : 1.0 - t), shape);
        poly << QPointF(x, body.bottom() - g * (body.height() - 1));
    }
    return poly;
}
// Small handle icons shown while a control is at its default: fade icons in the top corners, gain icon at the top centre
QRect PlaylistView::fadeIconRect(const Clip& c, int which) const {
    QRect r = clipRect(c);
    int y = r.top() + 16;
    return which == 1 ? QRect(r.left() + 3, y, 13, 13) : QRect(r.right() - 16, y, 13, 13);
}
QRect PlaylistView::gainIconRect(const Clip& c) const {
    QRect r = clipRect(c);
    return QRect(r.center().x() - 22, r.bottom() - 15, 44, 13);
}   // bottom centre
static double distToPolyline(const QPolygonF& poly, const QPointF& pt) {
    double best = 1e9;
    for (int i = 1; i < poly.size(); ++i) {
        QPointF a = poly[i - 1], b = poly[i];
        QPointF ab = b - a;
        double l2 = QPointF::dotProduct(ab, ab);
        double t = l2 > 0 ? std::clamp(QPointF::dotProduct(pt - a, ab) / l2, 0.0, 1.0) : 0.0;
        QPointF pr = a + ab * t;
        best = std::min(best, std::hypot(pt.x() - pr.x(), pt.y() - pr.y()));
    }
    return best;
}
int PlaylistView::clipHandleAt(const Clip& c, const QPoint& pos, QPoint*) const {
    if (c.type != ClipType::Audio) return 0;
    QRect r = clipRect(c);
    if (r.width() < 30 || r.height() < 30) return 0;
    if (c.fadeIn > 0 ? distToPolyline(fadeCurve(c, 1), pos) <= 5 : fadeIconRect(c, 1).adjusted(-2, -2, 2, 2).contains(pos)) return 1;
    if (c.fadeOut > 0 ? distToPolyline(fadeCurve(c, 2), pos) <= 5 : fadeIconRect(c, 2).adjusted(-2, -2, 2, 2).contains(pos)) return 2;
    if (gainIconRect(c).adjusted(-2, -2, 2, 2).contains(pos)) return 3;
    return 0;
}
void PlaylistView::drawClip(QPainter& p, const Clip& c, const QRectF& rectF, double alpha) {
    QRect r = rectF.toRect();
    Layout L = layout();
    if (r.isNull() || r.right() < L.area.left() || r.left() > L.area.right() || r.bottom() < L.area.top() || r.top() > L.area.bottom())
        return;
    p.save();   // the caller clips to the clip area; save/restore keeps that intact instead of dropping it per clip
    if (alpha < 0.999) p.setOpacity(alpha);
    auto& proj = m_app.project();
    bool sel = m_selected.count(c.id);
    QColor col = clipColor(c);
    if (c.muted) col = col.darker(220);
    // body and header are one rounded shape: a square header strip inside rounded corners left notched corners
    double rad = theme::radiusFor(r.height());
    QPainterPath shape;
    shape.addRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), rad, rad);   // on pixel centres, so the border is crisp
    QPainterPath head;
    head.addRect(QRectF(r.left(), r.top(), r.width(), 13.5));
    p.setRenderHint(QPainter::Antialiasing);
    p.fillPath(shape, col.darker(sel ? 135 : 190));
    p.fillPath(shape.intersected(head), theme::withAlpha(col, sel ? 230 : 170));
    p.setRenderHint(QPainter::Antialiasing, false);
    // the content (notes, waveform, automation) stays inside the rounded shape; intersect, so a clip scrolled off the left
    // never paints over the track headers. The border, drawn last and antialiased, covers the clip's hard edge.
    p.setClipPath(shape, Qt::IntersectClip);
    p.setPen(theme::bg());
    p.setFont(theme::smallFont());
    p.drawText(QRect(r.left() + 4, r.top(), r.width() - 8, 13), Qt::AlignVCenter | Qt::AlignLeft, c.name + (c.muted ? " (muted)" : ""));
    QRect body = r.adjusted(1, 14, -1, -1);
    if (c.type == ClipType::Pattern) {
        if (auto* pat = proj.pattern(c.sourceId)) {
            Tick plen = std::max<Tick>(pat->effectiveLength(proj.transport.timeSig), 1);
            int lo = 127, hi = 0;
            for (auto& ch : pat->channels) {
                for (auto& n : ch.notes) {
                    lo = std::min(lo, n.pitch);
                    hi = std::max(hi, n.pitch);
                }
                for (size_t i = 0; i < ch.steps.size(); ++i)
                    if (ch.steps[i].enabled) {
                        auto* cc = proj.channel(ch.id);
                        int pp = cc ? cc->rootNote : 60;
                        lo = std::min(lo, pp);
                        hi = std::max(hi, pp);
                    }
            }
            if (lo > hi) {
                lo = 48;
                hi = 72;
            }
            int range = std::max(hi - lo + 1, 6);
            Tick clen = proj.clipLength(c);
            for (Tick k = 0; k * plen < clen + c.sourceOffset; ++k) {
                if (k > 0 && !c.loopSource) break;
                Tick off = c.start - c.sourceOffset + k * plen;
                if (off + plen <= c.start) continue;
                if (k > 0) {
                    int x = tickToX(off);
                    p.setPen(QPen(col.darker(90), 1, Qt::DashLine));
                    p.drawLine(x, body.top(), x, body.bottom());
                }
                for (auto& ch : pat->channels) {
                    auto* cc = proj.channel(ch.id);
                    QColor nc = cc ? cc->color.lighter(130) : theme::text();
                    p.setPen(nc);
                    for (auto& n : ch.notes) {
                        int x1 = tickToX(off + n.start), x2 = tickToX(off + n.start + n.duration);
                        int y = body.bottom() - (int)((n.pitch - lo + 0.5) / range * body.height());
                        p.drawLine(std::max(x1, r.left()), y, std::max(x2, x1 + 1), y);
                    }
                    Tick st = pat->stepTicks();
                    for (size_t i = 0; i < ch.steps.size() && (int)i < pat->stepCount; ++i)
                        if (ch.steps[i].enabled) {
                            int x1 = tickToX(off + i * st);
                            int y = body.bottom() - (int)(((cc ? cc->rootNote : 60) - lo + 0.5) / range * body.height());
                            p.drawLine(x1, y, x1 + std::max(1, (int)(st * m_ppt) - 1), y);
                        }
                }
            }
        }
    } else if (c.type == ClipType::Audio) {
        const AudioSource* src = proj.audioSource(c.sourceId);
        AudioAssetPtr a = src ? m_app.cache().get(src->filePath) : nullptr;
        if (a && a->frames > 0) {
            double sr = a->sampleRate;
            TempoMap tm = proj.transport.tempoMap();   // the waveform sits where the engine plays it (clipSourceSeconds)
            double framesPerTick = (clipSourceSeconds(c, c.start + PPQ, tm) - clipSourceSeconds(c, c.start, tm)) * sr / PPQ;
            double spp = framesPerTick / m_ppt;
            const WaveformLevel& lv = a->levelFor(spp);
            int mid = body.center().y();
            int hh = body.height() / 2 - 1;
            p.setPen(col.lighter(150));
            int64_t srcStart = src->trimStart;
            int64_t srcLen = (src->trimEnd > 0 ? src->trimEnd : a->frames) - srcStart;
            bool rev = c.reverse != src->reverse;
            float g = c.gain * src->gain * (src->normalize && a->peakValue > 0 ? 1.f / a->peakValue : 1.f);
            int x0 = std::max(r.left() + 1, L.area.left()), x1 = std::min(r.right() - 1, L.area.right());
            for (int x = x0; x <= x1; ++x) {
                double frame = clipSourceSeconds(c, (double)xToTick(x), tm) * sr;
                if (frame >= srcLen && !c.loopSource) break;
                if (c.loopSource && srcLen > 0) frame = std::fmod(frame, (double)srcLen);
                if (rev) frame = srcLen - 1 - frame;
                int64_t bin = (int64_t)((srcStart + frame) / lv.samplesPerBin);
                if (bin < 0 || bin >= (int64_t)lv.maxL.size()) continue;
                float mx = std::max(lv.maxL[bin], lv.maxR[bin]) * g, mn = std::min(lv.minL[bin], lv.minR[bin]) * g;
                p.drawLine(x, mid - (int)(clamp(mx, -1.f, 1.f) * hh), x, mid - (int)(clamp(mn, -1.f, 1.f) * hh));
            }
            // fades: the curve itself is the handle (grab it: sideways = length, up/down = slope); the attenuated part of the
            // sound is darkened so the fade reads at a glance. A zero-length fade still shows a short stub in the corner to grab.
            p.setRenderHint(QPainter::Antialiasing);
            auto drawFade = [&](int which) {
                bool in = which == 1;
                bool hot = m_hoverHandle == which && m_hoverHandleClip == c.id;
                Tick f = in ? c.fadeIn : c.fadeOut;
                if (f <= 0) {
                    if (r.width() < 40 || r.height() < 30) return;
                    QRect ir = fadeIconRect(c, which);
                    QColor ic = hot ? theme::accent() : QColor(255, 255, 255, 150);
                    p.setPen(QPen(ic, 1.5, Qt::SolidLine, Qt::FlatCap));
                    p.setBrush(Qt::NoBrush);
                    QRectF arc = in ? QRectF(ir.left() + 1.5, ir.top() + 1.5, 2.0 * (ir.width() - 3), 2.0 * (ir.height() - 3))
                                    : QRectF(ir.right() - 1.5 - 2.0 * (ir.width() - 3), ir.top() + 1.5, 2.0 * (ir.width() - 3),
                                          2.0 * (ir.height() - 3));
                    p.drawArc(arc, in ? 90 * 16 : 0, 90 * 16);
                    return;
                }   // rounded-corner arc icon: grab it to start a fade
                QPolygonF cv = fadeCurve(c, which);
                if (cv.size() < 2) return;
                if (f > 0) {
                    QPolygonF dark = cv;
                    if (in) {
                        dark << QPointF(cv.last().x(), body.top()) << QPointF(cv.first().x(), body.top());
                    } else {
                        dark << QPointF(cv.last().x(), body.top()) << QPointF(cv.first().x(), body.top());
                    }
                    p.setPen(Qt::NoPen);
                    p.setBrush(QColor(0, 0, 0, 120));
                    p.drawPolygon(dark);
                }
                p.setPen(QPen(hot ? theme::accent() : QColor(255, 255, 255, f > 0 ? 210 : 120), hot ? 2.0 : 1.4));
                p.setBrush(Qt::NoBrush);
                p.drawPolyline(cv);
            };
            drawFade(1);
            drawFade(2);
            if (r.width() >= 60 &&
                r.height() >= 30) {   // gain: a small tag at the bottom centre showing the value in dB; grab it and drag up/down
                bool hot = m_hoverHandle == 3 && m_hoverHandleClip == c.id;
                bool unity = std::fabs(c.gain - 1.f) <= 0.005f;
                QRect ir = gainIconRect(c);
                p.setFont(theme::smallFont());
                QString txt = unity
                    ? "0 dB"
                    : (c.gain <= 0.0005f ? "-inf"
                                         : QString("%1%2 dB").arg(c.gain > 1.f ? "+" : "").arg(20.0 * std::log10(c.gain), 0, 'f', 1));
                p.setPen(Qt::NoPen);
                p.setBrush(hot ? theme::withAlpha(theme::accent(), 200) : QColor(0, 0, 0, unity ? 60 : 120));
                p.drawRoundedRect(ir, theme::radiusFor(ir.height()), theme::radiusFor(ir.height()));
                p.setPen(hot ? theme::bg() : QColor(255, 255, 255, unity ? 130 : 230));
                p.drawText(ir, Qt::AlignCenter, txt);
            }
            p.setRenderHint(QPainter::Antialiasing, false);
        } else {
            p.setPen(theme::textDim());
            p.drawText(body, Qt::AlignCenter, src && src->missing ? "MISSING FILE" : "loading...");
        }
    } else if (c.type == ClipType::Automation) {
        if (auto* l = proj.lane(c.sourceId)) {
            p.setPen(QPen(col.lighter(160), 1.5));
            int x0 = std::max(r.left() + 1, L.area.left()), x1 = std::min(r.right() - 1, L.area.right());
            QPolygon poly;
            Tick len = std::max<Tick>(l->length, 1);
            for (int x = x0; x <= x1; x += 2) {
                Tick t = xToTick(x) - c.start + c.sourceOffset;
                if (c.loopSource) t %= len;
                float v = l->valueAt(t);
                poly << QPoint(x, r.bottom() - 4 - (int)(v * (r.height() - 8)));
            }
            p.drawPolyline(poly);
            p.setBrush(col.lighter(180));
            p.setPen(theme::bg());
            for (auto& pt : l->points) {
                int x = tickToX(c.start + pt.time - c.sourceOffset);
                if (x < r.left() || x > r.right()) continue;
                int y = r.bottom() - 4 - (int)(pt.value * (r.height() - 8));
                p.drawEllipse(QPoint(x, y), 3, 3);
            }
        }
    }
    p.restore();
    p.save();
    if (alpha < 0.999) p.setOpacity(alpha);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(sel ? QPen(Qt::white, 1.2) : QPen(col.darker(125), 1));
    p.setBrush(Qt::NoBrush);
    p.drawPath(shape);
    p.restore();
}
void PlaylistView::renderCache() {
    Layout L = layout();
    prepareCache();
    QPainter p(&m_cache);
    p.setRenderHint(
        QPainter::TextAntialiasing); /* the cache is rendered at the screen's real resolution: text stays crisp at 125 % / 150 % scaling */
    auto& proj = m_app.project();
    const auto& ts = proj.transport.timeSig;
    Tick beat = ts.ticksPerBeat(), bar = ts.ticksPerBar();
    // tracks background
    p.setClipRect(L.area);
    int y = L.rulerH - m_scrollY;
    for (size_t i = 0; i < proj.playlistTracks.size(); ++i) {
        auto& t = proj.playlistTracks[i];
        int th = trackH((int)i);
        QRect tr(L.area.left(), y, L.area.width(), th);
        p.fillRect(tr, i % 2 ? theme::altRow() : theme::panel());
        if (t.mute) p.fillRect(tr, QColor(0, 0, 0, 80));
        p.setPen(theme::grid());
        p.drawLine(tr.left(), tr.bottom(), tr.right(), tr.bottom());
        y += th;
    }
    // grid density adapts to the zoom: never draw lines closer than ~10 px; bars are stronger than beats, beats than snap subdivisions
    Tick step = snapTicks();
    if (step * m_ppt < 10) step = beat;
    if (beat * m_ppt < 10) step = bar;
    Tick barStep = bar;
    while (barStep * m_ppt < 14) barStep *= 2;
    if (step < barStep && bar * m_ppt < 14) step = barStep;
    Tick last = xToTick(L.area.right());
    int barEvery = (int)(barStep / bar);
    forEachGridLine(m_scrollTick, last, step, barEvery, [&](Tick t, int64_t b, int kind) {
        int x = tickToX(t);
        bool strongBar = kind == 2 && b % (barEvery * 4) == 0;
        p.setPen(strongBar && barEvery > 1 ? theme::gridStrong().lighter(140)
                : kind == 2                ? theme::gridStrong().lighter(120)
                : kind == 1                ? theme::gridStrong()
                                           : theme::grid());
        p.drawLine(x, L.area.top(), x, L.area.bottom());
    });
    // loop / punch region
    if (proj.transport.loopEnabled) {
        int x1 = tickToX(proj.transport.loopStart), x2 = tickToX(proj.transport.loopEnd);
        p.fillRect(QRect(x1, L.area.top(), x2 - x1, L.area.height()), QColor(0xf2, 0xa9, 0x3b, 18));
    }
    p.setClipping(false);
    paintRuler(p, L.ruler, barStep, last);
    if (proj.transport.loopEnabled) {   // the selected loop area; once deselected it leaves no trace (the range is still kept)
        int x1 = tickToX(proj.transport.loopStart), x2 = tickToX(proj.transport.loopEnd);
        p.fillRect(QRect(x1, L.ruler.top(), x2 - x1, 5), theme::accent());
    }
    if (proj.transport.punchEnabled) {
        int x1 = tickToX(proj.transport.punchIn), x2 = tickToX(proj.transport.punchOut);
        p.fillRect(QRect(x1, L.ruler.top() + 5, x2 - x1, 3), theme::red());
    }
    for (auto& m : proj.markers) {
        int x = tickToX(m.position);
        p.setPen(theme::green());
        p.setBrush(theme::green());
        p.drawPolygon(QPolygon({QPoint(x, L.ruler.bottom()), QPoint(x - 5, L.ruler.bottom() - 7), QPoint(x + 5, L.ruler.bottom() - 7)}));
        p.drawText(x + 7, L.ruler.bottom() - 2, m.name);
    }
    TempoMap tm = proj.transport.tempoMap();
    for (auto& c : proj.transport.timeSigChanges) {   // time-signature markers
        int x = tickToX(tm.barStart(c.bar));
        p.setPen(theme::accent());
        p.drawLine(x, L.ruler.top() + 8, x, L.ruler.bottom());
        p.drawText(x + 3, L.ruler.top() + 21, QString("%1/%2").arg(c.sig.numerator).arg(c.sig.denominator));
    }
    p.setClipping(false);
    paintTempoLane(p, L);
    // headers
    p.fillRect(L.headers, theme::panel());
    p.setClipRect(L.headers);
    y = L.rulerH - m_scrollY;
    for (size_t i = 0; i < proj.playlistTracks.size(); ++i) {
        auto& t = proj.playlistTracks[i];
        int th = trackH((int)i);
        QRect hr(0, y, L.headerW, th);
        p.fillRect(hr, i % 2 ? theme::panel() : theme::altRow());
        p.fillRect(QRect(0, y + 2, 3, th - 4), t.color);
        p.setPen(theme::border());
        p.drawLine(0, hr.bottom(), L.headerW, hr.bottom());
        p.setPen(theme::text());
        p.setFont(theme::uiFont());
        p.drawText(QRect(10, y + 3, L.headerW - 14, 16), Qt::AlignVCenter | Qt::AlignLeft,
            p.fontMetrics().elidedText(t.name, Qt::ElideRight, L.headerW - 16));
        auto btn = [&](QRect r, const QString& s, bool on, QColor c) {
            p.setPen(QPen(on ? c.darker(120) : theme::border(), 1));
            p.setBrush(on ? c : theme::surface());
            p.drawRoundedRect(r, theme::radius(), theme::radius());
            p.setPen(on ? theme::bg() : theme::textDim());
            p.setFont(theme::smallFont());
            p.drawText(r, Qt::AlignCenter, s);
        };
        if (th >= 40) {
            btn(QRect(10, y + 22, 18, 14), "M", t.mute, theme::red());
            btn(QRect(31, y + 22, 18, 14), "S", t.solo, theme::accent());
            btn(QRect(52, y + 22, 18, 14), "L", t.locked, theme::textDim());
            int mi = proj.mixerIndex(t.mixerTrackId);
            btn(QRect(73, y + 22, 40, 14), mi <= 0 ? "> Master" : QString("> Ins %1").arg(mi), false, theme::bg());
        }
        y += th;
    }
    p.setPen(theme::textDim());
    p.setFont(theme::smallFont());
    p.drawText(QRect(6, y + 4, L.headerW - 12, 14), Qt::AlignLeft, "+ double-click to add track");
    p.setClipping(false);
    p.fillRect(QRect(0, 0, L.headerW, L.rulerH), theme::panel());
    p.setPen(theme::textDim());
    p.setFont(theme::smallFont());
    p.drawText(tempoToggleRect(L).adjusted(10, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, m_tempoOpen ? "\u25BE  Tempo" : "\u25B8  Tempo");
    p.setPen(theme::border());
    p.drawLine(0, L.rulerH - 1, L.headerW, L.rulerH - 1);
}
void PlaylistView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    drawCache(p);
    Layout L = layout();
    {   // clips: animated (appear / move / vanish), instant while being dragged
        p.save();
        p.setClipRect(L.area);
        auto& proj = m_app.project();
        bool dragging = m_drag == Moving || m_drag == ResizingL || m_drag == ResizingR || m_drag == FadeIn || m_drag == FadeOut ||
            m_drag == Gain;
        m_clipAnim.begin();
        // tracked in song space (tick / track index) so scrolling, zooming or resizing the view never "moves" clips
        auto trackIndex = [&](Id id) {
            auto& tr = proj.playlistTracks;
            for (size_t i = 0; i < tr.size(); ++i)
                if (tr[i].id == id) return (int)i;
            return -1;
        };
        auto toPx = [&](const QRectF& m) {
            int i = std::max(0, (int)std::floor(m.y()));
            double th = trackH(i);
            return QRectF(
                tickToX(0) + m.x() * m_ppt, trackTop(i) + (m.y() - i) * th + 1, std::max(3.0, m.width() * m_ppt), m.height() * th - 2);
        };
        for (auto& c : proj.clips) {
            int ti = trackIndex(c.trackId);
            if (ti < 0) continue;
            m_clipColor[c.id] = clipColor(c);
            QRectF target(c.start, ti, proj.clipLength(c), 1);
            double alpha = 1;
            QRectF r = toPx(m_clipAnim.item(c.id, target, dragging && m_selected.count(c.id), &alpha));
            drawClip(p, c, r, alpha);
        }
        // The clip is already gone from the project by now, so the ghost uses the colour remembered while it was drawn.
        m_clipAnim.end([&](Id id) {
            auto it = m_clipColor.find(id);
            return it != m_clipColor.end() ? it->second : theme::accent2();
        });
        // Deliberately not cleared per frame: the ghost of a just-deleted clip still needs its colour, and the clip is
        // already out of the project by then. Pruned only once the map has grown well past any plausible clip count.
        if (m_clipColor.size() > 4096 && !m_clipAnim.active()) {
            m_clipColor.clear();
            for (auto& c : proj.clips) m_clipColor[c.id] = clipColor(c);
        }
        m_clipAnim.drawGhosts([&](const QRectF& m, const QColor& c, double a) {
            QRectF r = toPx(m);
            QColor cc = c;
            cc.setAlphaF(a * 0.6);
            p.setPen(Qt::NoPen);
            p.setBrush(cc);
            { double rr = theme::radiusFor(r.height()); p.drawRoundedRect(r, rr, rr); }
        });
        p.restore();
        m_ticker->keepAlive(m_clipAnim.active());
    }
    paintPlayhead(p, L.ruler.top(), L.area.bottom());
    if (m_drag == Rubber) paintRubber(p);
    if (m_tool == Draw && m_drag == NoDrag && L.area.contains(m_hoverPos) && underMouse() && !clipAt(m_hoverPos)) {
        int tr = trackAtY(m_hoverPos.y());
        if (tr >= 0) {
            const Clip* b = brush();
            const Pattern* pat = m_app.project().pattern(m_app.project().selectedPatternId);
            if (b || pat) {
                QColor pc = b && b->type != ClipType::Pattern ? theme::accent() : pat ? pat->color : theme::accent();
                Tick t = snap(xToTick(m_hoverPos.x()));
                QRect r(tickToX(t), trackTop(tr) + 1, std::max(3, (int)(brushLength() * m_ppt)),
                    trackH(tr) - 2);
                p.save();
                p.setClipRect(L.area);
                p.setRenderHint(QPainter::Antialiasing);
                p.setPen(QPen(theme::withAlpha(pc, 200), 1));
                p.setBrush(theme::withAlpha(pc, 40));
                double rad = theme::radiusFor(r.height());
                p.drawRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), rad, rad);
                p.restore();
            }
        }
    }
    p.setPen(theme::border());
    p.setBrush(Qt::NoBrush);
    p.drawRect(rect().adjusted(0, 0, -1, -1));
}

// ---------------------------------------------------------------- edit transactions
void PlaylistView::beginEdit() {
    m_before.clear();
    for (auto& c : m_app.project().clips)
        if (m_selected.count(c.id)) m_before[c.id] = c;
    m_editing = true;
}
void PlaylistView::commitEdit(const QString& desc) {
    if (!m_editing) return;
    m_editing = false;
    auto& proj = m_app.project();
    std::vector<std::unique_ptr<Command>> cmds;
    std::vector<Clip> inserted;
    for (auto& c : proj.clips) {
        auto it = m_before.find(c.id);
        if (it == m_before.end()) {
            if (m_selected.count(c.id) && m_duplicated) inserted.push_back(c);
            continue;
        }
        Clip after = c;
        if (after == it->second) continue;   // every field: gain and fade-curve drags are edits too
        c = it->second;
        cmds.push_back(std::make_unique<EditObjectCommand<Clip>>(m_app.model(), c.id, "", [after](Clip& x) { x = after; }, 0));
    }
    if (m_duplicated) {
        for (auto& ins : inserted) {
            std::erase_if(proj.clips, [&](const Clip& x) { return x.id == ins.id; });
            cmds.push_back(std::make_unique<InsertObjectCommand<Clip>>(m_app.model(), ins, "", -1));
        }
    }
    m_duplicated = false;
    m_before.clear();
    if (cmds.empty()) {
        invalidate();
        return;
    }
    m_app.model().macro(desc, std::move(cmds));
}

// ---------------------------------------------------------------- mouse
void PlaylistView::mousePressEvent(QMouseEvent* e) {
    setFocus();
    Layout L = layout();
    m_pressPos = m_lastPos = e->pos();
    m_altHeld = e->modifiers() & Qt::AltModifier;
    auto& proj = m_app.project();
    if (e->button() == Qt::MiddleButton) {
        m_drag = Pan;
        return;
    }
    if (e->button() == Qt::LeftButton && tempoToggleRect(L).contains(e->pos())) {
        m_tempoOpen = !m_tempoOpen;
        invalidate();
        return;
    }
    if (L.tempo.contains(e->pos())) {
        int idx = tempoPointAt(e->pos());
        if (e->button() == Qt::RightButton) showTempoMenu(idx, e->globalPosition().toPoint());
        else if (e->button() == Qt::LeftButton && idx >= 0) {
            m_tempoRange = tempoRange();
            m_tempoBefore = proj.transport.tempoPoints;
            m_tempoPoint = idx;
            m_drag = TempoPointDrag;
        }
        return;
    }
    if (L.ruler.contains(e->pos())) {
        Tick t = std::max<Tick>(0, snapRound(xToTick(e->pos().x())));
        if (e->button() == Qt::RightButton) {
            QMenu m;
            addTimeSigActions(m, e->pos().x());
            m.addAction("Add marker here", [this, t] {
                if (auto n = askName(this, "Marker", QString("Marker %1").arg(m_app.project().markers.size() + 1), true)) m_app.addMarker(t, *n);
            });
            for (auto& mk : proj.markers)
                if (std::abs(tickToX(mk.position) - e->pos().x()) < 8) {
                    m.addAction("Rename marker '" + mk.name + "'", [this, id = mk.id, n = mk.name] {
                        if (auto nn = askName(this, "Marker", n, true))
                            m_app.model().edit<Marker>(id, "Rename marker", [nn = *nn](Marker& x) { x.name = nn; });
                    });
                    m.addAction(
                        "Delete marker '" + mk.name + "'", [this, id = mk.id] { m_app.model().remove<Marker>(id, "Delete marker"); });
                }
            m.addAction("Set punch in/out to loop", [this] {
                auto& t = m_app.project().transport;
                m_app.setPunch(true, t.loopStart, t.loopEnd);
            });
            m.exec(e->globalPosition().toPoint());
            return;
        }
        if (e->modifiers() & Qt::ControlModifier) {
            m_drag = LoopRange;
            m_loopDragStart = t;
        } else m_drag = Seek;
        m_pressTick = t;
        return;
    }
    if (L.headers.contains(e->pos())) {
        int idx = trackAtY(e->pos().y());
        if (idx < 0) return;
        auto& t = proj.playlistTracks[idx];
        int y = trackTop(idx);
        int ly = e->pos().y() - y;
        if (ly >= trackH(idx) - 5) {
            m_drag = TrackHeight;
            m_heightTrack = idx;
            m_heightStart = t.height;
            return;
        }
        if (e->button() == Qt::RightButton) {
            showTrackMenu(idx, e->globalPosition().toPoint());
            return;
        }
        if (trackH(idx) >= 40 && ly >= 22 && ly <= 36) {
            int x = e->pos().x();
            if (x >= 10 && x < 28)
                m_app.edit<PlaylistTrack>(t.id, t.mute ? "Unmute track" : "Mute track", [](PlaylistTrack& x) { x.mute = !x.mute; });
            else if (x >= 31 && x < 49) m_app.edit<PlaylistTrack>(t.id, "Solo track", [](PlaylistTrack& x) { x.solo = !x.solo; });
            else if (x >= 52 && x < 70) m_app.edit<PlaylistTrack>(t.id, "Lock track", [](PlaylistTrack& x) { x.locked = !x.locked; });
            else if (x >= 73 && x < 113) {
                QMenu m;
                fillMixerTrackMenu(&m, proj, t.mixerTrackId, [this, tid = t.id](Id mid) {
                    m_app.edit<PlaylistTrack>(tid, "Track mixer routing", [mid](PlaylistTrack& x) { x.mixerTrackId = mid; });
                });
                m.exec(e->globalPosition().toPoint());
            }
        }
        return;
    }
    if (!L.area.contains(e->pos())) return;
    int edge = 0;
    Clip* c = clipAt(e->pos(), &edge);
    int trackIdx = trackAtY(e->pos().y());
    if (c && proj.track(c->trackId) && proj.track(c->trackId)->locked && m_tool != MuteTool) return;
    if (e->button() == Qt::RightButton) {
        m_erasedAny = false;
        setCursor(theme::toolCursor(theme::Glyph::Eraser));
        Q_EMIT modeChanged(Erase);
        if (c) {
            if (m_tool == Draw || m_tool == Erase) {
                m_selected.clear();
                m_app.deleteClips({c->id});
                m_erasedAny = true;
            } else {
                showClipMenu(c, e->globalPosition().toPoint());
                return;
            }
        }
        m_drag = Erasing;
        return;
    }
    if (m_tool == Erase) {
        if (c) {
            m_drag = Erasing;
            m_app.deleteClips({c->id});
        }
        return;
    }
    if (m_tool == Slice) {
        if (c) m_app.splitClip(c->id, snap(xToTick(e->pos().x())));
        return;
    }
    if (m_tool == MuteTool) {
        if (c && c->type == ClipType::Audio && e->modifiers() & Qt::ShiftModifier) {
            useTake(c->id);
            return;
        }
        m_drag = Muting;
        m_touched.clear();
        if (c) {
            m_touched.insert(c->id);
            m_app.edit<Clip>(c->id, c->muted ? "Unmute clip" : "Mute clip", [](Clip& x) { x.muted = !x.muted; });
        }
        return;
    }
    if (c) {
        // automation point editing
        if (c->type == ClipType::Automation && !(e->modifiers() & Qt::ControlModifier)) {
            int pi = autoPointAt(*c, e->pos());
            if (pi >= 0) {
                m_autoLane = c->sourceId;
                m_autoPoint = pi;
                m_dragClip = c->id;
                m_laneBefore = *proj.lane(m_autoLane);
                m_drag = AutoPoint;
                return;
            }
        }
        if (int h = clipHandleAt(*c, e->pos())) {
            m_selected = {c->id};
            beginEdit();
            m_dragClip = c->id;
            m_pressPos = e->pos();
            m_handleStartFade = h == 1 ? c->fadeIn : c->fadeOut;
            m_handleStartShape = h == 1 ? c->fadeInShape : c->fadeOutShape;
            m_handleStartGain = c->gain;
            m_drag = h == 1 ? FadeIn : h == 2 ? FadeOut : Gain;
            return;
        }
        if (e->modifiers() & Qt::ControlModifier && !(e->modifiers() & Qt::ShiftModifier)) {
            m_drag = Rubber;
            beginRubber(e->pos(), c->id);
            return;
        }   // Ctrl: drag = rubber-band selection from here, click = toggle this clip
        if (!m_selected.count(c->id)) {
            if (!(e->modifiers() & Qt::ShiftModifier)) m_selected.clear();
            m_selected.insert(c->id);
        }
        if (e->button() == Qt::LeftButton) {
            setBrush(*c);   // FL: the clicked clip is what the draw tool places next
            if (c->type == ClipType::Pattern) m_app.selectPattern(c->sourceId);
            else if (auto* a = c->type == ClipType::Audio ? proj.audioSource(c->sourceId) : nullptr) Q_EMIT m_app.revealInProject("audio|" + a->filePath);
            else if (c->type == ClipType::Automation) Q_EMIT m_app.revealInProject("lane|" + QString::number(c->sourceId));
        }
        beginEdit();
        m_dragClip = c->id;
        m_pressTick = xToTick(e->pos().x());
        m_pressTrack = trackIdx;
        m_duplicated = false;
        if (e->modifiers() & Qt::ShiftModifier && e->modifiers() & Qt::ControlModifier) {
            std::vector<Clip> copies;
            for (auto& x : proj.clips)
                if (m_selected.count(x.id)) {
                    Clip d = x;
                    d.id = proj.newId();
                    copies.push_back(d);
                }
            m_selected.clear();
            for (auto& d : copies) {
                proj.clips.push_back(d);
                m_selected.insert(d.id);
            }
            m_duplicated = true;
            m_dragClip = copies.empty() ? 0 : copies.front().id;
        }
        m_drag = edge == 1 ? ResizingR : edge == -1 ? ResizingL : Moving;
        invalidate();
        return;
    }
    if (m_tool == Select || (e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))) {
        if (!(e->modifiers() & Qt::ShiftModifier)) m_selected.clear();
        m_drag = Rubber;
        beginRubber(e->pos());
        return;
    }
    // draw tool on empty: place the brush (last clicked clip), or the selected pattern
    if (trackIdx >= 0) {
        auto& t = proj.playlistTracks[trackIdx];
        if (t.locked) return;
        Tick at = std::max<Tick>(0, snap(xToTick(e->pos().x())));
        const Clip* b = brush();
        Id id = 0;
        if (b && b->type != ClipType::Pattern) {
            Clip n = *b;   // copy first: inserting may move the clip list
            n.id = proj.newId();
            n.trackId = t.id;
            n.start = at;
            m_app.model().insert<Clip>(n, "Add clip");
            id = n.id;
        } else id = m_app.addPatternClip(t.id, proj.selectedPatternId, at, b ? b->length : 0, b && b->loopSource);
        if (id) {
            m_selected = {id};
            beginEdit();
            m_dragClip = id;
            m_pressTick = xToTick(e->pos().x());
            m_pressTrack = trackIdx;
            m_drag = Moving;
            m_placed = true;
        }
    } else m_selected.clear();
    invalidate();
}
void PlaylistView::revealClips(const std::vector<Id>& ids) {
    auto& proj = m_app.project();
    m_selected = std::set<Id>(ids.begin(), ids.end());
    const Clip* first = nullptr;
    for (Id id : ids)
        if (const Clip* c = proj.clip(id); c && (!first || c->start < first->start)) first = c;
    if (first) {
        int ti = 0;
        while (ti < (int)proj.playlistTracks.size() && proj.playlistTracks[ti].id != first->trackId) ++ti;
        Layout L = layout();
        Tick view = (Tick)((L.area.width()) / m_ppt);
        if (first->start < m_scrollTick || first->start > m_scrollTick + view * 9 / 10)
            m_scrollTick = std::max<Tick>(0, first->start - proj.transport.timeSig.ticksPerBar());   // a bar of context before it
        if (ti < (int)proj.playlistTracks.size()) {
            int top = trackTop(ti) + m_scrollY - L.rulerH;   // unscrolled offset of the track
            if (trackTop(ti) < L.rulerH || trackTop(ti) + trackH(ti) > height()) m_scrollY = std::max(0, top - trackH(ti));
        }
        updateScrollbars();
    }
    setFocus();
    invalidate();
}
void PlaylistView::setBrush(const Clip& c) {
    m_brush = c;
    m_brushClip = c.id;
    m_hasBrush = true;
    m_brushPatSel = c.type == ClipType::Pattern ? c.sourceId : m_app.project().selectedPatternId;
}
const Clip* PlaylistView::brush() const {
    auto& proj = m_app.project();
    if (!m_hasBrush || proj.selectedPatternId != m_brushPatSel) return nullptr;
    if (const Clip* live = proj.clip(m_brushClip)) return live;   // current size / settings (tempo changes, edits since)
    bool sourceOk = m_brush.type == ClipType::Pattern ? proj.pattern(m_brush.sourceId) != nullptr
        : m_brush.type == ClipType::Audio             ? proj.audioSource(m_brush.sourceId) != nullptr
                                                      : proj.lane(m_brush.sourceId) != nullptr;
    return sourceOk ? &m_brush : nullptr;
}
Tick PlaylistView::brushLength() const {
    auto& proj = m_app.project();
    if (const Clip* b = brush()) return proj.clipLength(*b);
    const Pattern* p = proj.pattern(proj.selectedPatternId);
    return p ? p->effectiveLength(proj.transport.timeSig) : 0;
}
void PlaylistView::mouseMoveEvent(QMouseEvent* e) {
    m_hoverPos = e->pos();
    m_altHeld = e->modifiers() & Qt::AltModifier;
    Layout L = layout();
    auto& proj = m_app.project();
    if (m_drag == NoDrag) {
        int edge = 0;
        Clip* c = L.area.contains(e->pos()) ? clipAt(e->pos(), &edge) : nullptr;
        if (m_tool == Draw || m_tool == Select) {
            if (L.headers.contains(e->pos())) {
                int idx = trackAtY(e->pos().y());
                setCursor(idx >= 0 && e->pos().y() - trackTop(idx) >= trackH(idx) - 5 ? Qt::SizeVerCursor : Qt::ArrowCursor);
            } else {
                int hh = c ? clipHandleAt(*c, e->pos()) : 0;
                if (hh != m_hoverHandle || (c ? c->id : 0) != m_hoverHandleClip) {
                    m_hoverHandle = hh;
                    m_hoverHandleClip = c ? c->id : 0;
                    invalidate();
                }
                setCursor(c ? (hh ? QCursor(Qt::ArrowCursor)
                                      : edge != 0 ? QCursor(Qt::SizeHorCursor)
                                                  : QCursor(Qt::SizeAllCursor))
                            : toolCursorFor(m_tool));
            }
        }
        if (m_tool == Draw) update();
        return;
    }
    if (m_drag == Pan) {
        int dx = e->pos().x() - m_lastPos.x(), dy = e->pos().y() - m_lastPos.y();
        m_scrollTick = std::max<Tick>(0, m_scrollTick - (Tick)(dx / m_ppt));
        m_scrollY = std::max(0, m_scrollY - dy);
        m_lastPos = e->pos();
        updateScrollbars();
        invalidate();
        return;
    }
    if (m_drag == TempoPointDrag) {   // sideways moves the point (kept between its neighbours), up / down changes its BPM
        auto pts = m_tempoBefore;
        TempoPoint& pt = pts[m_tempoPoint];
        if (std::abs(e->pos().x() - m_pressPos.x()) >= 3) {
            Tick lo = m_tempoPoint > 0 ? pts[m_tempoPoint - 1].tick + 1 : 0;
            Tick hi = m_tempoPoint + 1 < (int)pts.size() ? pts[m_tempoPoint + 1].tick - 1 : std::numeric_limits<Tick>::max();
            pt.tick = clamp(snapNear(xToTick(e->pos().x())), lo, hi);
        }
        pt.bpm = clamp(std::round((pt.bpm + (m_pressPos.y() - e->pos().y()) * 0.5) * 10) / 10, 20.0, 999.0);
        proj.transport.tempoPoints = pts;
        invalidate();
        return;
    }
    if (m_drag == Seek) {
        Tick t = std::max<Tick>(0, snapRound(xToTick(e->pos().x())));
        if (t != m_pressTick) {
            m_pressTick = t;
            m_app.seek(t);
        }
        return;
    }
    if (m_drag == LoopRange) {
        Tick t = std::max<Tick>(0, snapRound(xToTick(e->pos().x())));
        if (std::abs(e->pos().x() - m_pressPos.x()) > 3) {
            Tick a = std::min(m_loopDragStart, t), b = std::max(m_loopDragStart, t);
            if (b > a) {
                proj.transport.loopStart = a;
                proj.transport.loopEnd = b;
                proj.transport.loopEnabled = true;
                m_app.engine().setLoop(true, a, b);
                invalidate();
            }
        }
        return;
    }
    if (m_drag == TrackHeight) {
        int h = clamp(m_heightStart + (int)((e->pos().y() - m_pressPos.y()) / m_vzoom), 24, 300);
        proj.playlistTracks[m_heightTrack].height = h;
        updateScrollbars();
        invalidate();
        return;
    }
    if (m_drag == Rubber) {
        dragRubber(e->pos(), e->modifiers());
        return;
    }
    if (m_drag == Erasing) {
        if (Clip* c = clipAt(e->pos())) {
            m_app.deleteClips({c->id});
            m_erasedAny = true;
        }
        return;
    }
    if (m_drag == Muting) {
        if (Clip* c = clipAt(e->pos())) {
            if (!m_touched.count(c->id)) {
                m_touched.insert(c->id);
                m_app.edit<Clip>(c->id, c->muted ? "Unmute clip" : "Mute clip", [](Clip& x) { x.muted = !x.muted; });
            }
        }
        return;
    }
    if (m_drag == AutoPoint) {
        auto* l = proj.lane(m_autoLane);
        const Clip* c = proj.clip(m_dragClip);
        if (!l || !c || m_autoPoint < 0 || m_autoPoint >= (int)l->points.size()) return;
        auto& pt = l->points[m_autoPoint];
        pt.value = autoValueAtY(*c, e->pos().y());
        Tick lo = m_autoPoint > 0 ? l->points[m_autoPoint - 1].time : 0,
             hi = m_autoPoint + 1 < (int)l->points.size() ? l->points[m_autoPoint + 1].time : LLONG_MAX;
        if (m_autoPoint > 0 && m_autoPoint + 1 < (int)l->points.size()) pt.time = clamp(snap(autoTickAtX(*c, e->pos().x())), lo, hi);
        invalidate();
        return;
    }
    if (m_drag == FadeIn || m_drag == FadeOut) {
        Clip* c = proj.clip(m_dragClip);
        if (!c) return;
        Tick len = proj.clipLength(*c);
        Tick t = xToTick(e->pos().x());
        float shape = clamp(m_handleStartShape + (m_pressPos.y() - e->pos().y()) / 50.f, -1.f, 1.f);   // sideways = length, up/down = slope
        if (m_drag == FadeIn) {
            c->fadeIn = clamp<Tick>(t - c->start, 0, len);
            c->fadeInShape = shape;
        } else {
            c->fadeOut = clamp<Tick>(c->start + len - t, 0, len);
            c->fadeOutShape = shape;
        }
        invalidate();
        return;
    }
    if (m_drag == Gain) {
        Clip* c = proj.clip(m_dragClip);
        if (!c) return;
        c->gain = clamp(m_handleStartGain + (m_pressPos.y() - e->pos().y()) / 60.f, 0.f, 2.f);
        invalidate();
        return;
    }
    if (m_drag == Moving) {
        Tick dt = xToTick(e->pos().x()) - m_pressTick;
        int dtrack = trackAtY(e->pos().y()) >= 0 ? trackAtY(e->pos().y()) - m_pressTrack : 0;
        auto ab = m_before.find(m_dragClip);
        Tick anchorStart = ab != m_before.end() ? ab->second.start : (proj.clip(m_dragClip) ? proj.clip(m_dragClip)->start : 0);
        Tick delta = snap(anchorStart + dt) - anchorStart;
        Tick minStart = LLONG_MAX;
        int minTrack = INT_MAX, maxTrack = 0;
        std::map<Id, int> trackIndex;
        for (size_t i = 0; i < proj.playlistTracks.size(); ++i) trackIndex[proj.playlistTracks[i].id] = (int)i;
        auto origOf = [&](const Clip& c) {
            auto it = m_before.find(c.id);
            return it != m_before.end() ? it->second : c;
        };
        for (auto& c : proj.clips)
            if (m_selected.count(c.id)) {
                Clip o = origOf(c);
                minStart = std::min(minStart, o.start);
                int ti = trackIndex[o.trackId];
                minTrack = std::min(minTrack, ti);
                maxTrack = std::max(maxTrack, ti);
            }
        if (minStart != LLONG_MAX && minStart + delta < 0) delta = -minStart;
        dtrack = clamp(dtrack, -minTrack, (int)proj.playlistTracks.size() - 1 - maxTrack);
        for (auto& c : proj.clips)
            if (m_selected.count(c.id)) {
                Clip o = origOf(c);
                if (m_before.find(c.id) == m_before.end() &&
                    m_duplicated) { /* duplicates: keep original position + delta relative to press */
                }
                c.start = o.start + delta;
                int ti = trackIndex[o.trackId] + dtrack;
                if (ti >= 0 && ti < (int)proj.playlistTracks.size()) c.trackId = proj.playlistTracks[ti].id;
            }
        invalidate();
        return;
    }
    if (m_drag == ResizingR || m_drag == ResizingL) {
        auto ab = m_before.find(m_dragClip);
        if (ab == m_before.end()) return;
        const Clip& o = ab->second;
        Tick olen = proj.clipLength(o);
        Tick t = snapNear(xToTick(e->pos().x()));
        for (auto& c : proj.clips)
            if (m_selected.count(c.id)) {
                auto it = m_before.find(c.id);
                if (it == m_before.end()) continue;
                const Clip& b = it->second;
                Tick blen = proj.clipLength(b);
                if (m_drag == ResizingR) {
                    Tick newLen = std::max<Tick>(snapTicks() > 1 ? snapTicks() : PPQ / 16, t - o.start + (blen - olen));
                    c.length = newLen;
                } else {
                    Tick newStart = clamp<Tick>(t, 0, b.start + blen - PPQ / 16);
                    Tick d = newStart - b.start;
                    c.start = newStart;
                    c.length = blen - d;
                    c.sourceOffset = std::max<Tick>(0, b.sourceOffset + d);
                }
            }
        invalidate();
        return;
    }
}
void PlaylistView::mouseReleaseEvent(QMouseEvent* e) {
    auto& proj = m_app.project();
    switch (m_drag) {
    case Moving:
        commitEdit(m_duplicated ? "Duplicate clips" : "Move clips");
        if (m_placed) {
            m_selected.clear();
            m_placed = false;
        }
        break;
    case ResizingR:
    case ResizingL:
        commitEdit("Resize clips");
        if (const Clip* c = proj.clip(m_dragClip)) setBrush(*c);   // like a resized note sets the draw length
        break;
    case FadeIn:
    case FadeOut: commitEdit("Edit fade"); break;
    case Gain: commitEdit("Clip gain"); break;
    case TempoPointDrag: {
        auto after = proj.transport.tempoPoints;
        const TempoPoint &a = after[m_tempoPoint], &b = m_tempoBefore[m_tempoPoint];
        bool moved = a.tick != b.tick || a.bpm != b.bpm;
        proj.transport.tempoPoints = m_tempoBefore;
        if (moved) m_app.editTempo("Move tempo point", [after](TransportSettings& t) { t.tempoPoints = after; });
        break;
    }
    case LoopRange: {
        if (std::abs(e->pos().x() - m_pressPos.x()) > 3) {
            Tick a = proj.transport.loopStart, b = proj.transport.loopEnd;
            m_app.setLoopRange(a, b);
            if (!proj.transport.loopEnabled) m_app.setLoop(true);
        } else if (proj.transport.loopEnabled) m_app.setLoop(false);
        else m_app.seek(m_pressTick);
        break;
    }
    case Seek: m_app.seek(m_pressTick); break;
    case TrackHeight: {
        int h = proj.playlistTracks[m_heightTrack].height;
        proj.playlistTracks[m_heightTrack].height = m_heightStart;
        m_app.edit<PlaylistTrack>(proj.playlistTracks[m_heightTrack].id, "Track height", [h](PlaylistTrack& t) { t.height = h; });
        break;
    }
    case AutoPoint: {
        auto* l = proj.lane(m_autoLane);
        if (l) {
            AutomationLane after = *l;
            *l = m_laneBefore;
            m_app.edit<AutomationLane>(m_autoLane, "Move automation point", [after](AutomationLane& x) { x = after; });
        }
        break;
    }
    case Rubber: finishRubber(e->modifiers()); break;
    case Erasing:
        setTool(m_tool);
        m_lastEraseWasClick = m_erasedAny && (e->pos() - m_pressPos).manhattanLength() < 4;
        m_lastEraseTime = QDateTime::currentMSecsSinceEpoch();
        break;   // a plain right-click is just an (empty) delete stroke: no menu (clip settings live in double-click / the project panel)
    case Muting: setTool(m_tool); break;
    default: break;
    }
    m_drag = NoDrag;
    m_dragClip = 0;
    m_autoPoint = -1;
    invalidate();
}
void PlaylistView::mouseDoubleClickEvent(QMouseEvent* e) {
    Layout L = layout();
    auto& proj = m_app.project();
    if (e->button() == Qt::RightButton && L.area.contains(e->pos())) {   // double right-click: mute mode while dragging
        if (m_lastEraseWasClick && QDateTime::currentMSecsSinceEpoch() - m_lastEraseTime < 600) {
            m_app.model().undoStack().undo();
            m_lastEraseWasClick = false;
        }
        m_drag = Muting;
        m_touched.clear();
        setCursor(toolCursorFor(MuteTool));
        Q_EMIT modeChanged(MuteTool);
        if (Clip* c = clipAt(e->pos())) {
            m_touched.insert(c->id);
            m_app.edit<Clip>(c->id, c->muted ? "Unmute clip" : "Mute clip", [](Clip& x) { x.muted = !x.muted; });
        }
        return;
    }
    if (L.headers.contains(e->pos())) {
        int idx = trackAtY(e->pos().y());
        if (idx < 0) {
            m_app.addPlaylistTrack();
            return;
        }
        auto& t = proj.playlistTracks[idx];
        if (auto n = askName(this, "Rename track", t.name)) m_app.edit<PlaylistTrack>(t.id, "Rename track", [n = *n](PlaylistTrack& x) { x.name = n; });
        return;
    }
    if (L.tempo.contains(e->pos())) {   // a new tempo point at the BPM under the pointer (or the point already at that tick)
        if (e->button() != Qt::LeftButton || tempoPointAt(e->pos()) >= 0) return;
        Tick t = std::max<Tick>(0, snapNear(xToTick(e->pos().x())));
        auto [lo, hi] = tempoRange();
        double bpm = clamp(std::round(lo + (L.tempo.bottom() - 3 - e->pos().y()) * (hi - lo) / (L.tempo.height() - 7)), 20.0, 999.0);
        m_app.editTempo("Add tempo point", [t, bpm](TransportSettings& tr) {
            auto it = std::lower_bound(
                tr.tempoPoints.begin(), tr.tempoPoints.end(), t, [](const TempoPoint& p, Tick v) { return p.tick < v; });
            if (it != tr.tempoPoints.end() && it->tick == t) it->bpm = bpm;
            else tr.tempoPoints.insert(it, {t, bpm, false});
        });
        return;
    }
    if (L.ruler.contains(e->pos())) {
        if (m_app.project().transport.loopEnabled) m_app.setLoop(false);
        else zoomToFit();
        return;
    }   // double-click on the loop bar deselects it
    if (Clip* c = clipAt(e->pos())) {
        if (c->type == ClipType::Automation) {
            auto* l = proj.lane(c->sourceId);
            if (!l) return;
            AutomationPoint pt;
            pt.time = snap(autoTickAtX(*c, e->pos().x()));
            pt.value = autoValueAtY(*c, e->pos().y());
            m_app.edit<AutomationLane>(l->id, "Add automation point", [pt](AutomationLane& x) {
                x.points.push_back(pt);
                std::sort(
                    x.points.begin(), x.points.end(), [](const AutomationPoint& a, const AutomationPoint& b) { return a.time < b.time; });
                if (x.length < pt.time) x.length = pt.time;
            });
            return;
        }
        if (c->type == ClipType::Pattern) {
            m_app.selectPattern(c->sourceId);
            Q_EMIT openPattern(c->sourceId);
            return;
        }
        if (c->type == ClipType::Audio) {
            Q_EMIT openAudioClip(c->id);
            return;
        }
    }
}
void PlaylistView::wheelEvent(QWheelEvent* e) {
    int dy = e->angleDelta().y();
    Layout L = layout();
    if (e->modifiers() & Qt::AltModifier) {
        int d = e->angleDelta().y() != 0 ? e->angleDelta().y() : e->angleDelta().x();
        double old = m_vzoom;
        m_vzoom = clamp(m_vzoom * (d > 0 ? 1.15 : 1 / 1.15), 0.4, 3.0);
        int my = e->position().toPoint().y() - L.rulerH;
        m_scrollY = std::max(0, (int)((m_scrollY + my) * m_vzoom / old) - my);
        updateScrollbars();
        invalidate();
        e->accept();
        return;
    }
    if (handleTimeWheel(e)) return;
    m_scrollY = clamp(m_scrollY - dy / 2, 0, m_vbar->maximum());
    updateScrollbars();
    invalidate();
    e->accept();
}
// FL Studio default keybinds (typing keyboard mode intercepts unmodified letters before this, see MainWindow)
void PlaylistView::keyPressEvent(QKeyEvent* e) {
    bool ctrl = e->modifiers() & Qt::ControlModifier, shift = e->modifiers() & Qt::ShiftModifier, alt = e->modifiers() & Qt::AltModifier;
    auto& proj = m_app.project();
    switch (e->key()) {   // playlist keys; the shortcuts shared with the piano roll are in TimelineView::handleSharedKey
    case Qt::Key_Insert:
        if (ctrl) break;
        sliceAtPlayhead();
        return;
    case Qt::Key_C:
        if (ctrl) break;
        if (shift) {
            selectBySource();
            return;
        }
        if (!alt) {
            pickTool(Slice);
            return;
        }
        break;
    case Qt::Key_D:
        if (ctrl || alt) break;
        pickTool(Erase);
        return;
    case Qt::Key_B:
        if (ctrl || alt) break;
        pickTool(Draw);
        return;
    case Qt::Key_M:
        if (!alt && !ctrl && !m_selected.empty()) {
            bool any = false;
            for (auto& c : proj.clips)
                if (m_selected.count(c.id) && !c.muted) any = true;
            muteSelected(any);
            return;
        }
        break;
    case Qt::Key_Left:
    case Qt::Key_Right: {
        if (m_selected.empty()) break;
        Tick dt = (e->key() == Qt::Key_Left ? -1 : 1) * ((ctrl || alt) ? PPQ / 32 : snapTicks());
        beginEdit();
        Tick minS = LLONG_MAX;
        for (auto& c : proj.clips)
            if (m_selected.count(c.id)) minS = std::min(minS, c.start);
        if (minS + dt < 0) dt = -minS;
        for (auto& c : proj.clips)
            if (m_selected.count(c.id)) c.start += dt;
        commitEdit(alt ? "Nudge clips" : "Move clips");
        return;
    }
    case Qt::Key_Up:
    case Qt::Key_Down: {
        if (m_selected.empty()) break;
        int dir = e->key() == Qt::Key_Up ? -1 : 1;
        auto& tr = proj.playlistTracks;
        auto idx = [&](Id id) {
            for (size_t i = 0; i < tr.size(); ++i)
                if (tr[i].id == id) return (int)i;
            return -1;
        };
        for (auto& c : proj.clips)
            if (m_selected.count(c.id)) {
                int i = idx(c.trackId);
                if (i + dir < 0 || i + dir >= (int)tr.size()) return;
            }
        beginEdit();
        for (auto& c : proj.clips)
            if (m_selected.count(c.id)) c.trackId = tr[idx(c.trackId) + dir].id;
        commitEdit("Move clips");
        return;
    }
    case Qt::Key_4: pickTool(Slice); return;
    case Qt::Key_5: pickTool(MuteTool); return;
    default: break;
    }
    if (handleSharedKey(e)) return;
    QWidget::keyPressEvent(e);
}
// clipboard: clips are copied relative to the earliest one and pasted at the playhead on their original tracks
void PlaylistView::copySelected() {
    m_clipboard.clear();
    Tick minS = LLONG_MAX;
    for (auto& c : m_app.project().clips)
        if (m_selected.count(c.id)) {
            m_clipboard.push_back(c);
            minS = std::min(minS, c.start);
        }
    for (auto& c : m_clipboard) c.start -= minS;
}
void PlaylistView::paste() {
    if (m_clipboard.empty()) return;
    Tick at = snap((Tick)m_app.engine().positionTicks());
    m_selected.clear();
    for (auto& c : m_clipboard) {
        if (!m_app.project().track(c.trackId)) continue;
        Id id = m_app.duplicateClip(c.id, at + c.start, c.trackId);
        if (id) m_selected.insert(id);
    }
    invalidate();
}
void PlaylistView::selectBySource() {
    auto& proj = m_app.project();
    if (m_selected.empty()) return;
    std::set<Id> src;
    for (auto& c : proj.clips)
        if (m_selected.count(c.id)) src.insert(c.sourceId);
    for (auto& c : proj.clips)
        if (src.count(c.sourceId)) m_selected.insert(c.id);
    invalidate();
}
void PlaylistView::sliceAtPlayhead() {
    Tick at = (Tick)m_app.engine().positionTicks();
    std::vector<Id> ids;
    for (auto& c : m_app.project().clips)
        if ((m_selected.empty() || m_selected.count(c.id)) && at > c.start && at < c.start + m_app.project().clipLength(c))
            ids.push_back(c.id);
    if (ids.empty()) return;
    for (Id id : ids) m_app.splitClip(id, at);
}
void PlaylistView::invertSelection() {
    std::set<Id> inv;
    for (auto& c : m_app.project().clips)
        if (!m_selected.count(c.id)) inv.insert(c.id);
    m_selected = inv;
    invalidate();
}
// Ctrl+Delete / Ctrl+Insert: remove or insert the loop area's span, shifting everything after it
void PlaylistView::deleteSpace() {
    auto& t = m_app.project().transport;
    if (t.loopEnd <= t.loopStart) return;
    Tick len = t.loopEnd - t.loopStart;
    std::vector<Id> del;
    std::set<Id> keep = m_selected;
    m_selected.clear();
    for (auto& c : m_app.project().clips) {
        if (c.start >= t.loopStart && c.start < t.loopEnd) del.push_back(c.id);
        else if (c.start >= t.loopEnd) m_selected.insert(c.id);
    }
    m_app.deleteClips(del);
    if (!m_selected.empty()) {
        beginEdit();
        for (auto& c : m_app.project().clips)
            if (m_selected.count(c.id)) c.start -= len;
        commitEdit("Delete space");
    }
    m_selected = keep;
    for (Id id : del) m_selected.erase(id);
    invalidate();
}
void PlaylistView::insertSpace() {
    auto& t = m_app.project().transport;
    if (t.loopEnd <= t.loopStart) return;
    Tick len = t.loopEnd - t.loopStart;
    std::set<Id> keep = m_selected;
    m_selected.clear();
    for (auto& c : m_app.project().clips)
        if (c.start >= t.loopStart) m_selected.insert(c.id);
    if (!m_selected.empty()) {
        beginEdit();
        for (auto& c : m_app.project().clips)
            if (m_selected.count(c.id)) c.start += len;
        commitEdit("Insert space");
    }
    m_selected = keep;
    invalidate();
}
void PlaylistView::contextMenuEvent(QContextMenuEvent* e) {
    e->accept();
}   // area menus open on right-button release (see mouseReleaseEvent) so right-drag can erase
void PlaylistView::showAreaMenu(const QPoint& pos, const QPoint& globalPos) {
    Layout L = layout();
    if (!L.area.contains(pos)) return;
    QMenu m(this);
    m.addAction("Select all", [this] { selectAll(); });
    m.addAction("Add track", [this] { m_app.addPlaylistTrack(); });
    m.addAction("Zoom to fit", [this] { zoomToFit(); });
    int tr = trackAtY(pos.y());
    Tick at = snap(xToTick(pos.x()));
    if (tr >= 0) {
        Id tid = m_app.project().playlistTracks[tr].id;
        auto* pm = m.addMenu("Insert pattern");
        for (auto& p : m_app.project().patterns) pm->addAction(p.name, [this, tid, pid = p.id, at] { m_app.addPatternClip(tid, pid, at); });
        auto* am = m.addMenu("Insert automation clip");
        for (auto& l : m_app.project().automationLanes)
            am->addAction(l.name, [this, tid, lid = l.id, at] { m_app.addAutomationClip(tid, lid, at, 0); });
    }
    m.exec(globalPos);
}
void PlaylistView::showTrackMenu(int idx, const QPoint& global) {
    auto& proj = m_app.project();
    if (idx < 0 || idx >= (int)proj.playlistTracks.size()) return;
    PlaylistTrack t = proj.playlistTracks[idx];
    Id id = t.id;
    QMenu m(this);
    m.addAction("Rename...", [this, id, t] {
        if (auto n = askName(this, "Rename track", t.name)) m_app.edit<PlaylistTrack>(id, "Rename track", [n = *n](PlaylistTrack& x) { x.name = n; });
    });
    m.addAction("Color...", [this, id, t] {
        QColor c = QColorDialog::getColor(t.color, this);
        if (c.isValid()) m_app.edit<PlaylistTrack>(id, "Track color", [c](PlaylistTrack& x) { x.color = c; });
    });
    m.addAction("Insert track above", [this, idx] {
        Id nid = m_app.addPlaylistTrack();
        m_app.model().reorder<PlaylistTrack>(nid, idx, "Move track");
    });
    m.addAction("Add track", [this] { m_app.addPlaylistTrack(); });
    m.addAction("Delete track", [this, id] { m_app.deletePlaylistTrack(id); });
    m.addSeparator();
    fillMixerTrackMenu(m.addMenu("Audio clips to mixer track"), proj, t.mixerTrackId,
        [this, id](Id mid) { m_app.edit<PlaylistTrack>(id, "Track mixer routing", [mid](PlaylistTrack& x) { x.mixerTrackId = mid; }); });
    auto* h = m.addMenu("Height");
    for (int hh : {30, 60, 100, 160})
        h->addAction(
            QString::number(hh), [this, id, hh] { m_app.edit<PlaylistTrack>(id, "Track height", [hh](PlaylistTrack& x) { x.height = hh; }); });
    m.exec(global);
}
void PlaylistView::showClipMenu(Clip* c, const QPoint& global) {
    if (!c) return;
    if (!m_selected.count(c->id)) {
        m_selected = {c->id};
        invalidate();
    }
    Id id = c->id;
    ClipType type = c->type;
    QMenu m(this);
    auto& proj = m_app.project();
    m.addAction(c->muted ? "Unmute" : "Mute", [this, id] { m_app.edit<Clip>(id, "Mute clip", [](Clip& x) { x.muted = !x.muted; }); });
    m.addAction("Delete", [this] { deleteSelected(); });
    m.addAction("Duplicate", [this] { duplicateSelected(); });
    m.addAction("Split at playhead", [this, id] { m_app.splitClip(id, (Tick)m_app.engine().positionTicks()); });
    m.addAction("Rename...", [this, id, n = c->name] {
        if (auto nn = askName(this, "Rename clip", n, true)) m_app.edit<Clip>(id, "Rename clip", [nn = *nn](Clip& x) { x.name = nn; });
    });
    m.addSeparator();
    if (type == ClipType::Pattern) {
        m.addAction("Make unique (clone pattern)", [this, id] { m_app.makeClipUnique(id); });
        m.addAction("Open in piano roll", [this, pid = c->sourceId] {
            m_app.selectPattern(pid);
            Q_EMIT openPattern(pid);
        });
        m.addAction(c->loopSource ? "Don't repeat pattern" : "Repeat pattern (when the clip is longer than the pattern)",
            [this, id] { m_app.edit<Clip>(id, "Repeat pattern", [](Clip& x) { x.loopSource = !x.loopSource; }); });
        if (c->length > 0)
            m.addAction("Fit to pattern length", [this, id] { m_app.edit<Clip>(id, "Fit clip to pattern", [](Clip& x) { x.length = 0; }); });
    }
    if (type == ClipType::Audio) {
        m.addAction("Edit audio clip...", [this, id] { Q_EMIT openAudioClip(id); });
        m.addAction("Edit audio...", [this, sid = c->sourceId] { openAudioEditor(m_app, this, sid, 0); });
        if (std::any_of(proj.clips.begin(), proj.clips.end(),
                [c](const Clip& x) { return x.type == ClipType::Audio && x.sourceId == c->sourceId && x.trackId != c->trackId; }))
            m.addAction("Use this take here", [this, id] { useTake(id); });
        m.addAction(c->reverse ? "Un-reverse" : "Reverse",
            [this, id] { m_app.edit<Clip>(id, "Reverse clip", [](Clip& x) { x.reverse = !x.reverse; }); });
        m.addAction("Normalize (source)",
            [this, sid = c->sourceId] { m_app.edit<AudioSource>(sid, "Normalize", [](AudioSource& a) { a.normalize = !a.normalize; }); });
        m.addAction("Gain...", [this, id, g = c->gain] {
            bool ok;
            double db = QInputDialog::getDouble(this, "Clip gain", "dB:", gainToDb(g), -60, 24, 1, &ok);
            if (ok) m_app.edit<Clip>(id, "Clip gain", [db](Clip& x) { x.gain = dbToGain((float)db); });
        });
        m.addAction("Fade in...", [this, id, c] {
            bool ok;
            double beats = QInputDialog::getDouble(this, "Fade in", "Beats:", ticksToBeats(c->fadeIn), 0, 64, 2, &ok);
            if (ok) m_app.edit<Clip>(id, "Fade in", [beats](Clip& x) { x.fadeIn = beatsToTicks(beats); });
        });
        m.addAction("Fade out...", [this, id, c] {
            bool ok;
            double beats = QInputDialog::getDouble(this, "Fade out", "Beats:", ticksToBeats(c->fadeOut), 0, 64, 2, &ok);
            if (ok) m_app.edit<Clip>(id, "Fade out", [beats](Clip& x) { x.fadeOut = beatsToTicks(beats); });
        });
        m.addAction("Crossfade with overlapping clip", [this] { crossfadeSelected(); });
        m.addAction("Time stretch / pitch...", [this, id] { Q_EMIT openAudioClip(id); });
        m.addAction("Stretch to whole bars", [this, id, c] {
            const AudioSource* s = m_app.project().audioSource(c->sourceId);
            if (!s || s->sampleRate <= 0) return;
            double secs = s->frames / s->sampleRate;
            TempoMap tm = m_app.project().transport.tempoMap();
            Tick nat = tm.ticksFor(c->start, secs);
            Tick bar = tm.sigAt(c->start).ticksPerBar();
            Tick target = std::max<Tick>(bar, ((nat + bar / 2) / bar) * bar);
            double ratio = (double)target / nat;
            m_app.edit<Clip>(id, "Stretch to whole bars", [ratio, target](Clip& x) {
                x.timeStretch = ratio;
                x.length = target;
                if (x.stretchMode == StretchResample) x.stretchMode = StretchGeneral;   // keep the pitch
                x.fitTempo = false;
            });
        });
        m.addAction("Slice at transients", [this, id, c] {
            const AudioSource* s = m_app.project().audioSource(c->sourceId);
            auto a = s ? m_app.cache().get(s->filePath) : nullptr;
            if (!a) return;
            std::vector<Tick> cuts;
            float env = 0;
            int64_t last = -100000;
            TempoMap tm = m_app.project().transport.tempoMap();
            for (int64_t i = 0; i < a->frames; ++i) {
                float x = std::fabs(a->L()[i]);
                if (x > env * 3.f && x > 0.15f && i - last > a->sampleRate * 0.08) {
                    last = i;
                    cuts.push_back(tm.ticksFor(c->start, i / a->sampleRate));
                }
                env = env * 0.999f + x * 0.001f;
            }
            Tick start = c->start;
            for (auto it = cuts.rbegin(); it != cuts.rend(); ++it) {
                if (*it <= 0) continue;
                m_app.splitClip(id, start + *it);
            }
        });
        m.addAction(c->loopSource ? "Don't loop audio" : "Loop audio",
            [this, id] { m_app.edit<Clip>(id, "Loop audio", [](Clip& x) { x.loopSource = !x.loopSource; }); });
    }
    if (type == ClipType::Automation) {
        m.addAction("Delete automation lane", [this, lid = c->sourceId] { m_app.deleteLane(lid); });
        auto* cm = m.addMenu("Curve type (all points)");
        const char* names[] = {"Linear", "Stepped", "Smooth", "Bezier"};
        for (int i = 0; i < 4; ++i)
            cm->addAction(names[i], [this, lid = c->sourceId, i] {
                m_app.edit<AutomationLane>(lid, "Curve type", [i](AutomationLane& l) {
                    for (auto& p : l.points) p.curve = (CurveType)i;
                });
            });
        int pi = autoPointAt(*c, mapFromGlobal(global));
        if (pi >= 0) {
            m.addAction("Delete point", [this, lid = c->sourceId, pi] {
                m_app.edit<AutomationLane>(lid, "Delete point", [pi](AutomationLane& l) {
                    if (l.points.size() > 1 && pi < (int)l.points.size()) l.points.erase(l.points.begin() + pi);
                });
            });
            auto* pcm = m.addMenu("Point curve");
            for (int i = 0; i < 4; ++i)
                pcm->addAction(names[i], [this, lid = c->sourceId, pi, i] {
                    m_app.edit<AutomationLane>(lid, "Curve type", [pi, i](AutomationLane& l) {
                        if (pi < (int)l.points.size()) l.points[pi].curve = (CurveType)i;
                    });
                });
            m.addAction("Tension...", [this, lid = c->sourceId, pi] {
                bool ok;
                double t = QInputDialog::getDouble(this, "Tension", "-1..1:", 0, -1, 1, 2, &ok);
                if (ok)
                    m_app.edit<AutomationLane>(lid, "Tension", [pi, t](AutomationLane& l) {
                        if (pi < (int)l.points.size()) {
                            l.points[pi].tension = (float)t;
                            l.points[pi].curve = CurveType::Bezier;
                        }
                    });
            });
        }
    }
    (void)proj;
    m.exec(global);
}
// comping: the clicked take plays in the time selection (the loop region), or over its own length when there is none
void PlaylistView::useTake(Id clipId) {
    const auto& tr = m_app.project().transport;
    const Clip* c = m_app.project().clip(clipId);
    if (!c) return;
    if (tr.loopEnabled && tr.loopEnd > tr.loopStart) m_app.useTake(clipId, tr.loopStart, tr.loopEnd);
    else m_app.useTake(clipId, c->start, c->start + c->length);
}
void PlaylistView::deleteSelected() {
    std::vector<Id> ids(m_selected.begin(), m_selected.end());
    m_selected.clear();
    m_app.deleteClips(ids);
}
void PlaylistView::selectAll() {
    for (auto& c : m_app.project().clips) m_selected.insert(c.id);
    invalidate();
}
void PlaylistView::muteSelected(bool m) {
    std::vector<std::unique_ptr<Command>> cmds;
    for (Id id : m_selected)
        cmds.push_back(std::make_unique<EditObjectCommand<Clip>>(m_app.model(), id, "", [m](Clip& x) { x.muted = m; }, 0));
    if (!cmds.empty()) m_app.model().macro(m ? "Mute clips" : "Unmute clips", std::move(cmds));
}
void PlaylistView::duplicateSelected() {
    auto& proj = m_app.project();
    const auto& tr = proj.transport;
    // FL-style: with a loop (time) selection, duplicate everything inside it to right after it and move the selection along
    if (tr.loopEnabled && tr.loopEnd > tr.loopStart) {
        Tick span = tr.loopEnd - tr.loopStart;
        std::vector<std::unique_ptr<Command>> cmds;
        std::set<Id> newSel;
        for (auto& c : proj.clips) {
            Tick cs = c.start, ce = c.start + proj.clipLength(c);
            bool inside = m_selected.empty() ? (cs < tr.loopEnd && ce > tr.loopStart) : m_selected.count(c.id) > 0;
            if (!inside) continue;
            Clip d = c;
            d.id = proj.newId();
            d.start += span;
            newSel.insert(d.id);
            cmds.push_back(std::make_unique<InsertObjectCommand<Clip>>(m_app.model(), d, "", -1));
        }
        if (cmds.empty()) return;
        m_app.model().macro("Duplicate loop area", std::move(cmds));
        m_selected = newSel;
        m_app.setLoopRange(tr.loopStart + span, tr.loopEnd + span);
        invalidate();
        return;
    }
    if (m_selected.empty()) return;
    Tick minS = LLONG_MAX, maxE = 0;
    for (auto& c : proj.clips)
        if (m_selected.count(c.id)) {
            minS = std::min(minS, c.start);
            maxE = std::max(maxE, c.start + proj.clipLength(c));
        }
    Tick span = maxE - minS;
    std::vector<std::unique_ptr<Command>> cmds;
    std::set<Id> newSel;
    for (auto& c : proj.clips)
        if (m_selected.count(c.id)) {
            Clip d = c;
            d.id = proj.newId();
            d.start += span;
            newSel.insert(d.id);
            cmds.push_back(std::make_unique<InsertObjectCommand<Clip>>(m_app.model(), d, "", -1));
        }
    m_app.model().macro("Duplicate clips", std::move(cmds));
    m_selected = newSel;
    invalidate();
}
void PlaylistView::crossfadeSelected() {
    auto& proj = m_app.project();
    std::vector<Clip*> sel;
    for (auto& c : proj.clips)
        if (m_selected.count(c.id) && c.type == ClipType::Audio) sel.push_back(&c);
    if (sel.size() != 2) {
        m_app.statusMessage("Select exactly two overlapping audio clips", 3000);
        return;
    }
    std::sort(sel.begin(), sel.end(), [](Clip* a, Clip* b) { return a->start < b->start; });
    Clip* a = sel[0];
    Clip* b = sel[1];
    Tick overlap = (a->start + proj.clipLength(*a)) - b->start;
    if (overlap <= 0) {
        m_app.statusMessage("Clips do not overlap", 3000);
        return;
    }
    std::vector<std::unique_ptr<Command>> cmds;
    cmds.push_back(std::make_unique<EditObjectCommand<Clip>>(m_app.model(), a->id, "", [overlap](Clip& x) { x.fadeOut = overlap; }, 0));
    cmds.push_back(std::make_unique<EditObjectCommand<Clip>>(m_app.model(), b->id, "", [overlap](Clip& x) { x.fadeIn = overlap; }, 0));
    m_app.model().macro("Crossfade", std::move(cmds));
}
void PlaylistView::dragEnterEvent(QDragEnterEvent* e) {
    if (hasAppDrop(e->mimeData())) e->acceptProposedAction();
}
void PlaylistView::dragMoveEvent(QDragMoveEvent* e) { e->acceptProposedAction(); }
void PlaylistView::dropEvent(QDropEvent* e) {
    QPoint pos = e->position().toPoint();
    int tr = trackAtY(pos.y());
    auto& proj = m_app.project();
    if (tr < 0) {
        if (proj.playlistTracks.empty()) m_app.addPlaylistTrack();
        tr = (int)proj.playlistTracks.size() - 1;
    }
    Id tid = proj.playlistTracks[tr].id;
    Tick at = std::max<Tick>(0, snap(xToTick(pos.x())));
    auto [kind, val] = dropItem(e->mimeData());
    if (e->mimeData()->hasUrls()) {
        for (auto& u : e->mimeData()->urls()) {
            QString p = u.toLocalFile();
            if (AudioFile::isAudioFile(p)) {
                m_app.addAudioClip(tid, p, at);
            } else if (MidiFile::isMidiFile(p)) m_app.importMidiFile(p, tid, at);
            else if (p.endsWith(".mtp")) m_app.openProject(p);
        }
    } else if (kind == "pattern") m_app.addPatternClip(tid, val.toULongLong(), at);
    else if (kind == "lane") m_app.addAutomationClip(tid, val.toULongLong(), at, 0);
    e->acceptProposedAction();
}

// ---------------------------------------------------------------- panel
PlaylistPanel::PlaylistPanel(App& app, QWidget* parent)
    : QWidget(parent)
    , m_app(app) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(2, 2, 2, 2);
    lay->setSpacing(2);
    auto* top = new QHBoxLayout();
    top->setSpacing(4);
    // Same toolbar language as the transport and piano roll: one shared height, flush tool groups, rules between groups.
    const int kH = theme::kBarH;
    auto sep = [&] {
        top->addSpacing(4);
        top->addWidget(theme::barSeparator(this));
        top->addSpacing(4);
    };
    const theme::Glyph icons[] = {
        theme::Glyph::Pencil, theme::Glyph::Marquee, theme::Glyph::Eraser, theme::Glyph::Slice, theme::Glyph::MuteSpeaker};
    const char* tips[] = {"Draw (1): click to place the selected pattern; drag clips to move, edges to resize; right-click deletes",
        "Select (2): drag a rectangle to select clips", "Erase (3)", "Slice (4): click a clip to split it",
        "Mute (5): click a clip to toggle mute; Shift+click a recorded take to use it in the time selection (or under it) and mute the "
        "other takes there"};
    auto* tg = new QHBoxLayout();
    tg->setSpacing(1);
    tg->setContentsMargins(0, 0, 0, 0);
    top->addLayout(tg);
    for (int i = 0; i < 5; ++i) {
        auto* b = new QToolButton(this);
        theme::setGlyph(b, icons[i], 24);
        b->setToolTip(tips[i]);
        b->setCheckable(true);
        b->setChecked(i == 0);
        b->setFixedSize(kH + 4, kH);
        b->setFocusPolicy(Qt::NoFocus);
        tg->addWidget(b);
        m_tools.push_back(b);
        connect(b, &QToolButton::clicked, this, [this, i] {
            m_view->setTool((PlaylistView::Tool)i);
            for (int k = 0; k < 5; ++k) m_tools[k]->setChecked(k == i);
        });
    }
    sep();
    m_snap = TransportBar::makeSnapCombo(this);
    m_snap->setCurrentIndex(1);
    m_snap->setFixedHeight(kH);
    m_snap->setFixedWidth(96);
    m_snap->setToolTip("Snap: the grid clips are placed and resized to (hold Alt to bypass)");
    m_snap->setFocusPolicy(Qt::NoFocus);
    top->addWidget(m_snap);
    sep();
    auto* addTrack = new QToolButton(this);
    addTrack->setText("+ Track");
    addTrack->setToolTip("Add a playlist track");
    addTrack->setFixedHeight(kH);
    addTrack->setFocusPolicy(Qt::NoFocus);
    top->addWidget(addTrack);
    auto* fit = new QToolButton(this);
    fit->setText("Fit");
    fit->setToolTip("Zoom to fit the song");
    fit->setFixedHeight(kH);
    fit->setFocusPolicy(Qt::NoFocus);
    top->addWidget(fit);
    top->addStretch(1);
    auto* hint = theme::dimLabel(
        "Drag samples / patterns / automation from the Browser · Ctrl+drag the ruler selects the loop area · Ctrl+B duplicates it · Ctrl+Shift+drag duplicates clips",
        this, "font-size:8pt;");
    hint->setToolTip(hint->text());
    hint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);   // tips may be cut short: never widen the window past the screen
    top->addWidget(hint, 1);
    lay->addLayout(top);
    m_view = new PlaylistView(app, this);
    theme::roundedFrame(m_view);
    lay->addWidget(m_view, 1);
    m_view->setSnapDivision(-1);
    auto showMode = [this](int t) {
        for (int k = 0; k < 5; ++k) m_tools[k]->setChecked(k == t);
    };
    connect(m_view, &PlaylistView::toolChanged, this, showMode);
    connect(m_view, &PlaylistView::modeChanged, this, showMode);
    connect(m_snap, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_view->setSnapDivision(TransportBar::snapDivisionFromIndex(i));
        m_view->refresh();
    });
    connect(addTrack, &QToolButton::clicked, this, [this] { m_app.addPlaylistTrack(); });
    connect(fit, &QToolButton::clicked, this, [this] { m_view->zoomToFit(); });
    connect(m_view, &PlaylistView::openPattern, this, &PlaylistPanel::openPattern);
    connect(m_view, &PlaylistView::openAudioClip, this, &PlaylistPanel::openAudioClip);
    connect(&app.cache(), &SampleCache::assetLoaded, this, [this](const QString&) { m_view->refresh(); });
}
void PlaylistPanel::refresh(uint32_t) { m_view->refresh(); }
void PlaylistPanel::uiTick() { m_view->uiTick(); }

}   // namespace mt
