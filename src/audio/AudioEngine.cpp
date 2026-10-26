#include "audio/AudioEngine.h"
#include "audio/RecordingEngine.h"
#include "core/Log.h"
#include <csetjmp>
#include <csignal>
#include <chrono>
#include <limits>
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
    sigaction(sig, old, nullptr);
    raise(sig);
}
void installCrashGuard() {
    static bool done = false;
    if (done) return;
    done = true;
    struct sigaction sa{};
    sa.sa_sigaction = crashHandler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &s_oldSegv);
    sigaction(SIGFPE, &sa, &s_oldFpe);
    sigaction(SIGILL, &sa, &s_oldIll);
    sigaction(SIGBUS, &sa, &s_oldBus);
}

float RtAutomationLane::valueAt(Tick t) const { return evalAutomation(points, t); }

// ---------------------------------------------------------------- engine
AudioEngine::AudioEngine(AudioDeviceManager& devices, SampleCache& cache, QObject* parent)
    : QObject(parent)
    , m_devices(devices)
    , m_cache(cache) {
    installCrashGuard();
    m_outL.resize(8192);
    m_outR.resize(8192);
    m_dryL.resize(8192);
    m_dryR.resize(8192);
    connect(&m_cache, &SampleCache::assetLoaded, this, &AudioEngine::onAssetLoaded);
    connect(&m_pollTimer, &QTimer::timeout, this, &AudioEngine::poll);
    m_pollTimer.start(16);
}
AudioEngine::~AudioEngine() {
    stopDevice();
    m_rtSnapshot = nullptr; /* owned by m_inFlight */
}

bool AudioEngine::startDevice(QString* error) {
    const auto& cfg = m_devices.config();
    bool ok = m_devices.open(
        cfg,
        [this](const float* in, float* out, int frames, int inCh, int outCh, bool xrun) { rtCallback(in, out, frames, inCh, outCh, xrun); },
        error);
    if (!ok) return false;
    double sr = m_devices.actualSampleRate();
    int block = cfg.bufferSize;
    if (sr != m_sr || block > m_maxBlock) {
        m_sr = sr;
        m_maxBlock = std::max(block, 64);
        m_cache.setSampleRate(sr);
        m_registry.prepareAll(m_sr, m_maxBlock);
        m_rebuildPending = true;
    }
    return true;
}
void AudioEngine::stopDevice() { m_devices.close(); }

// ---------------------------------------------------------------- transport API
void AudioEngine::play() {
    EngineCommand c;
    c.type = EngineCommand::Play;
    m_commands.push(c);
}
void AudioEngine::stop() {
    EngineCommand c;
    c.type = EngineCommand::Stop;
    m_commands.push(c);
}
void AudioEngine::pause() {
    EngineCommand c;
    c.type = EngineCommand::Pause;
    m_commands.push(c);
}
void AudioEngine::togglePlay() {
    if (m_playing.load()) stop();
    else play();
}
void AudioEngine::seek(Tick t) {
    EngineCommand c;
    c.type = EngineCommand::Seek;
    c.value = (double)std::max<Tick>(0, t);
    m_commands.push(c);
}
void AudioEngine::setRecording(bool r) {
    EngineCommand c;
    c.type = EngineCommand::SetRecording;
    c.ival = r ? 1 : 0;
    m_commands.push(c);
}
void AudioEngine::allNotesOff() {
    EngineCommand c;
    c.type = EngineCommand::AllNotesOff;
    m_commands.push(c);
}
void AudioEngine::previewPlay(std::shared_ptr<PreviewBuffer> b) {
    if (!b) return;
    m_preview.set(b);
    EngineCommand c;
    c.type = EngineCommand::PreviewPlay;
    m_commands.push(c);
}
void AudioEngine::previewStop() {
    EngineCommand c;
    c.type = EngineCommand::PreviewStop;
    m_commands.push(c);
}
void AudioEngine::previewSeek(double frame) {
    EngineCommand c;
    c.type = EngineCommand::PreviewPlay;
    c.value = frame;
    m_commands.push(c);
}

TransportView AudioEngine::transport() const {
    TransportView v;
    v.playing = m_playing.load();
    v.recording = m_recordingFlag.load();
    v.paused = m_paused.load();
    v.countingIn = m_countingIn.load();
    v.positionTicks = m_posTicks.load();
    const TempoMap& m = m_tempoUi;
    v.bpm = m.hasTempoPoints() ? m.bpmAt(v.positionTicks) : m_bpm.load();
    v.seconds = m.hasTempoPoints() ? m.secondsAt(v.positionTicks) : v.positionTicks * 60.0 / (v.bpm * PPQ);
    v.songMode = m_songMode.load();
    v.loop = m_loopEnabled.load();
    v.loopStart = m_loopStart.load();
    v.loopEnd = m_loopEnd.load();
    Tick pos = std::max<Tick>(0, (Tick)v.positionTicks);
    TimeSignature ts = m.sigAt(pos);
    v.tsNum = ts.numerator;
    v.tsDen = ts.denominator;
    int64_t bar = m.barAt(pos);
    Tick inBar = pos - m.barStart(bar), beat = ts.ticksPerBeat();
    v.bar = (int)bar + 1;
    v.beat = (int)(inBar / beat) + 1;
    v.tick = (int)(inBar % beat);
    return v;
}

void AudioEngine::offlineBegin(double sampleRate, int blockSize) {
    stopDevice();
    m_offline = true;
    m_savedSr = m_sr;
    m_savedBlock = m_maxBlock;
    m_sr = sampleRate;
    m_maxBlock = std::max(blockSize, 64);
    m_cache.setSampleRate(sampleRate);
    m_registry.prepareAll(m_sr, m_maxBlock);
    if (m_lastProject) rebuild(*m_lastProject);
    drainCommands();
}
void AudioEngine::offlineProcess(float* outL, float* outR, int n, const float* in, int inCh) { processFrames(in, inCh, outL, outR, n); }
// Every source reaches a track's output delayed by maxLatency minus the latency still ahead of it (PDC aligns everything at the
// master), so that is what a render taken there has to cut from the front
int AudioEngine::renderLatency() const {
    if (!m_rtSnapshot) return 0;
    Id tap = m_renderTap.load();
    for (auto& t : m_rtSnapshot->tracks)
        if (tap ? t.id == tap : t.isMaster)
            return m_rtSnapshot->maxLatency -
                (tap && m_renderTapIn.load() ? t.pathLatency
                        : t.outputIdx >= 0   ? m_rtSnapshot->tracks[t.outputIdx].pathLatency
                                             : 0);
    return 0;
}
void AudioEngine::offlineEnd() {
    m_offline = false;
    drainCommands();
    m_sr = m_savedSr;
    m_maxBlock = m_savedBlock;
    m_cache.setSampleRate(m_sr);
    m_registry.prepareAll(m_sr, m_maxBlock);
    if (m_lastProject) rebuild(*m_lastProject);
    QString err;
    startDevice(&err);
}

void AudioEngine::unloadProject() {
    // Send an empty snapshot and wait until the RT thread has adopted it, then free everything.
    m_waitingSamples.clear();
    auto empty = std::make_unique<Snapshot>();
    empty->generation = m_nextGen++;
    empty->maxBlock = m_maxBlock;
    empty->tmpL.resize(m_maxBlock);
    empty->tmpR.resize(m_maxBlock);
    uint64_t gen = empty->generation;
    EngineCommand c;
    c.type = EngineCommand::SetSnapshot;
    c.snapshot = empty.get();
    m_inFlight.push_back(std::move(empty));
    m_commands.push(c);
    if (m_devices.isOpen()) {
        for (int i = 0; i < 200 && m_activeGen.load() < gen; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } else {
        drainCommands();
    }
    poll();
    m_registry.clearAll();
    m_lastProject = nullptr;
}

// ---------------------------------------------------------------- UI pump
void AudioEngine::pollMessages() {
    EngineMessage m;
    while (m_messages.pop(m)) {
        switch (m.type) {
        case EngineMessage::SnapshotRetired: {
            m_retired = std::max(m_retired, m.snapshot->generation);
            m_anyRetired = true;
            for (size_t i = 0; i < m_inFlight.size(); ++i)
                if (m_inFlight[i].get() == m.snapshot) {
                    m_inFlight.erase(m_inFlight.begin() + i);
                    break;
                }
            break;
        }
        case EngineMessage::RecordedMidi: Q_EMIT recordedMidi(m.midi, m.tick); break;
        case EngineMessage::PluginCrashed: Q_EMIT pluginCrashed(m.id); break;
        case EngineMessage::RecordSegment: Q_EMIT recordSegment(m.tick, m.sample, m.id != 0); break;
        default: break;
        }
    }
}
void AudioEngine::poll() {
    pollMessages();
    bool idle = !m_devices.isOpen() && !m_offline;
    if (m_anyRetired || idle) m_registry.collectGarbage(idle ? m_activeGen.load() + 2 : m_retired, idle);
    m_anyRetired = false;
    m_retired = 0;
    for (auto& [id, p] : m_registry.processors())
        if (auto* ib = dynamic_cast<InstrumentBase*>(p.get())) ib->collectGarbage(idle);
    m_preview.prune(idle);
    if (m_rebuildPending && m_lastProject) {
        m_rebuildPending = false;
        rebuild(*m_lastProject);
    }
    static bool wasPlaying = false;
    bool nowPlaying = m_playing.load();
    if (nowPlaying != wasPlaying) {
        wasPlaying = nowPlaying;
        Q_EMIT transportStateChanged();
    }
}

void AudioEngine::onAssetLoaded(const QString&) {
    resolvePendingSamples();
    if (m_lastProject) m_rebuildPending = true;
}
void AudioEngine::resolvePendingSamples() {
    for (auto it = m_waitingSamples.begin(); it != m_waitingSamples.end();) {
        auto* proc = it->first;
        const QString& path = it->second;
        bool done = false;
        auto asset = m_cache.get(path);
        if (asset) {
            if (auto* s = dynamic_cast<Sampler*>(proc)) {
                if (s->pendingPath == path) {
                    s->setSample(asset, path);
                    s->pendingPath.clear();
                }
                done = true;
            } else if (auto* d = dynamic_cast<DrumSampler*>(proc)) {
                for (int i = 0; i < DrumSampler::kPads; ++i)
                    if (d->padPath(i) == path && !d->padSample(i)) d->setPadSample(i, asset, path);
                done = true;
            } else done = true;
        }
        if (done) it = m_waitingSamples.erase(it);
        else ++it;
    }
}

// ---------------------------------------------------------------- RT: commands
void AudioEngine::drainCommands() {
    EngineCommand c;
    while (m_commands.pop(c)) {
        switch (c.type) {
        case EngineCommand::SetSnapshot: {
            Snapshot* old = m_rtSnapshot;
            m_rtSnapshot = c.snapshot;
            m_activeGen.store(c.snapshot->generation);
            m_patternLength.store(
                c.snapshot->selectedPatternIdx >= 0 ? c.snapshot->patterns[c.snapshot->selectedPatternIdx].length : PPQ * 4);
            m_maxLatency.store(c.snapshot->maxLatency);
            if (old) {
                EngineMessage m;
                m.type = EngineMessage::SnapshotRetired;
                m.snapshot = old;
                m_messages.push(m);
            }
            if (old) {
                for (size_t i = 0; i < c.snapshot->clipStates.size() && i < old->clipStates.size(); ++i)
                    c.snapshot->clipStates[i] = old->clipStates[i];   // keep audio clip playheads continuous
                // carry pending note-offs over so notes sounding across a rebuild are still released (no allocation: fixed arrays)
                for (auto& nc : c.snapshot->channels)
                    for (auto& oc : old->channels)
                        if (oc.id == nc.id) {
                            if (nc.instrument == oc.instrument) nc.noteOffs = oc.noteOffs;
                            else if (oc.instrument) {
                                MidiEvent e;
                                e.type = MidiEvent::AllNotesOff;
                                nc.midi.add(e);
                            }
                            break;
                        }
            }
            break;
        }
        case EngineCommand::Play: {
            if (m_playing.load()) break;
            m_previewPlaying.store(false);
            m_playing.store(true);
            m_paused.store(false);
            m_transportJumped = true;
            if (m_countIn.load() && m_recordingFlag.load()) {
                m_countInRemaining = (double)sigAt(m_posTicks.load()).ticksPerBar() * m_countInBars.load();
                m_countingIn.store(true);
            } else {
                m_countInRemaining = 0;
                m_countingIn.store(false);
            }
            break;
        }
        case EngineCommand::Stop: {
            bool was = m_playing.load();
            m_playing.store(false);
            m_paused.store(false);
            m_countingIn.store(false);
            m_recordingFlag.store(false);
            if (was) {
                if (m_songMode.load()) m_posTicks.store((double)(m_loopEnabled.load() ? m_loopStart.load() : 0));
                else m_posTicks.store(0.0);
            }
            releaseAllNotes(false);
            m_transportJumped = true;
            break;
        }
        case EngineCommand::Pause: {
            m_playing.store(false);
            m_paused.store(true);
            m_countingIn.store(false);
            releaseAllNotes(false);
            break;
        }
        case EngineCommand::Seek:
            m_posTicks.store(c.value);
            m_transportJumped = true;
            releaseAllNotes(false);
            break;
        case EngineCommand::SetRecording: m_recordingFlag.store(c.ival != 0); break;
        case EngineCommand::AllNotesOff:
        case EngineCommand::Panic: releaseAllNotes(true); break;
        case EngineCommand::PreviewPlay:
            m_previewPosRt = c.value;
            m_previewPos.store(c.value);
            m_previewPlaying.store(true);
            break;
        case EngineCommand::PreviewStop: m_previewPlaying.store(false); break;
        default: break;
        }
    }
}

void AudioEngine::releaseAllNotes(bool resetInstruments) {
    if (!m_rtSnapshot) return;
    for (auto& ch : m_rtSnapshot->channels) {
        ch.noteOffs.clear();
        MidiEvent e;
        e.type = MidiEvent::AllNotesOff;
        ch.midi.add(e);
        e.type = MidiEvent::CC;   // pedal up too: a sustain lane may have left it down, holding every released note
        e.data1 = 64;
        e.data2 = 0;
        ch.midi.add(e);
        if (resetInstruments && ch.instrument) ch.instrument->reset();
    }
}

// ---------------------------------------------------------------- RT: callback
void AudioEngine::rtCallback(const float* in, float* out, int frames, int inCh, int outCh, bool xrun) {
    auto t0 = std::chrono::steady_clock::now();
    if (frames > (int)m_outL.size()) frames = (int)m_outL.size();
    processFrames(in, inCh, m_outL.data(), m_outR.data(), frames);
    for (int i = 0; i < frames; ++i) {
        float l = clamp(m_outL[i], -1.f, 1.f), r = clamp(m_outR[i], -1.f, 1.f);
        for (int c = 0; c < outCh; ++c) out[i * outCh + c] = (c % 2 == 0) ? l : r;
    }
    auto t1 = std::chrono::steady_clock::now();
    double used = std::chrono::duration<double>(t1 - t0).count();
    double avail = frames / m_sr;
    float load = (float)(used / avail);
    m_cpuLoad.store(m_cpuLoad.load() * 0.8f + load * 0.2f);
}

void AudioEngine::processFrames(const float* in, int inCh, float* outL, float* outR, int frames) {
    drainCommands();
    std::fill(outL, outL + frames, 0.f);
    std::fill(outR, outR + frames, 0.f);
    Snapshot* snap = m_rtSnapshot;
    int offset = 0;
    int remaining = frames;
    while (remaining > 0) {
        int n = std::min(remaining, snap ? snap->maxBlock : m_maxBlock);
        double pos = m_posTicks.load();
        bool playing = m_playing.load();
        bool countIn = m_countingIn.load();
        Tick loopS = 0, loopE = 0;
        bool loop = false;
        if (playing && !countIn) {
            if (m_songMode.load()) {
                loop = m_loopEnabled.load() && (m_loopRecord.load() || !m_recordingFlag.load());
                loopS = m_loopStart.load();
                loopE = m_loopEnd.load();
                if (!m_loopEnabled.load() && !m_recordingFlag.load() && m_songLoop.load() && snap && snap->songEnd > 0) {
                    loop = true;   // no loop area: the whole song repeats, as in FL Studio (a recording runs on past the end)
                    loopS = 0;
                    loopE = snap->songEnd;
                }
            } else {
                loop = true;
                loopS = 0;
                loopE = std::max<Tick>(m_patternLength.load(), PPQ);
            }
            if (loop && loopE > loopS && (pos >= loopE || pos < loopS)) {
                pos = (double)loopS;
                m_posTicks.store(pos);
                m_transportJumped = true;
            }
        }
        // the tempo at the position: the snapshot's map, or m_bpm (transport bar drag, external sync) when the map has no points
        m_map = snap && snap->tempo.hasTempoPoints() ? &snap->tempo : nullptr;
        m_blockBpm = m_map ? m_map->bpmAt(pos) : m_bpm.load();
        double spt = m_sr * 60.0 / (m_blockBpm * PPQ), endPos = pos;
        Tick t0 = (Tick)std::floor(pos);
        Tick t1 = t0;
        if (playing && !countIn) {
            // sub-blocks end at the loop end and at tempo points (a jump never falls inside one; ramps are taken per sub-block)
            Tick lim = loop && loopE > loopS ? loopE : std::numeric_limits<Tick>::max();
            if (m_map) lim = std::min(lim, m_map->nextTempoPoint(pos));
            if (lim != std::numeric_limits<Tick>::max()) {
                int samplesTo = (int)std::ceil(m_map ? (m_map->secondsAt(lim) - m_map->secondsAt(pos)) * m_sr : (lim - pos) * spt);
                if (samplesTo < n) n = std::max(1, samplesTo);
            }
            endPos = m_map ? m_map->tickAt(m_map->secondsAt(pos) + n / m_sr) : pos + n / spt;
            spt = n / (endPos - pos);
            t1 = (Tick)std::floor(endPos);
            if (t1 <= t0) t1 = t0 + 1;
        } else if (playing && countIn) {
            m_countInRemaining -= n / spt;
        }
        m_spt = spt;
        processSubBlock(in ? in + (size_t)offset * inCh : nullptr, inCh, outL + offset, outR + offset, n, snap, t0, t1);
        if (playing && countIn) {   // count-in bars in the signature of the position recording starts at
            TimeSignature ts = sigAt(pos);
            Tick total = ts.ticksPerBar() * m_countInBars.load();
            Tick c0 = total - (Tick)(m_countInRemaining + n / spt), c1 = total - (Tick)m_countInRemaining;
            renderMetronome(outL + offset, outR + offset, n, c0, c1, ts, 0);
            if (m_countInRemaining <= 0) m_countingIn.store(false);   // after the block: the last count-in block must not play or record
        } else if (playing && m_metronome.load() && snap)
            renderMetronome(outL + offset, outR + offset, n, t0, t1, snap->tempo.sigAt(t0), snap->tempo.barStart(snap->tempo.barAt(t0)));
        if (m_midiClock.load()) {
            // MIDI clock: 24 pulses per quarter note while the transport runs
            bool run = playing && !countIn;
            if (run && !m_clockRunning) {
                TimedMidiOut t;
                t.event.type = MidiEvent::Start;
                t.port = -1;
                t.sampleTime = m_samplesProcessed.load();
                m_midiOut.push(t);
                m_clockRunning = true;
                m_clockAccum = 0;
            }
            if (!run && m_clockRunning) {
                TimedMidiOut t;
                t.event.type = MidiEvent::Stop;
                t.port = -1;
                t.sampleTime = m_samplesProcessed.load();
                m_midiOut.push(t);
                m_clockRunning = false;
            }
            if (run) {
                double pulseSamples = spt * PPQ / 24.0;
                m_clockAccum += n;
                while (m_clockAccum >= pulseSamples) {
                    m_clockAccum -= pulseSamples;
                    TimedMidiOut t;
                    t.event.type = MidiEvent::Clock;
                    t.port = -1;
                    t.sampleTime = m_samplesProcessed.load() + offset + n - (int64_t)m_clockAccum;
                    m_midiOut.push(t);
                }
            }
        }
        if (playing && !countIn) {
            double newPos = endPos;
            if (loop && loopE > loopS && newPos >= loopE - 1e-6) {
                newPos = (double)loopS;
                m_transportJumped = true;
            } else m_transportJumped = false;
            m_posTicks.store(newPos);
        }
        offset += n;
        remaining -= n;
    }
    renderPreview(outL, outR, frames);
    m_samplesProcessed.fetch_add(frames);
}

// Stable sort by sample offset; at the same offset note-offs go first so a note ending exactly where the next one (same pitch) starts
// doesn't cut the new note (back-to-back 808s, legato step sequences)
static inline int midiRank(const MidiEvent& e) { return (e.type == MidiEvent::NoteOff || e.type == MidiEvent::AllNotesOff) ? 0 : 1; }
static inline bool midiBefore(const MidiEvent& a, const MidiEvent& b) {
    return a.sampleOffset < b.sampleOffset || (a.sampleOffset == b.sampleOffset && midiRank(a) < midiRank(b));
}
static inline void sortMidi(MidiEventList& l) {
    for (int i = 1; i < l.count; ++i) {
        MidiEvent k = l.events[i];
        int j = i - 1;
        while (j >= 0 && midiBefore(k, l.events[j])) {
            l.events[j + 1] = l.events[j];
            --j;
        }
        l.events[j + 1] = k;
    }
}

void AudioEngine::processSubBlock(const float* in, int inCh, float* outL, float* outR, int n, Snapshot* snap, Tick t0, Tick t1) {
    if (!snap) return;
    Snapshot& s = *snap;
    ProcessContext ctx;
    ctx.sampleRate = m_sr;
    ctx.blockSize = n;
    ctx.bpm = m_blockBpm;
    TimeSignature ts = s.tempo.sigAt(t0);
    ctx.tsNum = ts.numerator;
    ctx.tsDen = ts.denominator;
    ctx.playing = m_playing.load() && !m_countingIn.load();
    ctx.recording = m_recordingFlag.load();
    ctx.positionBeats = (double)t0 / PPQ;
    ctx.barNumber = s.tempo.barAt(t0);
    ctx.barStartBeats = (double)s.tempo.barStart(ctx.barNumber) / PPQ;
    ctx.positionSamples = (int64_t)(secondsBetween(0, t0) * m_sr);
    ctx.looping = m_loopEnabled.load();
    ctx.loopStartBeats = (double)m_loopStart.load() / PPQ;
    ctx.loopEndBeats = (double)m_loopEnd.load() / PPQ;
    for (auto& t : s.tracks) {
        std::fill(t.inL.begin(), t.inL.begin() + n, 0.f);
        std::fill(t.inR.begin(), t.inR.begin() + n, 0.f);
        std::fill(t.scL.begin(), t.scL.begin() + n, 0.f);
        std::fill(t.scR.begin(), t.scR.begin() + n, 0.f);
        std::fill(t.genL.begin(), t.genL.begin() + n, 0.f);
        std::fill(t.genR.begin(), t.genR.begin() + n, 0.f);
    }
    for (auto& c : s.channels) {
        std::fill(c.bufL.begin(), c.bufL.begin() + n, 0.f);
        std::fill(c.bufR.begin(), c.bufR.begin() + n, 0.f);
    }
    // Live MIDI (from hardware and from the UI keyboard)
    routeLiveMidi(s, t0);
    // Sequencer
    if (ctx.playing) runSequencer(s, t0, t1, n);
    // Pending note-offs from note durations
    for (auto& c : s.channels) {
        c.noteOffs.advance(n, c.midi);
        sortMidi(c.midi);
    }
    // Instruments -> channel buffers -> mixer track inputs
    for (auto& c : s.channels) {
        if (!c.instrument) {
            c.midi.clear();
            continue;
        }
        c.instrument->process(c.bufL.data(), c.bufR.data(), n, c.midi, ctx);
        if (c.isMidiOut) {
            if (auto* om = c.instrument->outputMidi())
                for (int i = 0; i < om->count; ++i) {
                    TimedMidiOut t;
                    t.event = om->events[i];
                    t.port = c.midiOutPort;
                    t.sampleTime = m_samplesProcessed.load() + om->events[i].sampleOffset;
                    m_midiOut.push(t);
                }
        }
        c.midi.clear();
        if (!c.audible || !c.enabled) continue;
        float vol = c.ctl->volume.load() / 0.8f, pan = c.ctl->pan.load();
        float a = (pan + 1.f) * 0.25f * dsp::kPi;
        float gl = std::cos(a) * 1.4142f * vol, gr = std::sin(a) * 1.4142f * vol;
        float pk = 0, pr = 0;
        for (int i = 0; i < n; ++i) {
            c.bufL[i] *= gl;
            c.bufR[i] *= gr;
            pk = std::max(pk, std::fabs(c.bufL[i]));
            pr = std::max(pr, std::fabs(c.bufR[i]));
        }
        c.ctl->meter.peakL.store(pk);
        c.ctl->meter.peakR.store(pr);
        if (c.mixerTrackIdx >= 0 && c.mixerTrackIdx < (int)s.tracks.size()) {
            auto& t = s.tracks[c.mixerTrackIdx];
            for (int i = 0; i < n; ++i) {
                t.genL[i] += c.bufL[i];
                t.genR[i] += c.bufR[i];
            }
        }
        // multi-out: extra output pairs straight to their mixer tracks (channel volume applies, pan is the main pair's)
        int nAux = std::min((int)c.auxTrackIdx.size(), c.instrument->auxOutputCount());
        for (int k = 0; k < nAux; ++k) {
            if (c.auxTrackIdx[k] < 0) continue;
            auto& t = s.tracks[c.auxTrackIdx[k]];
            const float *al = c.instrument->auxOutput(k, 0), *ar = c.instrument->auxOutput(k, 1);
            for (int i = 0; i < n; ++i) {
                t.genL[i] += al[i] * vol;
                t.genR[i] += ar[i] * vol;
            }
        }
    }
    // Audio clips
    if (ctx.playing && m_songMode.load())
        renderAudioClips(s, t0, t1, n);   // playlist clips belong to song mode; pattern mode plays the pattern only
    // Hardware inputs -> tracks (monitoring + recording)
    bool recWrite = false;
    if (in && inCh > 0) {
        bool rec = ctx.recording && ctx.playing;
        bool monitor = m_monitorInputs.load();
        if (rec && m_punch.load()) {
            Tick pi = m_punchIn.load(), po = m_punchOut.load();
            if (t0 < pi || t0 >= po) rec = false;
        }
        for (auto& t : s.tracks) {
            if (t.inputChannel < 0 || t.inputChannel >= inCh) continue;
            int chL = t.inputChannel, chR = (t.recordChannels == 2 && t.inputChannel + 1 < inCh) ? t.inputChannel + 1 : t.inputChannel;
            if (monitor || (rec && t.armed))
                for (int i = 0; i < n; ++i) {
                    t.genL[i] += in[i * inCh + chL];
                    t.genR[i] += in[i * inCh + chR];
                }
            if (rec && t.armed && t.recordRing) {
                if (!recWrite && (!m_recWriting || m_transportJumped)) {   // the take restarts here: tell the UI where
                    EngineMessage m;
                    m.type = EngineMessage::RecordSegment;
                    m.tick = m_posTicks.load();
                    m.sample = m_recFrames;
                    m.id = m_recWriting;   // recording ran on through a wrap or seek; otherwise it resumes after a gap
                    m_messages.push(m);
                }
                recWrite = true;
                for (int i = 0; i < n; ++i) {
                    float f[2] = {in[i * inCh + chL], in[i * inCh + chR]};
                    t.recordRing->write(f, t.recordChannels);
                }
            }
        }
    }
    if (recWrite) m_recFrames += n;
    m_recWriting = recWrite;
    // Mixer graph in topological order
    for (int idx : s.trackOrder) processTrack(s, s.tracks[idx], n, ctx, outL, outR);
}

void AudioEngine::processTrack(Snapshot& s, RtTrack& t, int n, const ProcessContext& ctxIn, float* outL, float* outR) {
    ProcessContext ctx = ctxIn;
    ctx.sidechainL = t.scL.data();
    ctx.sidechainR = t.scR.data();
    float* L = t.inL.data();
    float* R = t.inR.data();
    if (t.frozen) {   // its render plays from the playlist: what fed it would play twice
        std::fill(L, L + n, 0.f);
        std::fill(R, R + n, 0.f);
    }
    // generator inputs (instruments, audio clips, hardware inputs) pass through the delay-compensation line first
    t.ctl->generatorDelay.process(t.genL.data(), t.genR.data(), n);
    for (int i = 0; i < n; ++i) {
        L[i] += t.genL[i];
        R[i] += t.genR[i];
    }
    Id tap = m_renderTap.load();
    bool tapPre = m_renderTapPre.load(), tapIn = m_renderTapIn.load();
    if (tap == t.id && tapIn)   // consolidate: what reaches the master, before its effects
        for (int i = 0; i < n; ++i) {
            outL[i] += L[i];
            outR[i] += R[i];
        }
    if (!t.audible) {
        std::fill(L, L + n, 0.f);
        std::fill(R, R + n, 0.f);
    } else {
        for (auto& fx : t.effects) {
            if (fx.bypass || !fx.proc || (fx.crashed && fx.crashed->load())) continue;
            if (fx.mix < 0.999f) {
                std::copy(L, L + n, m_dryL.data());
                std::copy(R, R + n, m_dryR.data());
            }
            static MidiEventList empty;
            if (!guardedProcess(fx, L, R, n, empty, ctx)) continue;
            if (fx.mix < 0.999f)
                for (int i = 0; i < n; ++i) {
                    L[i] = m_dryL[i] + (L[i] - m_dryL[i]) * fx.mix;
                    R[i] = m_dryR[i] + (R[i] - m_dryR[i]) * fx.mix;
                }
        }
    }
    if (tap == t.id && tapPre)
        for (int i = 0; i < n; ++i) {
            outL[i] += L[i];
            outR[i] += R[i];
        }
    // pre-fader sends
    for (auto& sd : t.sends)
        if (sd.pre && sd.targetIdx >= 0) {
            float lv = sd.level->load();
            if (lv <= 0) continue;
            auto& dst = s.tracks[sd.targetIdx];
            float* dl = sd.sidechain ? dst.scL.data() : dst.inL.data();
            float* dr = sd.sidechain ? dst.scR.data() : dst.inR.data();
            if (sd.delay) {
                std::copy(L, L + n, s.tmpL.data());
                std::copy(R, R + n, s.tmpR.data());
                sd.delay->process(s.tmpL.data(), s.tmpR.data(), n);
                for (int i = 0; i < n; ++i) {
                    dl[i] += s.tmpL[i] * lv;
                    dr[i] += s.tmpR[i] * lv;
                }
            } else
                for (int i = 0; i < n; ++i) {
                    dl[i] += L[i] * lv;
                    dr[i] += R[i] * lv;
                }
        }
    // volume / pan with per-block linear ramp
    float vol = t.ctl->volume.load() / 0.8f, pan = t.ctl->pan.load();
    float a = (pan + 1.f) * 0.25f * dsp::kPi;
    float gl = std::cos(a) * 1.4142f * vol, gr = std::sin(a) * 1.4142f * vol;
    float pk = 0, pr = 0, sql = 0, sqr = 0;
    for (int i = 0; i < n; ++i) {
        L[i] *= gl;
        R[i] *= gr;
        float al = std::fabs(L[i]), ar = std::fabs(R[i]);
        pk = std::max(pk, al);
        pr = std::max(pr, ar);
        sql += L[i] * L[i];
        sqr += R[i] * R[i];
    }
    t.ctl->meter.peakL.store(pk);
    t.ctl->meter.peakR.store(pr);
    t.ctl->meter.rmsL.store(std::sqrt(sql / n));
    t.ctl->meter.rmsR.store(std::sqrt(sqr / n));
    if (pk >= 1.f || pr >= 1.f) t.ctl->meter.clipped.store(true);
    // post-fader sends
    for (auto& sd : t.sends)
        if (!sd.pre && sd.targetIdx >= 0) {
            float lv = sd.level->load();
            if (lv <= 0) continue;
            auto& dst = s.tracks[sd.targetIdx];
            float* dl = sd.sidechain ? dst.scL.data() : dst.inL.data();
            float* dr = sd.sidechain ? dst.scR.data() : dst.inR.data();
            if (sd.delay) {
                std::copy(L, L + n, s.tmpL.data());
                std::copy(R, R + n, s.tmpR.data());
                sd.delay->process(s.tmpL.data(), s.tmpR.data(), n);
                for (int i = 0; i < n; ++i) {
                    dl[i] += s.tmpL[i] * lv;
                    dr[i] += s.tmpR[i] * lv;
                }
            } else
                for (int i = 0; i < n; ++i) {
                    dl[i] += L[i] * lv;
                    dr[i] += R[i] * lv;
                }
        }
    if (tap ? t.id == tap && !tapPre && !tapIn : t.isMaster) {
        for (int i = 0; i < n; ++i) {
            outL[i] += L[i];
            outR[i] += R[i];
        }
    }
    if (!t.isMaster && t.outputIdx >= 0) {
        auto& dst = s.tracks[t.outputIdx];
        for (int i = 0; i < n; ++i) {
            dst.inL[i] += L[i];
            dst.inR[i] += R[i];
        }
    }
}

bool AudioEngine::guardedProcess(RtEffect& fx, float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx) {
    if (!fx.proc->isPlugin()) {
        fx.proc->process(L, R, n, midi, ctx);
        return true;
    }
    sigjmp_buf jb;
    sigjmp_buf* prev = t_guard;
    t_guard = &jb;
    if (sigsetjmp(jb, 1) == 0) {
        fx.proc->process(L, R, n, midi, ctx);
        t_guard = prev;
        return true;
    }
    t_guard = prev;
    if (fx.crashed) fx.crashed->store(true);
    std::fill(L, L + n, 0.f);
    std::fill(R, R + n, 0.f);
    EngineMessage m;
    m.type = EngineMessage::PluginCrashed;
    m.id = fx.slotId;
    m_messages.push(m);
    return false;
}

// ---------------------------------------------------------------- RT: live MIDI
void AudioEngine::routeLiveMidi(Snapshot& s, Tick t0) {
    MidiEvent e;
    bool rec = m_recordingFlag.load() && m_playing.load() && !m_countingIn.load();
    auto handle = [&](MidiEvent ev, bool fromHardware) {
        ev.sampleOffset = 0;
        // controller mappings
        bool mapped = false;
        if (ev.type == MidiEvent::CC) {
            for (auto& m : s.midiMappings)
                if (!m.isNote && m.cc == ev.data1 && (m.midiChannel < 0 || m.midiChannel == ev.channel)) {
                    m.target.setNormalized(ev.data2 / 127.f);
                    mapped = true;
                }
        }
        if (s.liveChannelIdx >= 0 && s.liveChannelIdx < (int)s.channels.size()) {
            auto& c = s.channels[s.liveChannelIdx];
            if (ev.type == MidiEvent::NoteOn && c.cutItself) {
                MidiEvent cut;
                cut.type = MidiEvent::AllNotesOff;
                cut.data1 = 1;
                cut.sampleOffset = 0;
                c.midi.add(cut);
            }
            c.midi.add(ev);
            if (ev.type == MidiEvent::NoteOn && c.instrument && c.instrument->kind() == ProcessorKind::Instrument) {}
        }
        // recorded: notes and controller lanes (a CC linked to a control drives that control, it isn't a lane)
        bool lane = (ev.type == MidiEvent::CC && !mapped) || ev.type == MidiEvent::PitchBend || ev.type == MidiEvent::ChannelPressure;
        if (rec && (ev.type == MidiEvent::NoteOn || ev.type == MidiEvent::NoteOff || lane)) {
            EngineMessage m;
            m.type = EngineMessage::RecordedMidi;
            m.midi = ev;
            m.tick = m_posTicks.load();
            m_messages.push(m);
        }
        (void)fromHardware;
    };
    // Live events all land at offset 0 and sortMidi puts note-offs first, so a note-off arriving in the same block as its
    // note-on (fast tap, stalled audio thread, back-to-back audition) would reach the instrument first and leave the note
    // stuck. Hold such a note-off, and anything after it on that pitch, for the next block.
    bool onNow[128] = {}, held[128] = {};
    MidiEventList& prev = m_liveDeferred[m_liveDeferredCur];
    m_liveDeferredCur ^= 1;
    MidiEventList& next = m_liveDeferred[m_liveDeferredCur];
    next.clear();
    auto gate = [&](const MidiEvent& ev, bool fromHardware) {
        if (ev.type == MidiEvent::NoteOn || ev.type == MidiEvent::NoteOff) {
            int p = ev.data1 & 127;
            if (held[p] || (ev.type == MidiEvent::NoteOff && onNow[p])) {
                held[p] = true;
                next.add(ev);
                return;
            }
            if (ev.type == MidiEvent::NoteOn) onNow[p] = true;
        }
        handle(ev, fromHardware);
    };
    for (int i = 0; i < prev.count; ++i) gate(prev.events[i], false);
    prev.clear();
    while (m_midiIn.pop(e)) gate(e, true);
    while (m_uiMidi.pop(e)) gate(e, false);
}

// ---------------------------------------------------------------- RT: sequencer
void AudioEngine::runSequencer(Snapshot& s, Tick t0, Tick t1, int n) {
    double spt = samplesPerTick();
    if (!m_songMode.load()) {
        if (s.selectedPatternIdx < 0) return;
        const RtPattern& pat = s.patterns[s.selectedPatternIdx];
        scheduleNotes(s, pat, t0, t1, 0, n, t0, t1, m_transportJumped);
        for (int li : pat.laneIndices) {
            auto& lane = s.lanes[li];
            if (lane.target.valid()) lane.target.setNormalized(lane.valueAt(t0));
        }
        return;
    }
    for (auto& clip : s.clips) {
        if (clip.muted || clip.end <= t0 || clip.start >= t1) continue;
        if (clip.type == ClipType::Pattern && clip.patternIdx >= 0) {
            const RtPattern& pat = s.patterns[clip.patternIdx];
            Tick len = std::max<Tick>(pat.length, 1);
            Tick lo = std::max(t0, clip.start) - clip.start + clip.sourceOffset,
                 hi = std::min(t1, clip.end) - clip.start + clip.sourceOffset;
            if (!clip.loopSource && lo >= len) continue;
            Tick kStart = lo / len, kEnd = (hi - 1) / len;
            if (!clip.loopSource) kEnd = std::min<Tick>(kEnd, 0);
            for (Tick k = kStart; k <= kEnd; ++k) {
                Tick wl = std::max<Tick>(lo - k * len, 0), wh = std::min<Tick>(hi - k * len, len);
                if (wh <= wl) continue;
                // tickOffset: pattern-local tick -> global tick
                Tick off = clip.start - clip.sourceOffset + k * len;
                scheduleNotes(s, pat, wl, wh, off, n, t0, t1,
                    k == kStart && (m_transportJumped || clip.start >= t0));   // chase lanes on a jump or where the clip starts
                for (int li : pat.laneIndices) {
                    auto& lane = s.lanes[li];
                    if (lane.target.valid()) lane.target.setNormalized(lane.valueAt(std::max(wl, std::min(t0 - off, wh))));
                }
            }
        } else if (clip.type == ClipType::Automation && clip.laneIdx >= 0) {
            auto& lane = s.lanes[clip.laneIdx];
            if (!lane.target.valid()) continue;
            Tick local = std::max(t0, clip.start) - clip.start + clip.sourceOffset;
            Tick len = std::max<Tick>(lane.length, 1);
            if (clip.loopSource && lane.length > 0) local %= len;
            lane.target.setNormalized(lane.valueAt(local));
        }
    }
    (void)spt;
    (void)n;
}

void AudioEngine::scheduleNotes(
    Snapshot& s, const RtPattern& pat, Tick winStart, Tick winEnd, Tick tickOffset, int n, Tick t0, Tick t1, bool chase) {
    double spt = samplesPerTick();
    double pos = m_posTicks.load();
    for (auto& pc : pat.channels) {
        if (pc.channelIndex < 0 || pc.channelIndex >= (int)s.channels.size()) continue;
        auto& ch = s.channels[pc.channelIndex];
        if (!ch.instrument || !ch.enabled) continue;
        // controller lanes go before the notes so a value on a note's tick applies to it; lanes hold their value, so playback
        // starting inside the pattern (play, seek, loop jump, a clip entered mid-way) first sends each lane's last earlier value
        const auto& cs = pc.controls;
        size_t c0 = std::lower_bound(cs.begin(), cs.end(), winStart, [](const RtControl& c, Tick t) { return c.tick < t; }) - cs.begin();
        if (chase) {
            bool sent[kCtrlPitchBend + 1] = {};
            for (size_t i = c0; i-- > 0;) {
                if (sent[cs[i].controller]) continue;
                sent[cs[i].controller] = true;
                MidiEvent e = cs[i].event;
                e.sampleOffset = clamp((int)std::floor((winStart + tickOffset - pos) * spt), 0, n - 1);
                ch.midi.add(e);
            }
        }
        for (size_t i = c0; i < cs.size() && cs[i].tick < winEnd; ++i) {
            MidiEvent e = cs[i].event;
            e.sampleOffset = clamp((int)std::floor((cs[i].tick + tickOffset - pos) * spt), 0, n - 1);
            ch.midi.add(e);
        }
        // notes sorted by start: binary search first note with start >= winStart
        const auto& notes = pc.notes;
        size_t lo = 0, hi = notes.size();
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (notes[mid].start < winStart) lo = mid + 1;
            else hi = mid;
        }
        for (size_t i = lo; i < notes.size() && notes[i].start < winEnd; ++i) {
            const RtNote& nt = notes[i];
            if (nt.probability < 1.f && m_rng.uniform() > nt.probability) continue;
            Tick global = nt.start + tickOffset;
            int off = (int)std::floor((global - pos) * spt);
            off = clamp(off, 0, n - 1);
            int durSamples = std::max(1, (int)(secondsBetween((double)global, (double)(global + nt.end - nt.start)) * m_sr));
            MidiEvent e;
            e.type = MidiEvent::NoteOn;
            e.data1 = (uint8_t)clamp(nt.pitch, 0, 127);
            e.data2 = (uint8_t)(nt.velocity * 127);
            e.velocity = nt.velocity;
            e.pan = nt.pan;
            e.finePitch = nt.finePitch;
            e.sampleOffset = off;
            e.channel = (uint8_t)nt.midiChannel;
            e.noteId = m_nextNoteId;
            m_nextNoteId = (m_nextNoteId + 1) & 0x7fffffff;
            if (ch.cutItself) {
                MidiEvent cut;
                cut.type = MidiEvent::AllNotesOff;
                cut.data1 = 1; /* choke: fast fade */
                cut.sampleOffset = off;
                ch.midi.add(cut);
            }   // 'Cut itself': the new note replaces whatever the channel is still playing
            ch.midi.add(e);
            ch.noteOffs.add(off + durSamples, e.data1, e.channel, e.noteId);
        }
    }
    (void)t0;
    (void)t1;
}

void AudioEngine::renderAudioClips(Snapshot& s, Tick t0, Tick t1, int n) {
    double spt = samplesPerTick();
    for (size_t ci = 0; ci < s.clips.size(); ++ci) {
        auto& clip = s.clips[ci];
        auto& st = s.clipStates[ci];
        if (clip.type != ClipType::Audio || !clip.asset || clip.muted || clip.trackIdx < 0) {
            st.active = false;
            continue;
        }
        if (clip.end <= t0 || clip.start >= t1) {
            st.active = false;
            continue;
        }
        const AudioAsset* a = clip.asset.get();
        auto& t = s.tracks[clip.trackIdx];
        int64_t srcLen = std::max<int64_t>(1, clip.srcEnd - clip.srcStart);
        double posTicks = m_posTicks.load();
        // block-local start sample where the clip becomes active
        int startOff = clip.start > t0 ? clamp((int)std::floor((clip.start - posTicks) * spt), 0, n - 1) : 0;
        int endOff = clip.end < t1 ? clamp((int)std::ceil((clip.end - posTicks) * spt), 0, n) : n;
        if (!st.active || m_transportJumped) {   // the source position at this tick, counted from the clip's anchor (clipSourceSeconds)
            double at = posTicks + startOff / spt, anchor = (double)(clip.start - clip.sourceOffset);
            st.pos = clip.framesPerTick > 0 ? (at - anchor) * clip.framesPerTick : secondsBetween(anchor, at) * m_sr * clip.rate;
            st.active = true;
        }
        float g = clip.gain;
        double inc = clip.framesPerTick > 0 ? clip.framesPerTick / spt : clip.rate;   // beat-locked clips follow the tempo
        double clipLen = (double)(clip.end - clip.start), clipPos = posTicks - clip.start;   // fades are in ticks
        for (int i = startOff; i < endOff; ++i) {
            double p = st.pos;
            if (p >= srcLen) {
                if (!clip.loopSource) break;
                p = std::fmod(p, (double)srcLen);
                st.pos = p;
            }
            double rp = clip.reverse ? (srcLen - 1 - p) : p;
            double fr = clip.srcStart + rp;
            float l = dsp::readInterp(a->L(), a->frames, fr), r = dsp::readInterp(a->R(), a->frames, fr);
            float env = g;
            double cp = clipPos + i / spt;
            if (clip.fadeIn > 0 && cp < clip.fadeIn) env *= dsp::fadeShape((float)(cp / clip.fadeIn), clip.fadeInShape);
            if (clip.fadeOut > 0 && cp > clipLen - clip.fadeOut)
                env *= dsp::fadeShape((float)std::max(0.0, (clipLen - cp) / clip.fadeOut), clip.fadeOutShape);
            t.genL[i] += l * env;
            t.genR[i] += r * env;
            st.pos += inc;
        }
    }
}

// ---------------------------------------------------------------- RT: metronome & preview
void AudioEngine::renderMetronome(float* L, float* R, int n, Tick t0, Tick t1, TimeSignature ts, Tick barStart) {
    Tick beat = ts.ticksPerBeat();
    double spt = samplesPerTick();
    Tick firstBeat = barStart + (t0 - barStart + beat - 1) / beat * beat;   // beats count from the start of t0's bar
    for (Tick b = firstBeat; b < t1; b += beat) {
        int off = clamp((int)((b - t0) * spt), 0, n - 1);
        bool down = (b - barStart) % ts.ticksPerBar() == 0;
        // render the click from 'off' onwards
        m_clickEnv = 1.f;
        m_clickPhase = 0;
        m_clickFreq = down ? 1400.f : 1000.f;
        // note: the click starts at 'off'; for simplicity the tail of a previous click is overwritten
        for (int i = off; i < n; ++i) {
            float s = std::sin((float)(m_clickPhase * dsp::kTwoPi)) * m_clickEnv * 0.5f;
            m_clickPhase += m_clickFreq / m_sr;
            m_clickEnv *= 0.9985f;
            L[i] += s;
            R[i] += s;
        }
        return;   // one click per block is enough in practice (blocks are far shorter than a beat)
    }
    if (m_clickEnv > 0.001f)
        for (int i = 0; i < n; ++i) {
            float s = std::sin((float)(m_clickPhase * dsp::kTwoPi)) * m_clickEnv * 0.5f;
            m_clickPhase += m_clickFreq / m_sr;
            m_clickEnv *= 0.9985f;
            L[i] += s;
            R[i] += s;
        }
}
void AudioEngine::renderPreview(float* L, float* R, int n) {
    m_preview.rtTick();
    if (!m_previewPlaying.load()) return;
    const PreviewBuffer* b = m_preview.get();
    if (!b || b->failed.load()) {
        m_previewPlaying.store(false);
        return;
    }
    int64_t avail = b->available.load(std::memory_order_acquire);
    if (avail <= 0) {
        if (b->done.load()) m_previewPlaying.store(false);
        return;
    }   // still opening / decoding the first chunk
    if (b->firstAudioUs.load() < 0)
        const_cast<PreviewBuffer*>(b)->firstAudioUs.store(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    float g = m_previewGain.load();
    double rate = b->fileRate / m_sr;
    int64_t total = b->frames;
    for (int i = 0; i < n; ++i) {
        if (m_previewPosRt >= total - 1) {
            m_previewPlaying.store(false);
            break;
        }
        if (m_previewPosRt + 1 >= avail) {
            if (b->done.load()) { m_previewPlaying.store(false); }
            break;
        }   // data not decoded yet: wait (silence)
        L[i] += dsp::readInterp(b->L(), avail, m_previewPosRt) * g;
        R[i] += dsp::readInterp(b->R(), avail, m_previewPosRt) * g;
        m_previewPosRt += rate;
    }
    m_previewPos.store(m_previewPosRt);
}

}   // namespace mt
