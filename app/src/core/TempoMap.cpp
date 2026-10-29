#include "core/TempoMap.h"
#include <limits>

namespace mt {

TempoMap::TempoMap(double bpm, const std::vector<TempoPoint>& points, TimeSignature sig, const std::vector<TimeSigChange>& sigs) {
    m_hasPoints = !points.empty();
    std::vector<TempoPoint> pts = points;
    std::stable_sort(pts.begin(), pts.end(), [](const TempoPoint& a, const TempoPoint& b) { return a.tick < b.tick; });
    std::vector<bool> ramp{false};
    m_seg.push_back({0, 0, std::max(bpm, 1.0), 0, 0});
    for (auto& p : pts) {
        double b = std::max(p.bpm, 1.0);
        if ((double)p.tick > m_seg.back().tick) {
            m_seg.push_back({(double)p.tick, 0, b, 0, 0});
            ramp.push_back(p.ramp);
        } else {
            m_seg.back().bpm = b;
            ramp.back() = p.ramp;
        }
    }
    for (size_t i = 0; i < m_seg.size(); ++i) {
        Seg& s = m_seg[i];
        bool last = i + 1 == m_seg.size();
        s.len = last ? 0 : m_seg[i + 1].tick - s.tick;
        s.endBpm = ramp[i] && !last ? m_seg[i + 1].bpm : s.bpm;
        if (!last) m_seg[i + 1].sec = s.sec + segSeconds(s, s.len);
    }
    std::vector<TimeSigChange> sc = sigs;
    std::stable_sort(sc.begin(), sc.end(), [](const TimeSigChange& a, const TimeSigChange& b) { return a.bar < b.bar; });
    m_sig.push_back({0, 0, sig});
    for (auto& c : sc) {
        SigSeg& b = m_sig.back();
        if (c.bar <= b.bar) b.sig = c.sig;
        else m_sig.push_back({c.bar, b.tick + (c.bar - b.bar) * b.sig.ticksPerBar(), c.sig});
    }
}

const TempoMap::Seg& TempoMap::segAtTick(double tick) const {
    auto it = std::upper_bound(m_seg.begin(), m_seg.end(), tick, [](double t, const Seg& s) { return t < s.tick; });
    return it == m_seg.begin() ? m_seg.front() : *(it - 1);
}

double TempoMap::bpmAt(double tick) const {
    const Seg& s = segAtTick(tick);
    if (s.endBpm == s.bpm) return s.bpm;
    return s.bpm + (s.endBpm - s.bpm) * clamp((tick - s.tick) / s.len, 0.0, 1.0);
}

double TempoMap::segSeconds(const Seg& s, double dt) {
    if (s.endBpm == s.bpm || dt <= 0) return dt * 60.0 / (s.bpm * PPQ);
    double k = (s.endBpm - s.bpm) / s.len;   // bpm per tick; the integral of 60 / (PPQ * bpm(t)) over the ramp
    return 60.0 / (PPQ * k) * std::log((s.bpm + k * dt) / s.bpm);
}

double TempoMap::secondsAt(double tick) const {
    const Seg& s = segAtTick(tick);
    return s.sec + segSeconds(s, tick - s.tick);
}

double TempoMap::tickAt(double seconds) const {
    auto it = std::upper_bound(m_seg.begin(), m_seg.end(), seconds, [](double v, const Seg& s) { return v < s.sec; });
    const Seg& s = it == m_seg.begin() ? m_seg.front() : *(it - 1);
    double ds = seconds - s.sec;
    if (s.endBpm == s.bpm || ds <= 0) return s.tick + ds * s.bpm * PPQ / 60.0;
    double k = (s.endBpm - s.bpm) / s.len;
    return s.tick + (s.bpm * std::exp(ds * PPQ * k / 60.0) - s.bpm) / k;
}

Tick TempoMap::nextTempoPoint(double tick) const {
    auto it = std::upper_bound(m_seg.begin(), m_seg.end(), tick, [](double t, const Seg& s) { return t < s.tick; });
    return it == m_seg.end() ? std::numeric_limits<Tick>::max() : (Tick)it->tick;
}

TimeSignature TempoMap::sigAt(Tick t) const {
    auto it = std::upper_bound(m_sig.begin(), m_sig.end(), t, [](Tick v, const SigSeg& s) { return v < s.tick; });
    return (it == m_sig.begin() ? m_sig.front() : *(it - 1)).sig;
}

int64_t TempoMap::barAt(Tick t) const {
    auto it = std::upper_bound(m_sig.begin(), m_sig.end(), t, [](Tick v, const SigSeg& s) { return v < s.tick; });
    const SigSeg& s = it == m_sig.begin() ? m_sig.front() : *(it - 1);
    Tick d = t - s.tick, bar = s.sig.ticksPerBar();
    return s.bar + (d >= 0 ? d / bar : -((-d + bar - 1) / bar));
}

Tick TempoMap::barStart(int64_t bar) const {
    auto it = std::upper_bound(m_sig.begin(), m_sig.end(), bar, [](int64_t v, const SigSeg& s) { return v < s.bar; });
    const SigSeg& s = it == m_sig.begin() ? m_sig.front() : *(it - 1);
    return s.tick + (bar - s.bar) * s.sig.ticksPerBar();
}

}   // namespace mt
