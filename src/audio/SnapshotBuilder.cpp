// Builds an immutable runtime Snapshot from the Project (UI thread) and manages the runtime registry.
#include "audio/AudioEngine.h"
#include "audio/RecordingEngine.h"
#include "dsp/NativeRegistry.h"
#include "core/Log.h"
#include <set>
#include <queue>
#include <functional>

namespace mt {

// ---------------------------------------------------------------- registry
static std::map<Id, QString>& procKeys() {
    static std::map<Id, QString> k;
    return k;
}
static std::map<Id, Id>& procPluginIds() {
    static std::map<Id, Id> k;
    return k;
}

static QString keyForChannel(const Channel& c) {
    return c.type == ChannelType::Plugin ? QString("plugin:%1").arg(c.pluginInstanceId) : c.instrumentKey;
}
static QString keyForSlot(const EffectSlot& s) {
    return s.pluginInstanceId ? QString("plugin:%1").arg(s.pluginInstanceId) : s.processorKey;
}

IProcessor* RuntimeRegistry::replaceProcessor(Id ownerId, std::unique_ptr<IProcessor> proc, double sr, int maxBlock, uint64_t gen) {
    auto it = m_procs.find(ownerId);
    if (it != m_procs.end()) {
        m_graveyard.emplace_back(gen, std::move(it->second));
        m_procs.erase(it);
    }
    if (!proc) {
        procKeys().erase(ownerId);
        return nullptr;
    }
    proc->prepare(sr, maxBlock);
    IProcessor* raw = proc.get();
    m_procs[ownerId] = std::move(proc);
    return raw;
}

static std::unique_ptr<IProcessor> createFor(const QString& key, const Project& p, Id pluginId, const RuntimeRegistry::PluginFactory& pf) {
    if (key.startsWith("plugin:")) {
        const PluginInstanceData* pd = p.plugin(pluginId);
        if (!pd || !pf) return nullptr;
        return pf(*pd);
    }
    return NativeRegistry::instance().create(key);
}

IProcessor* RuntimeRegistry::ensureInstrument(
    const Channel& ch, const Project& p, double sr, int maxBlock, const PluginFactory& pf, uint64_t gen) {
    QString key = keyForChannel(ch);
    auto it = m_procs.find(ch.id);
    if (it != m_procs.end() && procKeys()[ch.id] == key) return it->second.get();
    auto proc = createFor(key, p, ch.pluginInstanceId, pf);
    if (!proc) {
        if (it != m_procs.end()) replaceProcessor(ch.id, nullptr, sr, maxBlock, gen);
        return nullptr;
    }
    proc->restoreState(ch.instrumentState);
    if (auto* s = dynamic_cast<Sampler*>(proc.get())) {
        if (s->pendingPath.isEmpty() && !ch.samplePath.isEmpty()) {   // a new sampler (no saved state): tempo from names like "83 bpm"
            s->pendingPath = ch.samplePath;
            if (double b = AudioFile::bpmFromName(ch.samplePath); b > 0) s->params().set(Sampler::pSourceBpm, (float)b);
        }
    }
    if (auto* d = dynamic_cast<DrumSampler*>(proc.get())) {
        if (ch.instrumentState.contains("builtin") && d->builtinName().isEmpty()) d->setBuiltin(ch.instrumentState["builtin"].toString());
        if (!ch.samplePath.isEmpty() && d->padPath(0).isEmpty()) { /* resolved by the engine */
        }
    }
    procKeys()[ch.id] = key;
    procPluginIds()[ch.id] = ch.pluginInstanceId;
    return replaceProcessor(ch.id, std::move(proc), sr, maxBlock, gen);
}

IProcessor* RuntimeRegistry::ensureEffect(
    const EffectSlot& slot, const Project& p, double sr, int maxBlock, const PluginFactory& pf, uint64_t gen) {
    QString key = keyForSlot(slot);
    auto it = m_procs.find(slot.id);
    if (it != m_procs.end() && procKeys()[slot.id] == key) return it->second.get();
    auto proc = createFor(key, p, slot.pluginInstanceId, pf);
    if (!proc) return nullptr;
    proc->restoreState(slot.state);
    procKeys()[slot.id] = key;
    procPluginIds()[slot.id] = slot.pluginInstanceId;
    return replaceProcessor(slot.id, std::move(proc), sr, maxBlock, gen);
}

void RuntimeRegistry::retireUnused(const Project& p, uint64_t gen) {
    std::set<Id> live;
    for (auto& c : p.channels) live.insert(c.id);
    for (auto& t : p.mixerTracks)
        for (auto& e : t.effects) live.insert(e.id);
    for (auto it = m_procs.begin(); it != m_procs.end();) {
        if (!live.count(it->first)) {
            m_graveyard.emplace_back(gen, std::move(it->second));
            procKeys().erase(it->first);
            it = m_procs.erase(it);
        } else ++it;
    }
}
void RuntimeRegistry::collectGarbage(uint64_t retiredGen, bool idle) {
    for (auto it = m_graveyard.begin(); it != m_graveyard.end();) {
        if (idle || it->first <= retiredGen + 1) it = m_graveyard.erase(it);
        else ++it;
    }
}
void RuntimeRegistry::clearAll() {
    m_procs.clear();
    m_graveyard.clear();
    m_tracks.clear();
    m_channels.clear();
    m_crash.clear();
    procKeys().clear();
    procPluginIds().clear();
}
void RuntimeRegistry::prepareAll(double sr, int maxBlock) {
    for (auto& [id, p] : m_procs) p->prepare(sr, maxBlock);
}
std::vector<std::pair<Id, IProcessor*>> RuntimeRegistry::pluginProcessors() const {
    std::vector<std::pair<Id, IProcessor*>> v;
    for (auto& [id, p] : m_procs)
        if (p->isPlugin()) v.push_back({procPluginIds()[id], p.get()});
    return v;
}

void RuntimeRegistry::syncStateToProject(Project& p) const {
    for (auto& c : p.channels)
        if (auto* proc = processor(c.id)) {
            if (proc->isPlugin()) {
                QJsonObject st = proc->saveState();
                if (auto* pd = p.plugin(c.pluginInstanceId)) {
                    pd->state = QByteArray::fromBase64(st["pluginState"].toString().toLatin1());
                    pd->controllerState = QByteArray::fromBase64(st["controllerState"].toString().toLatin1());
                    pd->latency = proc->latencySamples();
                }
            } else {
                c.instrumentState = proc->saveState();
                if (auto* s = dynamic_cast<Sampler*>(proc)) c.samplePath = s->samplePath();
            }
        }
    for (auto& t : p.mixerTracks)
        for (auto& e : t.effects)
            if (auto* proc = processor(e.id)) {
                if (proc->isPlugin()) {
                    QJsonObject st = proc->saveState();
                    if (auto* pd = p.plugin(e.pluginInstanceId)) {
                        pd->state = QByteArray::fromBase64(st["pluginState"].toString().toLatin1());
                        pd->controllerState = QByteArray::fromBase64(st["controllerState"].toString().toLatin1());
                        pd->latency = proc->latencySamples();
                    }
                } else e.state = proc->saveState();
            }
}

// ---------------------------------------------------------------- snapshot build
static RtParamTarget resolveTarget(const ParamTarget& t, RuntimeRegistry& reg) {
    RtParamTarget r;
    switch (t.kind) {
    case ParamTarget::ChannelVolume:
        r.direct = &reg.channel(t.ownerId).volume;
        r.min = 0;
        r.max = 1;
        break;
    case ParamTarget::ChannelPan:
        r.direct = &reg.channel(t.ownerId).pan;
        r.min = -1;
        r.max = 1;
        break;
    case ParamTarget::MixerVolume:
        r.direct = &reg.track(t.ownerId).volume;
        r.min = 0;
        r.max = 1.25f;
        break;
    case ParamTarget::MixerPan:
        r.direct = &reg.track(t.ownerId).pan;
        r.min = -1;
        r.max = 1;
        break;
    case ParamTarget::MixerSend: {
        auto& tc = reg.track(t.ownerId);
        auto& lv = tc.sendLevels[t.slotId];
        if (!lv) lv = std::make_unique<std::atomic<float>>(0.f);
        r.direct = lv.get();
        r.min = 0;
        r.max = 1;
        break;
    }
    case ParamTarget::EffectParam:
        r.proc = reg.processor(t.slotId);
        r.paramId = t.paramId;
        break;
    case ParamTarget::InstrumentParam:
        r.proc = reg.processor(t.ownerId);
        r.paramId = t.paramId;
        break;
    default: break;
    }
    if (r.proc && r.proc->params().indexOf(r.paramId) < 0) r.proc = nullptr;
    return r;
}

void AudioEngine::rebuild(const Project& p) {
    m_lastProject = &p;
    m_rebuildPending = false;
    auto snap = std::make_unique<Snapshot>();
    Snapshot& s = *snap;
    s.generation = m_nextGen++;
    s.maxBlock = m_maxBlock;
    s.tmpL.resize(m_maxBlock);
    s.tmpR.resize(m_maxBlock);
    const TimeSignature ts = p.transport.timeSig;
    // transport atomics mirror project settings
    m_bpm.store(p.transport.bpm);
    m_tsNum.store(ts.numerator);
    m_tsDen.store(ts.denominator);
    m_loopStart.store(p.transport.loopStart);
    m_loopEnd.store(p.transport.loopEnd);
    m_loopEnabled.store(p.transport.loopEnabled);
    m_loopRecord.store(p.transport.loopRecord);
    m_songMode.store(p.transport.songMode);
    m_metronome.store(p.transport.metronome);
    m_countIn.store(p.transport.countIn);
    m_countInBars.store(p.transport.countInBars);
    m_selectedPattern.store(p.selectedPatternId);
    setPunch(p.transport.punchEnabled, p.transport.punchIn, p.transport.punchOut);

    // ---- mixer tracks
    std::map<Id, int> trackIdx;
    for (size_t i = 0; i < p.mixerTracks.size(); ++i) trackIdx[p.mixerTracks[i].id] = (int)i;
    bool anyTrackSolo = false;
    for (auto& t : p.mixerTracks)
        if (t.solo) anyTrackSolo = true;
    std::set<Id> audibleTracks;
    if (anyTrackSolo) {
        std::function<void(Id)> mark = [&](Id id) {
            if (!id || audibleTracks.count(id)) return;
            audibleTracks.insert(id);
            if (auto* t = p.mixerTrack(id)) {
                mark(t->outputTrackId);
                for (auto& sd : t->sends) mark(sd.targetTrackId);
            }
        };
        for (auto& t : p.mixerTracks)
            if (t.solo || t.isMaster) mark(t.id);
    }
    for (auto& mt : p.mixerTracks) {
        RtTrack t;
        t.id = mt.id;
        t.ctl = &m_registry.track(mt.id);
        t.ctl->volume.store(mt.volume);
        t.ctl->pan.store(mt.pan);
        t.isMaster = mt.isMaster;
        t.inputChannel = mt.inputChannel;
        t.armed = mt.armed;
        t.outputIdx = mt.isMaster
            ? -1
            : (trackIdx.count(mt.outputTrackId) ? trackIdx[mt.outputTrackId] : (p.masterId() ? trackIdx[p.masterId()] : -1));
        t.audible = !mt.mute && (!anyTrackSolo || audibleTracks.count(mt.id));
        t.frozen = mt.freezeTrackId != 0;
        for (auto& e : mt.effects) {
            RtEffect fx;
            fx.proc = m_registry.ensureEffect(e, p, m_sr, m_maxBlock, m_pluginFactory, s.generation);   // kept alive while frozen
            if (t.frozen) continue;
            fx.bypass = e.bypass;
            fx.mix = e.mix;
            fx.slotId = e.id;
            if (e.pluginInstanceId) fx.crashed = &m_registry.crashFlag(e.pluginInstanceId);
            if (fx.proc || e.pluginInstanceId) t.effects.push_back(fx);
        }
        for (auto& sd : mt.sends) {
            if (!sd.enabled || !trackIdx.count(sd.targetTrackId) || sd.targetTrackId == mt.id) continue;
            RtSend rs;
            rs.targetIdx = trackIdx[sd.targetTrackId];
            auto& lv = t.ctl->sendLevels[sd.targetTrackId];
            if (!lv) lv = std::make_unique<std::atomic<float>>(sd.level);
            else lv->store(sd.level);
            rs.level = lv.get();
            rs.pre = sd.preFader;
            rs.sidechain = sd.sidechain;
            t.sends.push_back(rs);
        }
        t.recordChannels = mt.inputStereo ? 2 : 1;   // monitoring too, armed or not
        if (t.armed && t.inputChannel >= 0 && m_recording) t.recordRing = m_recording->ringFor(mt.id, t.recordChannels);
        t.inL.assign(m_maxBlock, 0.f);
        t.inR.assign(m_maxBlock, 0.f);
        t.scL.assign(m_maxBlock, 0.f);
        t.scR.assign(m_maxBlock, 0.f);
        t.genL.assign(m_maxBlock, 0.f);
        t.genR.assign(m_maxBlock, 0.f);
        for (auto& fx : t.effects)
            if (fx.proc && !fx.bypass && !(fx.crashed && fx.crashed->load())) t.chainLatency += fx.proc->latencySamples();
        s.tracks.push_back(std::move(t));
    }
    // ---- topological order (edges: track -> output, track -> send targets)
    {
        int n = (int)s.tracks.size();
        std::vector<int> indeg(n, 0);
        std::vector<std::vector<int>> adj(n);
        for (int i = 0; i < n; ++i) {
            auto& t = s.tracks[i];
            if (t.outputIdx >= 0) {
                adj[i].push_back(t.outputIdx);
                indeg[t.outputIdx]++;
            }
            for (auto& sd : t.sends) {
                adj[i].push_back(sd.targetIdx);
                indeg[sd.targetIdx]++;
            }
        }
        std::queue<int> q;
        for (int i = 0; i < n; ++i)
            if (indeg[i] == 0) q.push(i);
        while (!q.empty()) {
            int i = q.front();
            q.pop();
            s.trackOrder.push_back(i);
            for (int j : adj[i])
                if (--indeg[j] == 0) q.push(j);
        }
        if ((int)s.trackOrder.size() < n) {
            Log::warn("Mixer routing contains a cycle; falling back to linear order");
            s.trackOrder.clear();
            for (int i = n - 1; i >= 0; --i) s.trackOrder.push_back(i);
        }
    }
    // ---- latency compensation: D(t) = chain(t) + D(out(t)); generators into t are delayed by maxD - D(t)
    {
        for (auto it = s.trackOrder.rbegin(); it != s.trackOrder.rend(); ++it) {
            auto& t = s.tracks[*it];
            t.pathLatency = t.chainLatency + (t.outputIdx >= 0 ? s.tracks[t.outputIdx].pathLatency : 0);
        }
        int maxD = 0;
        for (auto& t : s.tracks) maxD = std::max(maxD, t.pathLatency);
        s.maxLatency = maxD;
        for (auto& t : s.tracks) {
            int want = maxD - t.pathLatency;
            if (t.ctl->generatorDelay.delay != want) t.ctl->generatorDelay.set(want);
            for (auto& sd : t.sends) {
                int outD = t.outputIdx >= 0 ? s.tracks[t.outputIdx].pathLatency : 0;
                int need = outD - s.tracks[sd.targetIdx].pathLatency;
                auto& dl = t.ctl->sendDelays[s.tracks[sd.targetIdx].id];
                if (need > 0) {
                    if (!dl) dl = std::make_unique<dsp::PdcDelay>();
                    if (dl->delay != need) dl->set(need);
                    sd.delay = dl.get();
                } else sd.delay = nullptr;
            }
        }
    }
    // ---- channels
    std::map<Id, int> chanIdx;
    bool anyChanSolo = false;
    for (auto& c : p.channels)
        if (c.solo) anyChanSolo = true;
    for (auto& c : p.channels) {
        RtChannel rc;
        rc.id = c.id;
        rc.instrument = m_registry.ensureInstrument(c, p, m_sr, m_maxBlock, m_pluginFactory, s.generation);
        if (auto* mt = p.mixerTrack(c.mixerTrackId); mt && mt->freezeTrackId) rc.instrument = nullptr;   // its insert plays a render
        rc.ctl = &m_registry.channel(c.id);
        rc.ctl->volume.store(c.volume);
        rc.ctl->pan.store(c.pan);
        rc.mixerTrackIdx = trackIdx.count(c.mixerTrackId) ? trackIdx[c.mixerTrackId] : 0;
        rc.enabled = c.enabled;
        rc.cutItself = c.cutItself;
        rc.audible = !c.mute && (!anyChanSolo || c.solo);
        rc.rootNote = c.rootNote;
        rc.isMidiOut = c.type == ChannelType::MidiOut;
        rc.midiOutPort = c.midiOutPort;
        for (Id t : c.auxOutputTracks) rc.auxTrackIdx.push_back(t && trackIdx.count(t) ? trackIdx[t] : -1);
        rc.bufL.assign(m_maxBlock, 0.f);
        rc.bufR.assign(m_maxBlock, 0.f);
        chanIdx[c.id] = (int)s.channels.size();
        s.channels.push_back(std::move(rc));
        // pending sample loads
        if (auto* smp = dynamic_cast<Sampler*>(s.channels.back().instrument)) {
            if (!smp->pendingPath.isEmpty()) {
                bool queued = false;
                for (auto& w : m_waitingSamples)
                    if (w.first == smp) queued = true;
                if (!queued) m_waitingSamples.push_back({smp, smp->pendingPath});
                m_cache.get(smp->pendingPath);
            }
        }
        if (auto* ds = dynamic_cast<DrumSampler*>(s.channels.back().instrument)) {
            for (int i = 0; i < DrumSampler::kPads; ++i) {
                QString path = ds->padPath(i);
                if (path.isEmpty() || path.startsWith("builtin:") || ds->padSample(i)) continue;
                bool queued = false;
                for (auto& w : m_waitingSamples)
                    if (w.first == ds && w.second == path) queued = true;
                if (!queued) m_waitingSamples.push_back({ds, path});
                m_cache.get(path);
            }
            if (!c.samplePath.isEmpty() && ds->padPath(0).isEmpty()) {
                m_waitingSamples.push_back({ds, c.samplePath});
                m_cache.get(c.samplePath);
            }
        }
    }
    resolvePendingSamples();
    s.liveChannelIdx = chanIdx.count(p.selectedChannelId) ? chanIdx[p.selectedChannelId] : (s.channels.empty() ? -1 : 0);
    m_registry.retireUnused(p, s.generation);
    // ---- automation lanes
    std::map<Id, int> laneIdx;
    for (auto& l : p.automationLanes) {
        RtAutomationLane rl;
        rl.id = l.id;
        rl.target = resolveTarget(l.target, m_registry);
        rl.points = l.points;
        rl.length = l.length;
        laneIdx[l.id] = (int)s.lanes.size();
        s.lanes.push_back(std::move(rl));
    }
    // ---- patterns
    std::map<Id, int> patIdx;
    for (auto& pat : p.patterns) {
        RtPattern rp;
        rp.id = pat.id;
        rp.length = pat.effectiveLength(ts);
        rp.swing = pat.swing;
        for (auto& pc : pat.channels) {
            if (!chanIdx.count(pc.id)) continue;
            const Channel* ch = p.channel(pc.id);
            RtPatternChannel rc;
            rc.channelIndex = chanIdx[pc.id];
            for (auto& n : pc.notes)
                if (!n.muted)
                    rc.notes.push_back(
                        {n.pitch, n.start, n.start + std::max<Tick>(n.duration, 1), n.velocity, n.pan, n.finePitch, 1.f, n.channel});
            Tick st = pat.stepTicks();
            for (size_t i = 0; i < pc.steps.size() && (int)i < pat.stepCount; ++i) {
                const Step& sp = pc.steps[i];
                if (!sp.enabled) continue;
                Tick start = (Tick)i * st + (Tick)(sp.offset * st) + ((i % 2) ? (Tick)(pat.swing * st * 0.5f) : 0);
                rc.notes.push_back(
                    {(int)std::lround(ch->rootNote + sp.pitch), start, start + st, sp.velocity, sp.pan, 0.f, sp.probability, 0});
            }
            std::sort(rc.notes.begin(), rc.notes.end(), [](const RtNote& a, const RtNote& b) { return a.start < b.start; });
            for (auto& cp : pc.controls) {
                if (cp.controller < 0 || cp.controller > kCtrlPitchBend) continue;
                MidiEvent e;
                e.type = cp.controller == kCtrlPitchBend ? MidiEvent::PitchBend
                    : cp.controller == kCtrlPressure     ? MidiEvent::ChannelPressure
                                                         : MidiEvent::CC;
                e.data1 = (uint8_t)clamp(cp.controller, 0, 127);
                e.data2 = (uint8_t)clamp(cp.value, 0, 127);
                e.pitchBend = clamp(cp.value, 0, 16383) - 8192;
                rc.controls.push_back({cp.tick, cp.controller, e});
            }
            rp.channels.push_back(std::move(rc));
        }
        for (auto& l : p.automationLanes)
            if (l.patternId == pat.id && laneIdx.count(l.id)) rp.laneIndices.push_back(laneIdx[l.id]);
        for (Id lid : pat.automationLaneIds)
            if (laneIdx.count(lid) && std::find(rp.laneIndices.begin(), rp.laneIndices.end(), laneIdx[lid]) == rp.laneIndices.end())
                rp.laneIndices.push_back(laneIdx[lid]);
        patIdx[pat.id] = (int)s.patterns.size();
        s.patterns.push_back(std::move(rp));
    }
    s.selectedPatternIdx = patIdx.count(p.selectedPatternId) ? patIdx[p.selectedPatternId] : (s.patterns.empty() ? -1 : 0);
    // ---- playlist clips
    bool anyPlSolo = false;
    for (auto& t : p.playlistTracks)
        if (t.solo) anyPlSolo = true;
    for (auto& c : p.clips) {
        const PlaylistTrack* pt = p.track(c.trackId);
        if (!pt) continue;
        if (auto* mt = p.mixerTrack(pt->mixerTrackId); mt && mt->freezeTrackId && pt->id != mt->freezeTrackId && c.type == ClipType::Audio)
            continue;   // in the frozen insert's render already
        RtClip rc;
        rc.type = c.type;
        rc.start = c.start;
        rc.end = c.start + p.clipLength(c);
        rc.sourceOffset = c.sourceOffset;
        rc.loopSource = c.loopSource;
        rc.muted = c.muted || pt->mute || (anyPlSolo && !pt->solo) || (c.type == ClipType::Pattern && !p.transport.songMode && false);
        rc.trackIdx = trackIdx.count(pt->mixerTrackId) ? trackIdx[pt->mixerTrackId] : (p.masterId() ? trackIdx[p.masterId()] : 0);
        if (c.type == ClipType::Pattern) rc.patternIdx = patIdx.count(c.sourceId) ? patIdx[c.sourceId] : -1;
        else if (c.type == ClipType::Automation) rc.laneIdx = laneIdx.count(c.sourceId) ? laneIdx[c.sourceId] : -1;
        else if (c.type == ClipType::Audio) {
            const AudioSource* src = p.audioSource(c.sourceId);
            if (!src || src->filePath.isEmpty()) continue;
            double ratio = clipTimeRatio(c, p.transport.bpm);
            bool stretched = c.stretchMode != StretchResample && (std::fabs(ratio - 1.0) > 1e-4 || std::fabs(c.pitch) > 1e-3);
            AudioAssetPtr a = stretched ? m_cache.getStretched(src->filePath, ratio, c.pitch, c.stretchMode) : m_cache.get(src->filePath);
            if (!a && stretched) m_cache.get(src->filePath);
            rc.asset = a;
            if (a) {
                double scale = stretched ? ratio : 1.0;
                rc.srcStart = (int64_t)(src->trimStart * scale);
                rc.srcEnd = src->trimEnd > 0 ? (int64_t)(src->trimEnd * scale) : a->frames;
                rc.srcStart = clamp<int64_t>(rc.srcStart, 0, a->frames);
                rc.srcEnd = clamp<int64_t>(rc.srcEnd, rc.srcStart + 1, a->frames);
                rc.gain = c.gain * src->gain * (src->normalize && a->peakValue > 0 ? 1.f / a->peakValue : 1.f);
                rc.reverse = c.reverse != src->reverse;
                rc.rate = stretched ? 1.0 : clipSourceSpeed(c, p.transport.bpm);   // resample: pitch, stretch and tempo fit set the speed
                if (c.fitTempo && c.sourceBpm > 0) rc.framesPerTick = rc.rate * m_sr * 60.0 / (p.transport.bpm * PPQ);
                rc.fadeIn = c.fadeIn;
                rc.fadeOut = c.fadeOut;
                rc.fadeInShape = c.fadeInShape;
                rc.fadeOutShape = c.fadeOutShape;
                s.heldAssets.push_back(a);
            }
        }
        s.clips.push_back(std::move(rc));
    }
    s.clipStates.resize(s.clips.size());
    s.songEnd = p.songLength();
    s.tempo = p.transport.tempoMap();
    m_tempoUi = s.tempo;
    // ---- MIDI mappings
    for (auto& m : p.midiMappings) {
        RtMidiMapping rm;
        rm.cc = (uint8_t)clamp(m.cc, 0, 127);
        rm.midiChannel = (int8_t)m.midiChannel;
        rm.target = resolveTarget(m.target, m_registry);
        if (rm.target.valid()) s.midiMappings.push_back(rm);
    }
    // ---- hand over
    EngineCommand cmd;
    cmd.type = EngineCommand::SetSnapshot;
    cmd.snapshot = snap.get();
    m_inFlight.push_back(std::move(snap));
    if (!m_commands.push(cmd)) {
        Log::warn("Engine command queue full; retrying rebuild");
        m_rebuildPending = true;
    }
    if (!m_devices.isOpen() && !m_offline) drainCommands();   // no RT thread: adopt immediately
}

}   // namespace mt
