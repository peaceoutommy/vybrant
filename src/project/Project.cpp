#include "project/Project.h"
#include <QDateTime>
#include <set>

namespace mt {

Tick Pattern::effectiveLength(const TimeSignature& ts) const {
    if (length > 0) return length;
    Tick maxEnd = 0;
    for (auto& c : channels) {
        for (auto& n : c.notes) maxEnd = std::max(maxEnd, n.start + n.duration);
        int lastStep = -1; for (size_t i = 0; i < c.steps.size(); ++i) if (c.steps[i].enabled) lastStep = (int)i;
        if (lastStep >= 0) maxEnd = std::max(maxEnd, (Tick)(lastStep + 1) * stepTicks());
    }
    Tick bar = ts.ticksPerBar();
    Tick stepsLen = (Tick)stepCount * stepTicks();
    Tick base = std::max(maxEnd, stepsLen);
    if (base <= 0) return bar;
    return ((base + bar - 1) / bar) * bar;   // round up to whole bars
}

float AutomationLane::valueAt(Tick t) const { return evalAutomation(points, t); }

float evalAutomation(const std::vector<AutomationPoint>& points, Tick t) {
    if (points.empty()) return 0.f;
    if (t <= points.front().time) return points.front().value;
    if (t >= points.back().time) return points.back().value;
    size_t i = 0;
    // binary search for segment
    size_t lo = 0, hi = points.size() - 1;
    while (hi - lo > 1) { size_t mid = (lo + hi) / 2; if (points[mid].time <= t) lo = mid; else hi = mid; }
    i = lo;
    const auto& a = points[i]; const auto& b = points[i + 1];
    if (b.time == a.time) return b.value;
    float x = (float)(t - a.time) / (float)(b.time - a.time);
    switch (a.curve) {
    case CurveType::Stepped: return a.value;
    case CurveType::Smooth: { float s = x * x * (3.f - 2.f * x); return a.value + (b.value - a.value) * s; }
    case CurveType::Bezier: {
        // tension in -1..1 bends the curve towards either end point
        float tn = clamp(a.tension, -1.f, 1.f);
        float p = tn >= 0 ? 1.f + tn * 3.f : 1.f / (1.f - tn * 3.f);
        float s = std::pow(x, p);
        return a.value + (b.value - a.value) * s;
    }
    default: return a.value + (b.value - a.value) * x;
    }
}

bool Project::routingWouldCycle(Id from, Id to) const {
    // Walk downstream from 'to'; if we reach 'from', a cycle would form.
    std::set<Id> seen; Id cur = to; int guard = 0;
    while (cur != 0 && guard++ < 10000) {
        if (cur == from) return true;
        if (!seen.insert(cur).second) return true;
        const MixerTrack* t = mixerTrack(cur);
        if (!t) break;
        for (auto& s : t->sends) if (s.targetTrackId == from) return true;
        cur = t->outputTrackId;
    }
    return false;
}

Project makeDefaultProject(bool demo) {
    Project p;
    p.meta.created = QDateTime::currentDateTime().toString(Qt::ISODate);
    MixerTrack master; master.id = p.newId(); master.name = "Master"; master.isMaster = true; master.color = QColor(0xd0, 0x70, 0x50);
    p.mixerTracks.push_back(master);
    for (int i = 1; i <= 16; ++i) { MixerTrack t; t.id = p.newId(); t.name = QString("Insert %1").arg(i); t.outputTrackId = master.id; p.mixerTracks.push_back(t); }
    const char* names[] = {"Kick", "Clap", "Hat", "Snare"};
    const char* colors[] = {"#e2665c", "#e0a040", "#6dc26d", "#5aa9e6"};
    if (demo) for (int i = 0; i < 4; ++i) {
        Channel c; c.id = p.newId(); c.name = names[i]; c.color = QColor(colors[i]); c.type = ChannelType::DrumSampler; c.instrumentKey = "native.drumsampler"; c.mixerTrackId = p.mixerTracks[1 + i].id;
        c.instrumentState = QJsonObject{{"builtin", names[i]}};
        p.channels.push_back(c);
    }
    if (demo) { Channel c; c.id = p.newId(); c.name = "Synth"; c.color = QColor("#c07be0"); c.type = ChannelType::Synth; c.instrumentKey = "native.synth"; c.mixerTrackId = p.mixerTracks[5].id; p.channels.push_back(c); }
    Pattern pat; pat.id = p.newId(); pat.name = "Pattern 1";
    for (auto& c : p.channels) pat.ensureChannel(c.id);
    if (demo) { // A small default beat so the demo project makes sound immediately
    auto& kick = *pat.channelData(p.channels[0].id); for (int s : {0, 4, 8, 12}) kick.steps[s].enabled = true;
    auto& clap = *pat.channelData(p.channels[1].id); for (int s : {4, 12}) clap.steps[s].enabled = true;
    auto& hat = *pat.channelData(p.channels[2].id); for (int s = 0; s < 16; s += 2) { hat.steps[s].enabled = true; hat.steps[s].velocity = (s % 4 == 0) ? 0.9f : 0.55f; }
    // a short synth line in the piano roll
    auto& syn = *pat.channelData(p.channels[4].id); const int melody[8] = {48, 48, 55, 51, 48, 46, 51, 53};
    for (int i = 0; i < 8; ++i) { Note n; n.id = p.newId(); n.pitch = melody[i]; n.start = (Tick)i * PPQ / 2; n.duration = PPQ / 4; n.velocity = 0.7f; syn.notes.push_back(n); } }
    p.patterns.push_back(pat);
    p.selectedPatternId = pat.id; p.selectedChannelId = p.channels.empty() ? 0 : p.channels[0].id;
    for (int i = 1; i <= 8; ++i) { PlaylistTrack t; t.id = p.newId(); t.name = QString("Track %1").arg(i); p.playlistTracks.push_back(t); }
    if (demo) { Clip c; c.id = p.newId(); c.type = ClipType::Pattern; c.trackId = p.playlistTracks[0].id; c.sourceId = pat.id; c.start = 0; c.length = 0; c.loopSource = false; c.name = pat.name; p.clips.push_back(c); }
    p.transport.loopEnd = PPQ * 16;
    return p;
}

} // namespace mt
