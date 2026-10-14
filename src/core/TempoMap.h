// Tempo map: tempo changes (jumps or linear ramps) and time-signature changes along the song. Converts ticks <-> seconds by
// integrating the tempo and ticks <-> bars through the signature changes. Built on the UI thread; every query is
// allocation-free, so the audio thread reads the copy inside its snapshot.
#pragma once
#include "core/Types.h"
#include <vector>

namespace mt {

struct TempoPoint {
    Tick tick = 0;
    double bpm = 120;
    bool ramp = false;   // glide linearly to the next point's tempo instead of jumping there
};
struct TimeSigChange {
    int bar = 0;   // 0-based bar the signature starts at: changes stay on bar lines by construction
    TimeSignature sig;
};

class TempoMap {
public:
    // bpm / sig rule from tick 0 until the first change; a change at bar or tick 0 replaces them
    explicit TempoMap(
        double bpm = 120, const std::vector<TempoPoint>& points = {}, TimeSignature sig = {}, const std::vector<TimeSigChange>& sigs = {});
    bool hasTempoPoints() const { return m_hasPoints; }   // false: the single tempo rules (and may be set from outside)
    double bpmAt(double tick) const;
    double secondsAt(double tick) const;   // negative ticks extrapolate the first tempo
    double tickAt(double seconds) const;
    Tick ticksFor(Tick start, double seconds) const { return (Tick)std::llround(tickAt(secondsAt(start) + seconds)) - start; }
    Tick nextTempoPoint(double tick) const;   // first tempo point after tick (INT64_MAX when none)
    TimeSignature sigAt(Tick t) const;
    int64_t barAt(Tick t) const;   // 0-based bar containing t
    Tick barStart(int64_t bar) const;

private:
    struct Seg {
        double tick, len, bpm, endBpm, sec;   // the tempo glides from bpm to endBpm over len ticks (the last segment holds bpm)
    };
    struct SigSeg {
        int64_t bar;
        Tick tick;
        TimeSignature sig;
    };
    const Seg& segAtTick(double tick) const;
    static double segSeconds(const Seg& s, double dt);   // seconds from the segment start to dt ticks into it
    std::vector<Seg> m_seg;
    std::vector<SigSeg> m_sig;
    bool m_hasPoints = false;
};

}   // namespace mt
