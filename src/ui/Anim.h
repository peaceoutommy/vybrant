// Shared UI animation helpers: one duration, one easing curve, one ticker - so motion feels consistent everywhere.
#pragma once
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <QRectF>
#include <QColor>
#include <QElapsedTimer>
#include <QEasingCurve>
#include <functional>
#include <map>
#include <vector>

namespace mt::anim {

constexpr int kDurationMs = 160;                 // standard duration for appear / move / remove
inline double ease(double t) { static const QEasingCurve c(QEasingCurve::OutCubic); return c.valueForProgress(std::clamp(t, 0.0, 1.0)); }
inline qint64 now() { static QElapsedTimer t; if (!t.isValid()) t.start(); return t.elapsed(); }
inline QRectF lerp(const QRectF& a, const QRectF& b, double t) { return QRectF(a.x() + (b.x() - a.x()) * t, a.y() + (b.y() - a.y()) * t, a.width() + (b.width() - a.width()) * t, a.height() + (b.height() - a.height()) * t); }

// Animated rectangles for a set of items (notes, clips): eases each item's displayed rect towards its target,
// fades/scales new items in and keeps ghosts of removed ones that fade out. Call begin() / item() / end() per paint.
template <class Key>
class RectTracker {
public:
    struct State { QRectF from, to, cur; qint64 t0 = 0; bool fresh = true; };
    struct Ghost { QRectF rect; QColor color; qint64 t0; };
    // Returns the rect to draw for 'key' whose target is 'target'; 'instant' skips the animation (item being dragged)
    QRectF item(const Key& key, const QRectF& target, bool instant, double* alpha = nullptr) {
        qint64 t = now(); auto it = m_states.find(key); m_seen.push_back(key);
        if (it == m_states.end()) { State s; s.from = s.to = s.cur = target; s.t0 = instant || !m_enabled ? 0 : t; s.fresh = !instant && m_enabled; it = m_states.emplace(key, s).first; }
        State& s = it->second;
        if (instant) { s.from = s.to = s.cur = target; s.t0 = 0; s.fresh = false; if (alpha) *alpha = 1; return target; }
        if (s.to != target) { s.from = s.cur; s.to = target; s.t0 = t; s.fresh = false; }
        double p = s.t0 ? ease((double)(t - s.t0) / kDurationMs) : 1.0; if (t - s.t0 >= kDurationMs) { s.t0 = 0; p = 1; }
        if (s.fresh) { // appear: grow from the centre + fade in
            double sc = 0.6 + 0.4 * p; QPointF c = target.center(); s.cur = QRectF(c.x() - target.width() * sc / 2, c.y() - target.height() * sc / 2, target.width() * sc, target.height() * sc); if (alpha) *alpha = p; if (p >= 1) s.fresh = false;
        } else { s.cur = lerp(s.from, s.to, p); if (alpha) *alpha = 1; }
        if (p < 1) m_active = true; return s.cur;
    }
    void begin() { m_seen.clear(); m_active = false; }
    // Items not seen this frame were removed: keep a fading ghost
    void end(const std::function<QColor(const Key&)>& ghostColor) {
        qint64 t = now();
        for (auto it = m_states.begin(); it != m_states.end();) { bool seen = false; for (auto& k : m_seen) if (k == it->first) { seen = true; break; } if (!seen) { if (m_enabled) m_ghosts.push_back({it->second.cur, ghostColor(it->first), t}); it = m_states.erase(it); } else ++it; }
        for (auto it = m_ghosts.begin(); it != m_ghosts.end();) { if (t - it->t0 >= kDurationMs) it = m_ghosts.erase(it); else ++it; }
        if (!m_ghosts.empty()) m_active = true;
    }
    template <class Fn> void drawGhosts(Fn draw) { qint64 t = now(); for (auto& g : m_ghosts) { double p = ease((double)(t - g.t0) / kDurationMs); double sc = 1.0 - 0.3 * p; QPointF c = g.rect.center(); QRectF r(c.x() - g.rect.width() * sc / 2, c.y() - g.rect.height() * sc / 2, g.rect.width() * sc, g.rect.height() * sc); draw(r, g.color, 1.0 - p); } }
    bool active() const { return m_active; }
    void forget(const Key& k) { m_states.erase(k); }
    void clear() { m_states.clear(); m_ghosts.clear(); }
    void setEnabled(bool e) { m_enabled = e; }
private:
    std::map<Key, State> m_states; std::vector<Ghost> m_ghosts; std::vector<Key> m_seen; bool m_active = false, m_enabled = true;
};

// Short "pulse" highlights (e.g. a toggled step): value 1 -> 0 over the standard duration
class Pulses {
public:
    void trigger(int key) { m_map[key] = now(); }
    double value(int key) { auto it = m_map.find(key); if (it == m_map.end()) return 0; double p = (double)(now() - it->second) / kDurationMs; if (p >= 1) { m_map.erase(it); return 0; } return 1.0 - ease(p); }
    bool active() const { return !m_map.empty(); }
private:
    std::map<int, qint64> m_map;
};

// Repaints a widget at ~60 fps while something is animating
class Ticker : public QObject {
public:
    explicit Ticker(QWidget* w) : QObject(w), m_w(w) { m_timer.setInterval(16); QObject::connect(&m_timer, &QTimer::timeout, this, [this] { m_w->update(); }); }
    void keepAlive(bool animating) { if (animating && !m_timer.isActive()) m_timer.start(); else if (!animating && m_timer.isActive()) m_timer.stop(); }
private:
    QWidget* m_w; QTimer m_timer;
};

inline bool enabled() { static int cached = -1; if (cached < 0) cached = 1; return cached != 0; }

} // namespace mt::anim
