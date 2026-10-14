// Runtime (real-time) state derived from the Project. A Snapshot is an immutable graph built on the UI
// thread and handed to the audio thread through a lock-free command; the audio thread never touches Project.
#pragma once
#include "core/Types.h"
#include "core/RingBuffer.h"
#include "dsp/Processor.h"
#include "dsp/DspUtils.h"
#include "audio/AudioFile.h"
#include "project/Project.h"
#include <atomic>
#include <memory>
#include <vector>
#include <map>

namespace mt {

// Per-track meter values written by the RT thread, read by the UI at a controlled rate.
struct MeterSlot {
    std::atomic<float> peakL{0}, peakR{0}, rmsL{0}, rmsR{0};
    std::atomic<bool> clipped{false};
};

// Persistent (across snapshots) controls for mixer tracks and channels so parameter changes are immediate.
struct TrackControls {
    std::atomic<float> volume{0.8f}, pan{0.f};
    std::atomic<bool> mute{false}, solo{false};
    MeterSlot meter;
    dsp::PdcDelay generatorDelay;
    std::map<Id, std::unique_ptr<std::atomic<float>>> sendLevels;
    std::map<Id, std::unique_ptr<dsp::PdcDelay>> sendDelays;
};
struct ChannelControls {
    std::atomic<float> volume{0.8f}, pan{0.f};
    std::atomic<bool> mute{false}, solo{false};
    MeterSlot meter;
};

// A resolved automation / MIDI-mapping target: either a processor parameter or a direct atomic.
struct RtParamTarget {
    IProcessor* proc = nullptr;
    uint32_t paramId = 0;
    std::atomic<float>* direct = nullptr;
    float min = 0, max = 1;
    inline void setNormalized(float n) const {
        if (proc) proc->params().setNormalized(paramId, n);
        else if (direct) direct->store(min + clamp(n, 0.f, 1.f) * (max - min), std::memory_order_relaxed);
    }
    inline float getNormalized() const {
        if (proc) return proc->params().getNormalized(paramId);
        if (direct) return (direct->load() - min) / (max - min);
        return 0;
    }
    bool valid() const { return proc || direct; }
};

struct RtNote {
    int pitch;
    Tick start;
    Tick end;
    float velocity;
    float pan;
    float finePitch;
    float probability;
    int midiChannel;
};
struct RtControl {
    Tick tick;
    int controller;
    MidiEvent event;   // CC / pitch bend / channel pressure; the sample offset is set when scheduled
};
struct RtPatternChannel {
    int channelIndex;
    std::vector<RtNote> notes;
    std::vector<RtControl> controls;   // sorted by tick
};
struct RtPattern {
    Id id;
    Tick length;
    std::vector<RtPatternChannel> channels;
    std::vector<int> laneIndices;
    float swing = 0;
};

struct RtAutomationLane {
    Id id;
    RtParamTarget target;
    std::vector<AutomationPoint> points;
    Tick length;
    float valueAt(Tick t) const;
};

struct RtClip {
    ClipType type;
    Tick start, end;
    int trackIdx = -1;
    int patternIdx = -1;
    int laneIdx = -1;
    Tick sourceOffset = 0;
    bool loopSource = true;
    // audio
    AudioAssetPtr asset;
    int64_t srcStart = 0, srcEnd = 0;
    float gain = 1.f;
    bool reverse = false;
    double rate = 1.0;
    double framesPerTick = 0;   // fit tempo: the source is locked to beats at this speed; 0 = plays at its own speed in seconds
    Tick fadeIn = 0, fadeOut = 0;
    float fadeInShape = 0, fadeOutShape = 0;
    bool muted = false;
};
struct RtClipState {
    bool active = false;
    double pos = 0;
};

// Pending note-offs scheduled from note durations (fixed capacity, RT-safe)
struct NoteOffScheduler {
    struct Pending {
        int samplesLeft;
        uint8_t pitch;
        uint8_t ch;
        int noteId;
        bool used;
    };
    static constexpr int kCap = 512;
    Pending items[kCap] = {};
    int count = 0;
    void add(int samples, uint8_t pitch, uint8_t ch, int noteId) {
        for (auto& p : items)
            if (!p.used) {
                p = {samples, pitch, ch, noteId, true};
                return;
            }
    }
    void clear() {
        for (auto& p : items) p.used = false;
    }
    // Emit note-offs falling within the next n samples into 'out', then advance
    void advance(int n, MidiEventList& out) {
        for (auto& p : items) {
            if (!p.used) continue;
            if (p.samplesLeft < n) {
                MidiEvent e;
                e.type = MidiEvent::NoteOff;
                e.data1 = p.pitch;
                e.channel = p.ch;
                e.sampleOffset = std::max(0, p.samplesLeft);
                e.noteId = p.noteId;
                out.add(e);
                p.used = false;
            } else p.samplesLeft -= n;
        }
    }
};

struct RtChannel {
    Id id;
    IProcessor* instrument = nullptr;
    int mixerTrackIdx = 0;
    ChannelControls* ctl = nullptr;
    bool enabled = true;
    bool audible = true;
    bool cutItself = false;
    int rootNote = 60;
    bool isMidiOut = false;
    int midiOutPort = -1;
    MidiEventList midi;
    NoteOffScheduler noteOffs;
    std::vector<float> bufL, bufR;
    std::vector<int> auxTrackIdx;   // multi-out: the mixer track index for each extra output pair (-1 = off)
    std::vector<int> patternLanes;
    dsp::PdcDelay* pdc = nullptr;
};
struct RtEffect {
    IProcessor* proc = nullptr;
    bool bypass = false;
    float mix = 1.f;
    Id slotId = 0;
    std::atomic<bool>* crashed = nullptr;
};
struct RtSend {
    int targetIdx;
    std::atomic<float>* level;
    bool pre;
    bool sidechain;
    dsp::PdcDelay* delay = nullptr;
};
struct RtTrack {
    Id id;
    TrackControls* ctl = nullptr;
    std::vector<RtEffect> effects;
    std::vector<RtSend> sends;
    int outputIdx = -1;
    bool isMaster = false;
    int inputChannel = -1;
    bool audible = true;
    bool armed = false;
    bool frozen = false;
    std::vector<float> inL, inR, scL, scR, genL, genR;
    int chainLatency = 0;
    int pathLatency = 0;
    FloatRing* recordRing = nullptr;
    int recordChannels = 1;
    bool monitor = true;
};
struct RtMidiMapping {
    uint8_t cc;
    int8_t midiChannel;
    RtParamTarget target;
    bool isNote = false;
};

struct Snapshot {
    uint64_t generation = 0;
    std::vector<RtPattern> patterns;
    std::vector<RtChannel> channels;
    std::vector<RtTrack> tracks;
    std::vector<int> trackOrder;
    std::vector<RtAutomationLane> lanes;
    std::vector<RtClip> clips;
    std::vector<RtClipState> clipStates;
    std::vector<RtMidiMapping> midiMappings;
    std::vector<int> songLanes;   // lane indices active in song mode via clips (resolved per block)
    int selectedPatternIdx = -1;
    int liveChannelIdx = -1;
    Tick songEnd = 0;
    TempoMap tempo;
    int maxBlock = 512;
    int maxLatency = 0;
    std::vector<AudioAssetPtr> heldAssets;   // keep assets alive for the lifetime of the snapshot
    std::vector<float> tmpL, tmpR;
};

// Commands from UI -> RT
struct EngineCommand {
    enum Type : uint8_t { None, SetSnapshot, Play, Stop, Pause, Seek, SetRecording, AllNotesOff, PreviewPlay, PreviewStop, CountIn, Panic };
    Type type = None;
    Snapshot* snapshot = nullptr;
    double value = 0;
    int64_t ival = 0;
};
// Messages RT -> UI
struct EngineMessage {
    // RecordSegment: tick + sample (recorded frame)
    enum Type : uint8_t { None, SnapshotRetired, RecordedMidi, PluginCrashed, RecordSegment };
    Type type = None;
    Snapshot* snapshot = nullptr;
    MidiEvent midi;
    double tick = 0;
    Id id = 0;
    int64_t sample = 0;
};

}   // namespace mt
