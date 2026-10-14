// Real-time audio engine. Owns the runtime registry (processors + persistent controls), the transport clock and
// the active Snapshot. The UI talks to it through lock-free commands and atomics; it never blocks the RT thread.
#pragma once
#include "audio/Runtime.h"
#include "audio/AudioDeviceManager.h"
#include "audio/AudioFile.h"
#include "dsp/instruments/Instruments.h"
#include "audio/PreviewPlayer.h"
#include <QObject>
#include <QTimer>
#include <functional>
#include <map>
#include <atomic>
#include <deque>

namespace mt {

class RecordingEngine;

struct TimedMidiOut {
    MidiEvent event;
    int port;
    int64_t sampleTime;
};

// UI-thread owned registry of runtime objects that survive snapshot rebuilds.
class RuntimeRegistry {
public:
    using PluginFactory = std::function<std::unique_ptr<IProcessor>(const PluginInstanceData&)>;
    IProcessor* processor(Id ownerId) const {
        auto it = m_procs.find(ownerId);
        return it == m_procs.end() ? nullptr : it->second.get();
    }
    IProcessor* ensureInstrument(const Channel& ch, const Project& p, double sr, int maxBlock, const PluginFactory& pf, uint64_t gen);
    IProcessor* ensureEffect(const EffectSlot& slot, const Project& p, double sr, int maxBlock, const PluginFactory& pf, uint64_t gen);
    IProcessor* replaceProcessor(Id ownerId, std::unique_ptr<IProcessor> proc, double sr, int maxBlock, uint64_t gen);
    TrackControls& track(Id id) {
        auto& t = m_tracks[id];
        if (!t) t = std::make_unique<TrackControls>();
        return *t;
    }
    ChannelControls& channel(Id id) {
        auto& c = m_channels[id];
        if (!c) c = std::make_unique<ChannelControls>();
        return *c;
    }
    TrackControls* trackIfExists(Id id) const {
        auto it = m_tracks.find(id);
        return it == m_tracks.end() ? nullptr : it->second.get();
    }
    ChannelControls* channelIfExists(Id id) const {
        auto it = m_channels.find(id);
        return it == m_channels.end() ? nullptr : it->second.get();
    }
    std::atomic<bool>& crashFlag(Id pluginId) {
        auto& f = m_crash[pluginId];
        if (!f) f = std::make_unique<std::atomic<bool>>(false);
        return *f;
    }
    void retireUnused(const Project& p, uint64_t gen);   // move processors not referenced by the project to the graveyard
    void collectGarbage(uint64_t retiredGen, bool engineIdle);
    void clearAll();
    void prepareAll(double sr, int maxBlock);
    void syncStateToProject(Project& p) const;
    const std::map<Id, std::unique_ptr<IProcessor>>& processors() const { return m_procs; }
    std::vector<std::pair<Id, IProcessor*>> pluginProcessors() const;

private:
    std::map<Id, std::unique_ptr<IProcessor>> m_procs;
    std::map<Id, std::unique_ptr<TrackControls>> m_tracks;
    std::map<Id, std::unique_ptr<ChannelControls>> m_channels;
    std::map<Id, std::unique_ptr<std::atomic<bool>>> m_crash;
    std::vector<std::pair<uint64_t, std::unique_ptr<IProcessor>>> m_graveyard;
};

struct TransportView {
    bool playing = false, recording = false, paused = false, countingIn = false;
    double positionTicks = 0;
    double bpm = 120;
    int tsNum = 4, tsDen = 4;
    bool songMode = false;
    bool loop = false;
    Tick loopStart = 0, loopEnd = 0;
    int bar = 1, beat = 1, tick = 0;
};

class AudioEngine : public QObject {
    Q_OBJECT
public:
    AudioEngine(AudioDeviceManager& devices, SampleCache& cache, QObject* parent = nullptr);
    ~AudioEngine();

    bool startDevice(QString* error = nullptr);
    void stopDevice();
    bool deviceRunning() const { return m_devices.isOpen(); }
    double sampleRate() const { return m_sr; }
    int maxBlockSize() const { return m_maxBlock; }
    RuntimeRegistry& registry() { return m_registry; }
    void setPluginFactory(RuntimeRegistry::PluginFactory f) { m_pluginFactory = std::move(f); }
    void setRecordingEngine(RecordingEngine* r) { m_recording = r; }

    // ---- Project -> runtime ----
    void rebuild(const Project& p);   // build & send a new snapshot (UI thread)
    void requestRebuild() { m_rebuildPending = true; }   // coalesced
    void unloadProject();   // drop all runtime objects (blocking, UI thread)
    void syncStateToProject(Project& p) { m_registry.syncStateToProject(p); }

    // ---- Transport (UI thread) ----
    void play();
    void stop();
    void pause();
    void togglePlay();
    void seek(Tick t);
    void setRecording(bool r);
    void setBpm(double b) { m_bpm.store(b); }
    void setTimeSig(int n, int d) {
        m_tsNum.store(n);
        m_tsDen.store(d);
    }
    void setLoop(bool on, Tick s, Tick e) {
        m_loopStart.store(s);
        m_loopEnd.store(e);
        m_loopEnabled.store(on);
    }
    void setSongMode(bool s) { m_songMode.store(s); }
    void setLoopRecord(bool on) { m_loopRecord.store(on); }   // FL: off, recording plays through the song loop instead of repeating it
    void setMetronome(bool m) { m_metronome.store(m); }
    void setCountIn(bool c, int bars) {
        m_countIn.store(c);
        m_countInBars.store(bars);
    }
    void setMonitorInputs(bool m) { m_monitorInputs.store(m); }
    void setMidiClockOut(bool on) { m_midiClock.store(on); }
    void setPunch(bool on, Tick in, Tick out) {
        m_punchIn.store(in);
        m_punchOut.store(out);
        m_punch.store(on);
    }
    void setSelectedPattern(Id id) { m_selectedPattern.store(id); }
    // Offline stems / freeze: output this track (before or after its fader) instead of the master
    void setRenderTap(Id mixerTrackId, bool preFader = false) {
        m_renderTap.store(mixerTrackId);
        m_renderTapPre.store(preFader);
    }
    int renderLatency() const;   // offline: frames the graph delays the timeline by at the render output (the tap or the master)
    TransportView transport() const;
    double positionTicks() const { return m_posTicks.load(); }
    bool isPlaying() const { return m_playing.load(); }
    bool isRecording() const { return m_recordingFlag.load(); }

    // ---- Live MIDI ----
    void pushUiMidi(const MidiEvent& e) { m_uiMidi.push(e); }
    SpscQueue<MidiEvent>& midiInQueue() { return m_midiIn; }   // fed by the MIDI engine thread
    SpscQueue<TimedMidiOut>& midiOutQueue() { return m_midiOut; }   // drained by the MIDI engine thread
    void allNotesOff();

    // ---- Preview (independent from the transport) ----
    void previewPlay(std::shared_ptr<PreviewBuffer> b);
    void previewStop();
    void previewSeek(double frame);   // restart playback from a frame (also resumes a stopped preview)
    bool previewPlaying() const { return m_previewPlaying.load(); }
    double previewPosition() const { return m_previewPos.load(); }
    std::shared_ptr<const PreviewBuffer> previewBuffer() const { return m_preview.shared(); }

    // ---- Stats ----
    float cpuLoad() const { return m_cpuLoad.load(); }
    int xruns() const { return m_devices.xruns(); }
    int totalLatencySamples() const { return m_maxLatency.load(); }

    // ---- Offline rendering (device must be stopped) ----
    void offlineBegin(double sampleRate, int blockSize);
    void offlineProcess(float* outL, float* outR, int n, const float* in = nullptr, int inCh = 0);
    void offlineEnd();

    // Pump: drains RT->UI messages. Called from a UI timer.
    void poll();

Q_SIGNALS:
    void recordedMidi(const MidiEvent& e, double tick);
    void pluginCrashed(Id pluginInstanceId);
    void transportStateChanged();
    // Recorded input (one continuous run of frames per pass) restarts at 'tick' from recorded frame 'frame': at the start, after
    // a count-in, punch-in, loop wrap or seek. The frame counter never resets, so a pass's first segment is its frame 0.
    void recordSegment(double tick, int64_t frame);

private:
    void rtCallback(const float* in, float* out, int frames, int inCh, int outCh, bool xrun);
    void processFrames(const float* in, int inCh, float* outL, float* outR, int frames);
    void processSubBlock(const float* in, int inCh, float* outL, float* outR, int n, Snapshot* snap, Tick t0, Tick t1);
    void runSequencer(Snapshot& s, Tick t0, Tick t1, int n);
    void scheduleNotes(Snapshot& s, const RtPattern& pat, Tick winStart, Tick winEnd, Tick tickOffset, int n, Tick t0, Tick t1);
    void renderAudioClips(Snapshot& s, Tick t0, Tick t1, int n);
    void routeLiveMidi(Snapshot& s, Tick t0);
    void drainCommands();
    void releaseAllNotes(bool resetInstruments);   // RT: drop scheduled note-offs and send every channel AllNotesOff
    void processTrack(Snapshot& s, RtTrack& t, int n, const ProcessContext& ctx, float* outL, float* outR);
    bool guardedProcess(RtEffect& fx, float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx);
    void renderMetronome(float* L, float* R, int n, Tick t0, Tick t1, bool countIn);
    void renderPreview(float* L, float* R, int n);
    void onAssetLoaded(const QString& path);
    void resolvePendingSamples();
    double samplesPerTick() const { return m_sr * 60.0 / (m_bpm.load() * PPQ); }

    AudioDeviceManager& m_devices;
    SampleCache& m_cache;
    RuntimeRegistry m_registry;
    RuntimeRegistry::PluginFactory m_pluginFactory;
    RecordingEngine* m_recording = nullptr;
    double m_sr = 44100;
    int m_maxBlock = 512;
    bool m_offline = false;
    double m_savedSr = 44100;
    int m_savedBlock = 512;
    // command / message queues
    SpscQueue<EngineCommand> m_commands{256};
    SpscQueue<EngineMessage> m_messages{4096};
    SpscQueue<MidiEvent> m_uiMidi{1024};
    SpscQueue<MidiEvent> m_midiIn{2048};
    SpscQueue<TimedMidiOut> m_midiOut{4096};
    // snapshot
    Snapshot* m_rtSnapshot = nullptr;
    std::atomic<uint64_t> m_activeGen{0};
    uint64_t m_nextGen = 1;
    std::vector<std::unique_ptr<Snapshot>> m_inFlight;
    bool m_rebuildPending = false;
    const Project* m_lastProject = nullptr;
    // transport atomics
    std::atomic<bool> m_playing{false}, m_recordingFlag{false}, m_paused{false}, m_countingIn{false};
    std::atomic<double> m_posTicks{0};
    std::atomic<double> m_bpm{140};
    std::atomic<int> m_tsNum{4}, m_tsDen{4};
    std::atomic<bool> m_loopEnabled{false}, m_loopRecord{false}, m_songMode{false}, m_metronome{false}, m_countIn{false}, m_monitorInputs{true};
    std::atomic<Tick> m_loopStart{0}, m_loopEnd{PPQ * 16};
    std::atomic<int> m_countInBars{1};
    std::atomic<bool> m_punch{false}, m_renderTapPre{false};
    std::atomic<bool> m_midiClock{false};
    double m_clockAccum = 0;
    bool m_clockRunning = false;
    std::atomic<Tick> m_punchIn{0}, m_punchOut{0};
    std::atomic<Id> m_selectedPattern{0}, m_renderTap{0};
    std::atomic<Tick> m_patternLength{PPQ * 4};
    double m_countInRemaining = 0;
    int64_t m_lastBeatIndex = -1;
    bool m_transportJumped = true;
    int64_t m_recFrames = 0;   // RT: input frames written to the record rings so far
    bool m_recWriting = false;   // RT: the previous block was recorded
    // metronome
    double m_clickPhase = 0;
    float m_clickEnv = 0;
    float m_clickFreq = 1000;
    // preview
    SwapHolder<PreviewBuffer> m_preview;
    std::atomic<bool> m_previewPlaying{false};
    std::atomic<double> m_previewPos{0};
    std::atomic<float> m_previewGain{0.8f};
    double m_previewPosRt = 0;
    // stats
    std::atomic<float> m_cpuLoad{0};
    std::atomic<int> m_maxLatency{0};
    std::atomic<int64_t> m_samplesProcessed{0};
    std::vector<float> m_outL, m_outR, m_dryL, m_dryR;
    dsp::Rng m_rng;
    int m_nextNoteId = 0;   // VST3 note ids must be unique per sounding note (chords share a start tick)
    MidiEventList m_liveDeferred[2];   // live note events held back to the next block (see routeLiveMidi)
    int m_liveDeferredCur = 0;
    QTimer m_pollTimer;
    std::vector<std::pair<IProcessor*, QString>> m_waitingSamples;   // processors waiting for a sample path to load
};

// Guarded call support for plugin crash isolation (see AudioEngine.cpp)
void installCrashGuard();

}   // namespace mt
