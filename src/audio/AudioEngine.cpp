#include "audio/AudioEngine.h"
#include "audio/RecordingEngine.h"
#include "core/Log.h"
#include <csetjmp>
#include <csignal>
#include <chrono>
#include <thread>

namespace mt {

// ---------------------------------------------------------------- crash guard
// Third-party plugin code runs inside a sigsetjmp/siglongjmp guard so a segfault inside a plugin marks the plugin
// as crashed and bypasses it instead of taking the whole DAW down. This is best-effort isolation: the process keeps
// running with the project intact so the user can save and recover.
static thread_local sigjmp_buf* t_guard = nullptr;
static struct sigaction s_oldSegv, s_oldFpe, s_oldIll, s_oldBus;
static void crashHandler(int sig, siginfo_t*, void*) {
    if (t_guard) siglongjmp(*t_guard, sig);
    struct sigaction* old = sig == SIGSEGV ? &s_oldSegv : sig == SIGFPE ? &s_oldFpe : sig == SIGILL ? &s_oldIll : &s_oldBus;
    sigaction(sig, old, nullptr); raise(sig);
}
void installCrashGuard() {
    static bool done = false; if (done) return; done = true;
    struct sigaction sa{}; sa.sa_sigaction = crashHandler; sa.sa_flags = SA_SIGINFO | SA_NODEFER; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &s_oldSegv); sigaction(SIGFPE, &sa, &s_oldFpe); sigaction(SIGILL, &sa, &s_oldIll); sigaction(SIGBUS, &sa, &s_oldBus);
}

float RtAutomationLane::valueAt(Tick t) const { return evalAutomation(points, t); }

// ---------------------------------------------------------------- engine
AudioEngine::AudioEngine(AudioDeviceManager& devices, SampleCache& cache, QObject* parent) : QObject(parent), m_devices(devices), m_cache(cache) {
    installCrashGuard();
    m_outL.resize(8192); m_outR.resize(8192); m_dryL.resize(8192); m_dryR.resize(8192);
    connect(&m_cache, &SampleCache::assetLoaded, this, &AudioEngine::onAssetLoaded);
    connect(&m_pollTimer, &QTimer::timeout, this, &AudioEngine::poll);
    m_pollTimer.start(16);
}
AudioEngine::~AudioEngine() { stopDevice(); m_rtSnapshot = nullptr; /* owned by m_inFlight */ }

bool AudioEngine::startDevice(QString* error) {
    const auto& cfg = m_devices.config();
    bool ok = m_devices.open(cfg, [this](const float* in, float* out, int frames, int inCh, int outCh, bool xrun) { rtCallback(in, out, frames, inCh, outCh, xrun); }, error);
    if (!ok) return false;
    double sr = m_devices.actualSampleRate(); int block = cfg.bufferSize;
    if (sr != m_sr || block > m_maxBlock) { m_sr = sr; m_maxBlock = std::max(block, 64); m_cache.setSampleRate(sr); m_registry.prepareAll(m_sr, m_maxBlock); m_rebuildPending = true; }
    return true;
}
void AudioEngine::stopDevice() { m_devices.close(); }

// ---------------------------------------------------------------- transport API
void AudioEngine::play() { EngineCommand c; c.type = EngineCommand::Play; m_commands.push(c); }
void AudioEngine::stop() { EngineCommand c; c.type = EngineCommand::Stop; m_commands.push(c); }
void AudioEngine::pause() { EngineCommand c; c.type = EngineCommand::Pause; m_commands.push(c); }
void AudioEngine::togglePlay() { if (m_playing.load()) stop(); else play(); }
void AudioEngine::seek(Tick t) { EngineCommand c; c.type = EngineCommand::Seek; c.value = (double)std::max<Tick>(0, t); m_commands.push(c); }
void AudioEngine::setRecording(bool r) { EngineCommand c; c.type = EngineCommand::SetRecording; c.ival = r ? 1 : 0; m_commands.push(c); }
void AudioEngine::allNotesOff() { EngineCommand c; c.type = EngineCommand::AllNotesOff; m_commands.push(c); }
void AudioEngine::previewPlay(std::shared_ptr<PreviewBuffer> b) { if (!b) return; m_preview.set(b); EngineCommand c; c.type = EngineCommand::PreviewPlay; m_commands.push(c); }
void AudioEngine::previewStop() { EngineCommand c; c.type = EngineCommand::PreviewStop; m_commands.push(c); }
void AudioEngine::previewSeek(double frame) { EngineCommand c; c.type = EngineCommand::PreviewPlay; c.value = frame; m_commands.push(c); }

TransportView AudioEngine::transport() const {
    TransportView v; v.playing = m_playing.load(); v.recording = m_recordingFlag.load(); v.paused = m_paused.load(); v.countingIn = m_countingIn.load(); v.positionTicks = m_posTicks.load(); v.bpm = m_bpm.load(); v.tsNum = m_tsNum.load(); v.tsDen = m_tsDen.load(); v.songMode = m_songMode.load(); v.loop = m_loopEnabled.load(); v.loopStart = m_loopStart.load(); v.loopEnd = m_loopEnd.load();
    TimeSignature ts{v.tsNum, v.tsDen}; Tick pos = (Tick)v.positionTicks; Tick bar = ts.ticksPerBar(), beat = ts.ticksPerBeat(); v.bar = (int)(pos / bar) + 1; v.beat = (int)((pos % bar) / beat) + 1; v.tick = (int)(pos % beat);
    return v;
}

void AudioEngine::offlineBegin(double sampleRate, int blockSize) {
    stopDevice(); m_offline = true; m_savedSr = m_sr; m_savedBlock = m_maxBlock; m_sr = sampleRate; m_maxBlock = std::max(blockSize, 64);
    m_cache.setSampleRate(sampleRate); m_registry.prepareAll(m_sr, m_maxBlock);
    if (m_lastProject) rebuild(*m_lastProject);
    drainCommands();
}
void AudioEngine::offlineProcess(float* outL, float* outR, int n) { processFrames(nullptr, 0, outL, outR, n); }
void AudioEngine::offlineEnd() {
    m_offline = false; drainCommands();
    m_sr = m_savedSr; m_maxBlock = m_savedBlock; m_cache.setSampleRate(m_sr); m_registry.prepareAll(m_sr, m_maxBlock);
    if (m_lastProject) rebuild(*m_lastProject);
    QString err; startDevice(&err);
}

void AudioEngine::unloadProject() {
    // Send an empty snapshot and wait until the RT thread has adopted it, then free everything.
    m_waitingSamples.clear();
    auto empty = std::make_unique<Snapshot>(); empty->generation = m_nextGen++; empty->maxBlock = m_maxBlock; empty->tmpL.resize(m_maxBlock); empty->tmpR.resize(m_maxBlock);
    uint64_t gen = empty->generation; EngineCommand c; c.type = EngineCommand::SetSnapshot; c.snapshot = empty.get(); m_inFlight.push_back(std::move(empty)); m_commands.push(c);
    if (m_devices.isOpen()) { for (int i = 0; i < 200 && m_activeGen.load() < gen; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    else { drainCommands(); }
    poll();
    m_registry.clearAll(); m_lastProject = nullptr;
}

// ---------------------------------------------------------------- UI pump
void AudioEngine::poll() {
    EngineMessage m; uint64_t retired = 0; bool anyRetired = false;
    while (m_messages.pop(m)) {
        switch (m.type) {
        case EngineMessage::SnapshotRetired: { retired = std::max(retired, m.snapshot->generation); anyRetired = true; for (size_t i = 0; i < m_inFlight.size(); ++i) if (m_inFlight[i].get() == m.snapshot) { m_inFlight.erase(m_inFlight.begin() + i); break; } break; }
        case EngineMessage::RecordedMidi: Q_EMIT recordedMidi(m.midi, m.tick); break;
        case EngineMessage::PluginCrashed: Q_EMIT pluginCrashed(m.id); break;
        case EngineMessage::LoopWrapped: Q_EMIT loopWrapped(); break;
        case EngineMessage::Xrun: Q_EMIT xrunOccurred(); break;
        case EngineMessage::RecordStarted: Q_EMIT recordStarted(m.tick); break;
        default: break;
        }
    }
    bool idle = !m_devices.isOpen() && !m_offline;
    if (anyRetired || idle) m_registry.collectGarbage(idle ? m_activeGen.load() + 2 : retired, idle);
    for (auto& [id, p] : m_registry.processors()) if (auto* ib = dynamic_cast<InstrumentBase*>(p.get())) ib->collectGarbage(idle);
    m_preview.prune(idle);
    if (m_rebuildPending && m_lastProject) { m_rebuildPending = false; rebuild(*m_lastProject); }
    static bool wasPlaying = false; bool nowPlaying = m_playing.load(); if (nowPlaying != wasPlaying) { wasPlaying = nowPlaying; Q_EMIT transportStateChanged(); }
}

void AudioEngine::onAssetLoaded(const QString&) { resolvePendingSamples(); if (m_lastProject) m_rebuildPending = true; }
void AudioEngine::resolvePendingSamples() {
    for (auto it = m_waitingSamples.begin(); it != m_waitingSamples.end();) {
        auto* proc = it->first; const QString& path = it->second; bool done = false;
        auto asset = m_cache.get(path);
        if (asset) {
            if (auto* s = dynamic_cast<Sampler*>(proc)) { if (s->pendingPath == path) { s->setSample(asset, path); s->pendingPath.clear(); } done = true; }
            else if (auto* d = dynamic_cast<DrumSampler*>(proc)) { for (int i = 0; i < DrumSampler::kPads; ++i) if (d->padPath(i) == path && !d->padSample(i)) d->setPadSample(i, asset, path); done = true; }
            else done = true;
        }
        if (done) it = m_waitingSamples.erase(it); else ++it;
    }
}

// ---------------------------------------------------------------- RT: commands
void AudioEngine::drainCommands() {
    EngineCommand c;
    while (m_commands.pop(c)) {
        switch (c.type) {
        case EngineCommand::SetSnapshot: {
            Snapshot* old = m_rtSnapshot; m_rtSnapshot = c.snapshot; m_activeGen.store(c.snapshot->generation);
            m_patternLength.store(c.snapshot->selectedPatternIdx >= 0 ? c.snapshot->patterns[c.snapshot->selectedPatternIdx].length : PPQ * 4);
            m_maxLatency.store(c.snapshot->maxLatency);
            if (old) { EngineMessage m; m.type = EngineMessage::SnapshotRetired; m.snapshot = old; m_messages.push(m); }
            if (old) {
                for (size_t i = 0; i < c.snapshot->clipStates.size() && i < old->clipStates.size(); ++i) c.snapshot->clipStates[i] = old->clipStates[i]; // keep audio clip playheads continuous
                // carry pending note-offs over so notes sounding across a rebuild are still released (no allocation: fixed arrays)
                for (auto& nc : c.snapshot->channels) for (auto& oc : old->channels) if (oc.id == nc.id) { if (nc.instrument == oc.instrument) nc.noteOffs = oc.noteOffs; else if (oc.instrument) { MidiEvent e; e.type = MidiEvent::AllNotesOff; nc.midi.add(e); } break; }
            }
            break; }
        case EngineCommand::Play: {
            if (m_playing.load()) break;
            m_previewPlaying.store(false);
            m_playing.store(true); m_paused.store(false); m_transportJumped = true;
            if (m_countIn.load() && m_recordingFlag.load()) { TimeSignature ts{m_tsNum.load(), m_tsDen.load()}; m_countInRemaining = (double)ts.ticksPerBar() * m_countInBars.load(); m_countingIn.store(true); }
            else { m_countInRemaining = 0; m_countingIn.store(false); if (m_recordingFlag.load()) { EngineMessage m; m.type = EngineMessage::RecordStarted; m.tick = m_posTicks.load(); m_messages.push(m); } }
            break; }
        case EngineCommand::Stop: {
            bool was = m_playing.load(); m_playing.store(false); m_paused.store(false); m_countingIn.store(false); m_recordingFlag.store(false);
            if (was) { if (m_songMode.load()) m_posTicks.store((double)(m_loopEnabled.load() ? m_loopStart.load() : 0)); else m_posTicks.store(0.0); }
            if (m_rtSnapshot) for (auto& ch : m_rtSnapshot->channels) { ch.noteOffs.clear(); MidiEvent e; e.type = MidiEvent::AllNotesOff; ch.midi.add(e); }
            m_transportJumped = true; break; }
        case EngineCommand::Pause: { m_playing.store(false); m_paused.store(true); m_countingIn.store(false); if (m_rtSnapshot) for (auto& ch : m_rtSnapshot->channels) { ch.noteOffs.clear(); MidiEvent e; e.type = MidiEvent::AllNotesOff; ch.midi.add(e); } break; }
        case EngineCommand::Seek: m_posTicks.store(c.value); m_transportJumped = true; if (m_rtSnapshot) for (auto& ch : m_rtSnapshot->channels) { ch.noteOffs.clear(); MidiEvent e; e.type = MidiEvent::AllNotesOff; ch.midi.add(e); } break;
        case EngineCommand::SetRecording: m_recordingFlag.store(c.ival != 0); break;
        case EngineCommand::AllNotesOff: case EngineCommand::Panic: if (m_rtSnapshot) for (auto& ch : m_rtSnapshot->channels) { ch.noteOffs.clear(); MidiEvent e; e.type = MidiEvent::AllNotesOff; ch.midi.add(e); if (ch.instrument) ch.instrument->reset(); } break;
        case EngineCommand::PreviewPlay: m_previewPosRt = c.value; m_previewPos.store(c.value); m_previewPlaying.store(true); break;
        case EngineCommand::PreviewStop: m_previewPlaying.store(false); break;
        default: break;
        }
    }
}

// ---------------------------------------------------------------- RT: callback
void AudioEngine::rtCallback(const float* in, float* out, int frames, int inCh, int outCh, bool xrun) {
    auto t0 = std::chrono::steady_clock::now();
    if (xrun) { EngineMessage m; m.type = EngineMessage::Xrun; m_messages.push(m); }
    if (frames > (int)m_outL.size()) frames = (int)m_outL.size();
    processFrames(in, inCh, m_outL.data(), m_outR.data(), frames);
    for (int i = 0; i < frames; ++i) { float l = clamp(m_outL[i], -1.f, 1.f), r = clamp(m_outR[i], -1.f, 1.f); for (int c = 0; c < outCh; ++c) out[i * outCh + c] = (c % 2 == 0) ? l : r; }
    auto t1 = std::chrono::steady_clock::now();
    double used = std::chrono::duration<double>(t1 - t0).count(); double avail = frames / m_sr;
    float load = (float)(used / avail); m_cpuLoad.store(m_cpuLoad.load() * 0.8f + load * 0.2f);
}

void AudioEngine::processFrames(const float* in, int inCh, float* outL, float* outR, int frames) {
    drainCommands();
    std::fill(outL, outL + frames, 0.f); std::fill(outR, outR + frames, 0.f);
    Snapshot* snap = m_rtSnapshot;
    int offset = 0; int remaining = frames;
    while (remaining > 0) {
        int n = std::min(remaining, snap ? snap->maxBlock : m_maxBlock);
        double spt = samplesPerTick(); double pos = m_posTicks.load(); Tick t0 = (Tick)std::floor(pos); Tick t1 = t0;
        bool playing = m_playing.load(); bool countIn = m_countingIn.load();
        Tick loopS = 0, loopE = 0; bool loop = false;
        if (playing && !countIn) {
            if (m_songMode.load()) { loop = m_loopEnabled.load(); loopS = m_loopStart.load(); loopE = m_loopEnd.load(); }
            else { loop = true; loopS = 0; loopE = std::max<Tick>(m_patternLength.load(), PPQ); }
            if (loop && loopE > loopS) {
                if (pos >= loopE || pos < loopS) { pos = (double)loopS; m_posTicks.store(pos); t0 = loopS; m_transportJumped = true; }
                double ticksToEnd = loopE - pos; int samplesToEnd = (int)std::ceil(ticksToEnd * spt);
                if (samplesToEnd < n) n = std::max(1, samplesToEnd);
            }
            double endPos = pos + n / spt; t1 = (Tick)std::floor(endPos); if (t1 <= t0) t1 = t0 + 1;
        } else if (playing && countIn) {
            double dt = n / spt; m_countInRemaining -= dt;
            if (m_countInRemaining <= 0) { m_countingIn.store(false); EngineMessage m; m.type = EngineMessage::RecordStarted; m.tick = m_posTicks.load(); m_messages.push(m); }
        }
        processSubBlock(in ? in + (size_t)offset * inCh : nullptr, inCh, outL + offset, outR + offset, n, snap, t0, t1);
        if (playing && countIn) { TimeSignature ts{m_tsNum.load(), m_tsDen.load()}; Tick total = ts.ticksPerBar() * m_countInBars.load(); Tick c0 = total - (Tick)(m_countInRemaining + n / spt), c1 = total - (Tick)m_countInRemaining; renderMetronome(outL + offset, outR + offset, n, c0, c1, true); }
        else if (playing && m_metronome.load()) renderMetronome(outL + offset, outR + offset, n, t0, t1, false);
        if (m_midiClock.load()) {
            // MIDI clock: 24 pulses per quarter note while the transport runs
            bool run = playing && !countIn;
            if (run && !m_clockRunning) { TimedMidiOut t; t.event.type = MidiEvent::Start; t.port = -1; t.sampleTime = m_samplesProcessed.load(); m_midiOut.push(t); m_clockRunning = true; m_clockAccum = 0; }
            if (!run && m_clockRunning) { TimedMidiOut t; t.event.type = MidiEvent::Stop; t.port = -1; t.sampleTime = m_samplesProcessed.load(); m_midiOut.push(t); m_clockRunning = false; }
            if (run) { double pulseSamples = spt * PPQ / 24.0; m_clockAccum += n; while (m_clockAccum >= pulseSamples) { m_clockAccum -= pulseSamples; TimedMidiOut t; t.event.type = MidiEvent::Clock; t.port = -1; t.sampleTime = m_samplesProcessed.load() + offset + n - (int64_t)m_clockAccum; m_midiOut.push(t); } }
        }
        if (playing && !countIn) {
            double newPos = pos + n / spt;
            if (loop && loopE > loopS && newPos >= loopE - 1e-6) { newPos = (double)loopS; m_transportJumped = true; EngineMessage m; m.type = EngineMessage::LoopWrapped; m_messages.push(m); }
            else m_transportJumped = false;
            m_posTicks.store(newPos);
        }
        offset += n; remaining -= n;
    }
    renderPreview(outL, outR, frames);
    m_samplesProcessed.fetch_add(frames);
}

// Stable sort by sample offset; at the same offset note-offs go first so a note ending exactly where the next one (same pitch) starts
// doesn't cut the new note (back-to-back 808s, legato step sequences)
static inline int midiRank(const MidiEvent& e) { return (e.type == MidiEvent::NoteOff || e.type == MidiEvent::AllNotesOff) ? 0 : 1; }
static inline bool midiBefore(const MidiEvent& a, const MidiEvent& b) { return a.sampleOffset < b.sampleOffset || (a.sampleOffset == b.sampleOffset && midiRank(a) < midiRank(b)); }
static inline void sortMidi(MidiEventList& l) { for (int i = 1; i < l.count; ++i) { MidiEvent k = l.events[i]; int j = i - 1; while (j >= 0 && midiBefore(k, l.events[j])) { l.events[j + 1] = l.events[j]; --j; } l.events[j + 1] = k; } }

void AudioEngine::processSubBlock(const float* in, int inCh, float* outL, float* outR, int n, Snapshot* snap, Tick t0, Tick t1) {
    if (!snap) return;
    Snapshot& s = *snap;
    ProcessContext ctx; ctx.sampleRate = m_sr; ctx.blockSize = n; ctx.bpm = m_bpm.load(); ctx.tsNum = m_tsNum.load(); ctx.tsDen = m_tsDen.load(); ctx.playing = m_playing.load() && !m_countingIn.load(); ctx.recording = m_recordingFlag.load();
    ctx.positionBeats = (double)t0 / PPQ; ctx.positionSamples = (int64_t)(ctx.positionBeats * 60.0 / ctx.bpm * m_sr); ctx.looping = m_loopEnabled.load(); ctx.loopStartBeats = (double)m_loopStart.load() / PPQ; ctx.loopEndBeats = (double)m_loopEnd.load() / PPQ;
    for (auto& t : s.tracks) { std::fill(t.inL.begin(), t.inL.begin() + n, 0.f); std::fill(t.inR.begin(), t.inR.begin() + n, 0.f); std::fill(t.scL.begin(), t.scL.begin() + n, 0.f); std::fill(t.scR.begin(), t.scR.begin() + n, 0.f); std::fill(t.genL.begin(), t.genL.begin() + n, 0.f); std::fill(t.genR.begin(), t.genR.begin() + n, 0.f); }
    for (auto& c : s.channels) { std::fill(c.bufL.begin(), c.bufL.begin() + n, 0.f); std::fill(c.bufR.begin(), c.bufR.begin() + n, 0.f); }
    // Live MIDI (from hardware and from the UI keyboard)
    routeLiveMidi(s, t0);
    // Sequencer
    if (ctx.playing) runSequencer(s, t0, t1, n);
    // Pending note-offs from note durations
    for (auto& c : s.channels) { c.noteOffs.advance(n, c.midi); sortMidi(c.midi); }
    // Instruments -> channel buffers -> mixer track inputs
    for (auto& c : s.channels) {
        if (!c.instrument) { c.midi.clear(); continue; }
        c.instrument->process(c.bufL.data(), c.bufR.data(), n, c.midi, ctx);
        if (c.isMidiOut) { if (auto* om = c.instrument->outputMidi()) for (int i = 0; i < om->count; ++i) { TimedMidiOut t; t.event = om->events[i]; t.port = c.midiOutPort; t.sampleTime = m_samplesProcessed.load() + om->events[i].sampleOffset; m_midiOut.push(t); } }
        c.midi.clear();
        if (!c.audible || !c.enabled) continue;
        float vol = c.ctl->volume.load() / 0.8f, pan = c.ctl->pan.load(); float a = (pan + 1.f) * 0.25f * dsp::kPi; float gl = std::cos(a) * 1.4142f * vol, gr = std::sin(a) * 1.4142f * vol;
        float pk = 0, pr = 0;
        for (int i = 0; i < n; ++i) { c.bufL[i] *= gl; c.bufR[i] *= gr; pk = std::max(pk, std::fabs(c.bufL[i])); pr = std::max(pr, std::fabs(c.bufR[i])); }
        c.ctl->meter.peakL.store(pk); c.ctl->meter.peakR.store(pr);
        if (c.mixerTrackIdx >= 0 && c.mixerTrackIdx < (int)s.tracks.size()) { auto& t = s.tracks[c.mixerTrackIdx]; for (int i = 0; i < n; ++i) { t.genL[i] += c.bufL[i]; t.genR[i] += c.bufR[i]; } }
    }
    // Audio clips
    if (ctx.playing && m_songMode.load()) renderAudioClips(s, t0, t1, n);   // playlist clips belong to song mode; pattern mode plays the pattern only
    // Hardware inputs -> tracks (monitoring + recording)
    if (in && inCh > 0) {
        bool rec = ctx.recording && ctx.playing; bool monitor = m_monitorInputs.load();
        if (rec && m_punch.load()) { Tick pi = m_punchIn.load(), po = m_punchOut.load(); if (t0 < pi || t0 >= po) rec = false; }
        for (auto& t : s.tracks) {
            if (t.inputChannel < 0 || t.inputChannel >= inCh) continue;
            int chL = t.inputChannel, chR = (t.recordChannels == 2 && t.inputChannel + 1 < inCh) ? t.inputChannel + 1 : t.inputChannel;
            if (monitor || (rec && t.armed)) for (int i = 0; i < n; ++i) { t.genL[i] += in[i * inCh + chL]; t.genR[i] += in[i * inCh + chR]; }
            if (rec && t.armed && t.recordRing) { for (int i = 0; i < n; ++i) { float f[2] = {in[i * inCh + chL], in[i * inCh + chR]}; t.recordRing->write(f, t.recordChannels); } }
        }
    }
    // Mixer graph in topological order
    for (int idx : s.trackOrder) processTrack(s, s.tracks[idx], n, ctx, outL, outR);
}

void AudioEngine::processTrack(Snapshot& s, RtTrack& t, int n, const ProcessContext& ctxIn, float* outL, float* outR) {
    ProcessContext ctx = ctxIn; ctx.sidechainL = t.scL.data(); ctx.sidechainR = t.scR.data();
    float* L = t.inL.data(); float* R = t.inR.data();
    // generator inputs (instruments, audio clips, hardware inputs) pass through the delay-compensation line first
    t.ctl->generatorDelay.process(t.genL.data(), t.genR.data(), n);
    for (int i = 0; i < n; ++i) { L[i] += t.genL[i]; R[i] += t.genR[i]; }
    if (!t.audible) { std::fill(L, L + n, 0.f); std::fill(R, R + n, 0.f); }
    else {
        for (auto& fx : t.effects) {
            if (fx.bypass || !fx.proc || (fx.crashed && fx.crashed->load())) continue;
            if (fx.mix < 0.999f) { std::copy(L, L + n, m_dryL.data()); std::copy(R, R + n, m_dryR.data()); }
            static MidiEventList empty;
            if (!guardedProcess(fx, L, R, n, empty, ctx)) continue;
            if (fx.mix < 0.999f) for (int i = 0; i < n; ++i) { L[i] = m_dryL[i] + (L[i] - m_dryL[i]) * fx.mix; R[i] = m_dryR[i] + (R[i] - m_dryR[i]) * fx.mix; }
        }
    }
    // pre-fader sends
    for (auto& sd : t.sends) if (sd.pre && sd.targetIdx >= 0) { float lv = sd.level->load(); if (lv <= 0) continue; auto& dst = s.tracks[sd.targetIdx]; float* dl = sd.sidechain ? dst.scL.data() : dst.inL.data(); float* dr = sd.sidechain ? dst.scR.data() : dst.inR.data(); if (sd.delay) { std::copy(L, L + n, s.tmpL.data()); std::copy(R, R + n, s.tmpR.data()); sd.delay->process(s.tmpL.data(), s.tmpR.data(), n); for (int i = 0; i < n; ++i) { dl[i] += s.tmpL[i] * lv; dr[i] += s.tmpR[i] * lv; } } else for (int i = 0; i < n; ++i) { dl[i] += L[i] * lv; dr[i] += R[i] * lv; } }
    // volume / pan with per-block linear ramp
    float vol = t.ctl->volume.load() / 0.8f, pan = t.ctl->pan.load(); float a = (pan + 1.f) * 0.25f * dsp::kPi; float gl = std::cos(a) * 1.4142f * vol, gr = std::sin(a) * 1.4142f * vol;
    float pk = 0, pr = 0, sql = 0, sqr = 0;
    for (int i = 0; i < n; ++i) { L[i] *= gl; R[i] *= gr; float al = std::fabs(L[i]), ar = std::fabs(R[i]); pk = std::max(pk, al); pr = std::max(pr, ar); sql += L[i] * L[i]; sqr += R[i] * R[i]; }
    t.ctl->meter.peakL.store(pk); t.ctl->meter.peakR.store(pr); t.ctl->meter.rmsL.store(std::sqrt(sql / n)); t.ctl->meter.rmsR.store(std::sqrt(sqr / n)); if (pk >= 1.f || pr >= 1.f) t.ctl->meter.clipped.store(true);
    // post-fader sends
    for (auto& sd : t.sends) if (!sd.pre && sd.targetIdx >= 0) { float lv = sd.level->load(); if (lv <= 0) continue; auto& dst = s.tracks[sd.targetIdx]; float* dl = sd.sidechain ? dst.scL.data() : dst.inL.data(); float* dr = sd.sidechain ? dst.scR.data() : dst.inR.data(); if (sd.delay) { std::copy(L, L + n, s.tmpL.data()); std::copy(R, R + n, s.tmpR.data()); sd.delay->process(s.tmpL.data(), s.tmpR.data(), n); for (int i = 0; i < n; ++i) { dl[i] += s.tmpL[i] * lv; dr[i] += s.tmpR[i] * lv; } } else for (int i = 0; i < n; ++i) { dl[i] += L[i] * lv; dr[i] += R[i] * lv; } }
    if (t.isMaster) { for (int i = 0; i < n; ++i) { outL[i] += L[i]; outR[i] += R[i]; } }
    else if (t.outputIdx >= 0) { auto& dst = s.tracks[t.outputIdx]; for (int i = 0; i < n; ++i) { dst.inL[i] += L[i]; dst.inR[i] += R[i]; } }
}

bool AudioEngine::guardedProcess(RtEffect& fx, float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx) {
    if (!fx.proc->isPlugin()) { fx.proc->process(L, R, n, midi, ctx); return true; }
    sigjmp_buf jb; sigjmp_buf* prev = t_guard; t_guard = &jb;
    if (sigsetjmp(jb, 1) == 0) { fx.proc->process(L, R, n, midi, ctx); t_guard = prev; return true; }
    t_guard = prev;
    if (fx.crashed) fx.crashed->store(true);
    std::fill(L, L + n, 0.f); std::fill(R, R + n, 0.f);
    EngineMessage m; m.type = EngineMessage::PluginCrashed; m.id = fx.slotId; m_messages.push(m);
    return false;
}

// ---------------------------------------------------------------- RT: live MIDI
void AudioEngine::routeLiveMidi(Snapshot& s, Tick t0) {
    MidiEvent e; bool rec = m_recordingFlag.load() && m_playing.load() && !m_countingIn.load();
    auto handle = [&](MidiEvent ev, bool fromHardware) {
        ev.sampleOffset = 0;
        // controller mappings
        if (ev.type == MidiEvent::CC) { for (auto& m : s.midiMappings) if (!m.isNote && m.cc == ev.data1 && (m.midiChannel < 0 || m.midiChannel == ev.channel)) m.target.setNormalized(ev.data2 / 127.f); }
        if (s.liveChannelIdx >= 0 && s.liveChannelIdx < (int)s.channels.size()) {
            auto& c = s.channels[s.liveChannelIdx]; if (ev.type == MidiEvent::NoteOn && c.cutItself) { MidiEvent cut; cut.type = MidiEvent::AllNotesOff; cut.data1 = 1; cut.sampleOffset = 0; c.midi.add(cut); } c.midi.add(ev);
            if (ev.type == MidiEvent::NoteOn && c.instrument && c.instrument->kind() == ProcessorKind::Instrument) {}
        }
        if (rec && (ev.type == MidiEvent::NoteOn || ev.type == MidiEvent::NoteOff)) { EngineMessage m; m.type = EngineMessage::RecordedMidi; m.midi = ev; m.tick = m_posTicks.load(); m_messages.push(m); }
        (void)fromHardware;
    };
    while (m_midiIn.pop(e)) handle(e, true);
    while (m_uiMidi.pop(e)) handle(e, false);
}

// ---------------------------------------------------------------- RT: sequencer
void AudioEngine::runSequencer(Snapshot& s, Tick t0, Tick t1, int n) {
    double spt = samplesPerTick();
    if (!m_songMode.load()) {
        if (s.selectedPatternIdx < 0) return;
        const RtPattern& pat = s.patterns[s.selectedPatternIdx];
        scheduleNotes(s, pat, t0, t1, 0, n, t0, t1);
        for (int li : pat.laneIndices) { auto& lane = s.lanes[li]; if (lane.target.valid()) lane.target.setNormalized(lane.valueAt(t0)); }
        return;
    }
    for (auto& clip : s.clips) {
        if (clip.muted || clip.end <= t0 || clip.start >= t1) continue;
        if (clip.type == ClipType::Pattern && clip.patternIdx >= 0) {
            const RtPattern& pat = s.patterns[clip.patternIdx]; Tick len = std::max<Tick>(pat.length, 1);
            Tick lo = std::max(t0, clip.start) - clip.start + clip.sourceOffset, hi = std::min(t1, clip.end) - clip.start + clip.sourceOffset;
            if (!clip.loopSource && lo >= len) continue;
            Tick kStart = lo / len, kEnd = (hi - 1) / len; if (!clip.loopSource) kEnd = std::min<Tick>(kEnd, 0);
            for (Tick k = kStart; k <= kEnd; ++k) {
                Tick wl = std::max<Tick>(lo - k * len, 0), wh = std::min<Tick>(hi - k * len, len);
                if (wh <= wl) continue;
                // tickOffset: pattern-local tick -> global tick
                Tick off = clip.start - clip.sourceOffset + k * len;
                scheduleNotes(s, pat, wl, wh, off, n, t0, t1);
                for (int li : pat.laneIndices) { auto& lane = s.lanes[li]; if (lane.target.valid()) lane.target.setNormalized(lane.valueAt(std::max(wl, std::min(t0 - off, wh)))); }
            }
        } else if (clip.type == ClipType::Automation && clip.laneIdx >= 0) {
            auto& lane = s.lanes[clip.laneIdx]; if (!lane.target.valid()) continue;
            Tick local = std::max(t0, clip.start) - clip.start + clip.sourceOffset; Tick len = std::max<Tick>(lane.length, 1); if (clip.loopSource && lane.length > 0) local %= len;
            lane.target.setNormalized(lane.valueAt(local));
        }
    }
    (void)spt; (void)n;
}

void AudioEngine::scheduleNotes(Snapshot& s, const RtPattern& pat, Tick winStart, Tick winEnd, Tick tickOffset, int n, Tick t0, Tick t1) {
    double spt = samplesPerTick(); double pos = m_posTicks.load();
    for (auto& pc : pat.channels) {
        if (pc.channelIndex < 0 || pc.channelIndex >= (int)s.channels.size()) continue;
        auto& ch = s.channels[pc.channelIndex]; if (!ch.instrument || !ch.enabled) continue;
        // notes sorted by start: binary search first note with start >= winStart
        const auto& notes = pc.notes; size_t lo = 0, hi = notes.size();
        while (lo < hi) { size_t mid = (lo + hi) / 2; if (notes[mid].start < winStart) lo = mid + 1; else hi = mid; }
        for (size_t i = lo; i < notes.size() && notes[i].start < winEnd; ++i) {
            const RtNote& nt = notes[i];
            if (nt.probability < 1.f && m_rng.uniform() > nt.probability) continue;
            Tick global = nt.start + tickOffset; int off = (int)std::floor((global - pos) * spt); off = clamp(off, 0, n - 1);
            int durSamples = std::max(1, (int)((nt.end - nt.start) * spt));
            MidiEvent e; e.type = MidiEvent::NoteOn; e.data1 = (uint8_t)clamp(nt.pitch, 0, 127); e.data2 = (uint8_t)(nt.velocity * 127); e.velocity = nt.velocity; e.pan = nt.pan; e.finePitch = nt.finePitch; e.sampleOffset = off; e.channel = (uint8_t)nt.midiChannel; e.noteId = (int)(global & 0x7fffffff);
            if (ch.cutItself) { MidiEvent cut; cut.type = MidiEvent::AllNotesOff; cut.data1 = 1; /* choke: fast fade */ cut.sampleOffset = off; ch.midi.add(cut); }   // 'Cut itself': the new note replaces whatever the channel is still playing
            ch.midi.add(e); ch.noteOffs.add(off + durSamples, e.data1, e.channel, e.noteId);
        }
    }
    (void)t0; (void)t1;
}

void AudioEngine::renderAudioClips(Snapshot& s, Tick t0, Tick t1, int n) {
    double spt = samplesPerTick();
    for (size_t ci = 0; ci < s.clips.size(); ++ci) {
        auto& clip = s.clips[ci]; auto& st = s.clipStates[ci];
        if (clip.type != ClipType::Audio || !clip.asset || clip.muted || clip.trackIdx < 0) { st.active = false; continue; }
        if (clip.end <= t0 || clip.start >= t1) { st.active = false; continue; }
        const AudioAsset* a = clip.asset.get(); auto& t = s.tracks[clip.trackIdx];
        int64_t srcLen = std::max<int64_t>(1, clip.srcEnd - clip.srcStart);
        double posTicks = m_posTicks.load();
        // block-local start sample where the clip becomes active
        int startOff = clip.start > t0 ? clamp((int)std::floor((clip.start - posTicks) * spt), 0, n - 1) : 0;
        int endOff = clip.end < t1 ? clamp((int)std::ceil((clip.end - posTicks) * spt), 0, n) : n;
        if (!st.active || m_transportJumped) { double ticksIn = (posTicks + startOff / spt) - clip.start + clip.sourceOffset; st.pos = ticksToSeconds((Tick)ticksIn, m_bpm.load()) * m_sr * clip.rate; st.active = true; }
        float g = clip.gain; double fadeInS = clip.fadeIn * spt, fadeOutS = clip.fadeOut * spt; double clipLenS = (clip.end - clip.start) * spt; double clipPosS = (posTicks - clip.start) * spt;
        for (int i = startOff; i < endOff; ++i) {
            double p = st.pos; if (p >= srcLen) { if (!clip.loopSource) break; p = std::fmod(p, (double)srcLen); st.pos = p; }
            double rp = clip.reverse ? (srcLen - 1 - p) : p; double fr = clip.srcStart + rp;
            float l = dsp::readInterp(a->L(), a->frames, fr), r = dsp::readInterp(a->R(), a->frames, fr);
            float env = g; double cp = clipPosS + i;
            if (fadeInS > 0 && cp < fadeInS) env *= (float)(cp / fadeInS);
            if (fadeOutS > 0 && cp > clipLenS - fadeOutS) env *= (float)std::max(0.0, (clipLenS - cp) / fadeOutS);
            t.genL[i] += l * env; t.genR[i] += r * env; st.pos += clip.rate;
        }
    }
}

void AudioEngine::applyAutomation(Snapshot&, Tick) {}

// ---------------------------------------------------------------- RT: metronome & preview
void AudioEngine::renderMetronome(float* L, float* R, int n, Tick t0, Tick t1, bool countIn) {
    TimeSignature ts{m_tsNum.load(), m_tsDen.load()}; Tick beat = ts.ticksPerBeat(); double spt = samplesPerTick();
    Tick firstBeat = (t0 + beat - 1) / beat;
    for (Tick b = firstBeat; b * beat < t1; ++b) {
        int off = clamp((int)((b * beat - t0) * spt), 0, n - 1); bool down = (b % ts.numerator) == 0;
        // render the click from 'off' onwards
        m_clickEnv = 1.f; m_clickPhase = 0; m_clickFreq = down ? 1400.f : 1000.f;
        (void)off; (void)countIn;
        // note: the click starts at 'off'; for simplicity the tail of a previous click is overwritten
        for (int i = off; i < n; ++i) { float s = std::sin((float)(m_clickPhase * dsp::kTwoPi)) * m_clickEnv * 0.5f; m_clickPhase += m_clickFreq / m_sr; m_clickEnv *= 0.9985f; L[i] += s; R[i] += s; }
        return; // one click per block is enough in practice (blocks are far shorter than a beat)
    }
    if (m_clickEnv > 0.001f) for (int i = 0; i < n; ++i) { float s = std::sin((float)(m_clickPhase * dsp::kTwoPi)) * m_clickEnv * 0.5f; m_clickPhase += m_clickFreq / m_sr; m_clickEnv *= 0.9985f; L[i] += s; R[i] += s; }
}
void AudioEngine::renderPreview(float* L, float* R, int n) {
    m_preview.rtTick();
    if (!m_previewPlaying.load()) return;
    const PreviewBuffer* b = m_preview.get(); if (!b || b->failed.load()) { m_previewPlaying.store(false); return; }
    int64_t avail = b->available.load(std::memory_order_acquire); if (avail <= 0) { if (b->done.load()) m_previewPlaying.store(false); return; }   // still opening / decoding the first chunk
    if (b->firstAudioUs.load() < 0) const_cast<PreviewBuffer*>(b)->firstAudioUs.store(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    float g = m_previewGain.load(); double rate = b->fileRate / m_sr; int64_t total = b->frames;
    for (int i = 0; i < n; ++i) {
        if (m_previewPosRt >= total - 1) { m_previewPlaying.store(false); break; }
        if (m_previewPosRt + 1 >= avail) { if (b->done.load()) { m_previewPlaying.store(false); } break; }   // data not decoded yet: wait (silence)
        L[i] += dsp::readInterp(b->L(), avail, m_previewPosRt) * g; R[i] += dsp::readInterp(b->R(), avail, m_previewPosRt) * g; m_previewPosRt += rate;
    }
    m_previewPos.store(m_previewPosRt);
}

} // namespace mt
