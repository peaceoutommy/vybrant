// Persistent project model. This is the source of truth; the audio engine and UI both derive from it.
// All cross-references use stable ids (never indexes).
#pragma once
#include "core/Types.h"
#include <QString>
#include <QColor>
#include <QJsonObject>
#include <QByteArray>
#include <vector>
#include <cmath>
#include <map>

namespace mt {

// ---------------- Sequencing ----------------
struct Step {
    bool enabled = false;
    float velocity = 0.8f;
    float pan = 0.f;   // -1..1
    float pitch = 0.f;   // semitone offset
    float probability = 1.f;   // 0..1
    float offset = 0.f;   // -0.5..0.5 of a step
    bool operator==(const Step&) const = default;
};

struct Note {
    Id id = 0;
    int pitch = 60;
    Tick start = 0;
    Tick duration = PPQ / 4;
    float velocity = 0.8f;
    float releaseVelocity = 0.5f;
    float pan = 0.f;
    float finePitch = 0.f;   // cents
    int channel = 0;   // MIDI channel 0..15
    bool muted = false;
    bool operator==(const Note&) const = default;   // every field: an edit to any of them is an edit
};

struct PatternChannelData {
    Id id = 0;   // channel id
    std::vector<Note> notes;   // piano-roll notes
    std::vector<Step> steps;   // step-sequencer grid
};

struct Pattern {
    Id id = 0;
    QString name;
    QColor color = QColor(0x8f, 0x97, 0xa1);   // neutral grey by default; App::addPattern cycles a palette after that
    Tick length = 0;   // 0 = automatic (derived from content / step count)
    int stepsPerBeat = 4;   // step resolution (4 = 16ths, 2 = 8ths, 8 = 32nds, 3 = triplets)
    int stepCount = 16;   // steps visible in the rack
    float swing = 0.f;   // 0..1
    std::vector<PatternChannelData> channels;
    std::vector<Id> automationLaneIds;

    PatternChannelData* channelData(Id ch) {
        for (auto& c : channels)
            if (c.id == ch) return &c;
        return nullptr;
    }
    const PatternChannelData* channelData(Id ch) const {
        for (auto& c : channels)
            if (c.id == ch) return &c;
        return nullptr;
    }
    PatternChannelData& ensureChannel(Id ch) {
        if (auto* c = channelData(ch)) return *c;
        channels.push_back({ch, {}, std::vector<Step>(stepCount)});
        return channels.back();
    }
    Tick stepTicks() const { return (Tick)PPQ / stepsPerBeat; }
    Tick effectiveLength(const TimeSignature& ts) const;
};

// ---------------- Channels ----------------
enum class ChannelType : uint8_t { Sampler = 0, DrumSampler, Synth, MidiOut, AudioGenerator, Plugin };
// the channel type a native instrument key creates (every other native instrument is synth-like)
inline ChannelType channelTypeForKey(const QString& key) {
    return key == "native.sampler"   ? ChannelType::Sampler
        : key == "native.drumsampler" ? ChannelType::DrumSampler
        : key == "native.midiout"     ? ChannelType::MidiOut
        : key == "native.tonegen"     ? ChannelType::AudioGenerator
                                      : ChannelType::Synth;
}

struct Channel {
    Id id = 0;
    QString name;
    QColor color = QColor(0x8c, 0xd1, 0x7c);
    ChannelType type = ChannelType::Sampler;
    QString instrumentKey;   // native processor key ("native.synth") or "plugin"
    Id pluginInstanceId = 0;
    QString samplePath;   // sampler convenience reference
    float volume = 0.8f;   // 0..1 (linear)
    float pan = 0.f;
    bool mute = false, solo = false, enabled = true;
    bool cutItself = false;   // FL 'Cut itself': a new note on this channel cuts the notes it is still playing
    Id mixerTrackId = 0;   // 0 = master
    int rootNote = 60;
    int midiOutPort = -1, midiOutChannel = 0;
    QJsonObject instrumentState;   // saved native instrument state
    Id pluginId() const { return pluginInstanceId; }
};

// ---------------- Playlist ----------------
struct PlaylistTrack {
    Id id = 0;
    QString name;
    QColor color = QColor(0x55, 0x66, 0x77);
    int height = 60;
    bool mute = false, solo = false, locked = false, armed = false;
    int recordInput = -1;   // device input channel (-1 = none)
    Id mixerTrackId = 0;   // mixer destination for audio clips on this track (0 = master)
};

enum class ClipType : uint8_t { Pattern = 0, Audio, Automation, Midi };

struct Clip {
    Id id = 0;
    ClipType type = ClipType::Pattern;
    Id trackId = 0;
    Tick start = 0, length = PPQ * 4;
    Id sourceId = 0;   // patternId / audio source id / automation lane id
    bool muted = false;
    float gain = 1.f;
    Tick fadeIn = 0, fadeOut = 0;
    float fadeInShape = 0.f, fadeOutShape = 0.f;   // fade slope: 0 linear, >0 slow start (exponential), <0 fast start (logarithmic)
    double timeStretch = 1.0;   // playback ratio (1 = original); audio only
    float pitch = 0.f;   // semitones (audio only)
    Tick sourceOffset = 0;   // offset into the source
    bool reverse = false;
    bool loopSource = true;   // repeat pattern/audio when the clip is longer than the source
    QString name;
    int stretchMode = 0;   // audio: StretchMode (0 = resample: pitch, stretch and tempo fit all change the playback speed)
    bool fitTempo = false;   // audio: follow the project tempo, treating the source as sourceBpm
    double sourceBpm = 0;
    bool operator==(const Clip&) const = default;
};
// Audio clip playback. Length multiplier: the stretch knob times the tempo fit (an 83 BPM loop at 120 BPM: 83/120)
inline double clipTimeRatio(const Clip& c, double bpm) {
    return c.timeStretch * (c.fitTempo && c.sourceBpm > 0 && bpm > 0 ? c.sourceBpm / bpm : 1.0);
}
// Seconds of source audio per second of clip time. Stretch modes render pitch separately, so only the length counts.
inline double clipSourceSpeed(const Clip& c, double bpm) {
    double s = 1.0 / clipTimeRatio(c, bpm);
    return c.stretchMode == 0 ? s * std::pow(2.0, c.pitch / 12.0) : s;
}

// Non-destructive editing state of an audio file reference
struct AudioSource {
    Id id = 0;
    QString filePath;
    QString name;
    double sampleRate = 0;
    int channels = 0;
    int64_t frames = 0;
    bool reverse = false, normalize = false, loop = false;
    int64_t trimStart = 0, trimEnd = 0;   // frames (trimEnd 0 = end)
    float gain = 1.f;
    std::vector<int64_t> slices;   // frames
    bool missing = false;
    // clip settings (gain, fades, pitch, mode, tempo...) a new clip of this sample starts with; edited from the project panel
    // while the sample is not on the playlist
    bool hasPlaceDefaults = false;
    Clip placeDefaults;
};
// the clip-level settings the audio clip editor edits, copied between clips and a sample's placement defaults
inline void copyClipSettings(const Clip& from, Clip& to) {
    to.gain = from.gain;
    to.fadeIn = from.fadeIn;
    to.fadeOut = from.fadeOut;
    to.pitch = from.pitch;
    to.timeStretch = from.timeStretch;
    to.sourceOffset = from.sourceOffset;
    to.reverse = from.reverse;
    to.loopSource = from.loopSource;
    to.muted = from.muted;
    to.stretchMode = from.stretchMode;
    to.fitTempo = from.fitTempo;
    to.sourceBpm = from.sourceBpm;
}

// ---------------- Mixer ----------------
struct EffectSlot {
    Id id = 0;
    QString processorKey;
    Id pluginInstanceId = 0;
    bool bypass = false;
    float mix = 1.f;
    QJsonObject state;
    QString name;
};
struct Send {
    Id targetTrackId = 0;
    float level = 0.f;
    bool preFader = false;
    bool enabled = true;
    bool sidechain = false;
};
struct MixerTrack {
    Id id = 0;
    QString name;
    QColor color = QColor(0x66, 0x77, 0x88);
    float volume = 0.8f;
    float pan = 0.f;
    bool mute = false, solo = false, armed = false;
    int inputChannel = -1;   // hardware input index (-1 none)
    bool inputStereo = true;   // inputChannel and the next one as a pair; false = mono input on both sides
    Id outputTrackId = 0;   // destination track (0 = hardware output; only master)
    std::vector<EffectSlot> effects;
    std::vector<Send> sends;
    bool isMaster = false;
    bool customName = false;   // renamed by the user; otherwise named after the first channel routed here (App::syncMixerTrackNames)
};

// ---------------- Plugins ----------------
struct PluginInstanceData {
    Id id = 0;
    QString format;
    QString identifier;
    QString name;
    QString manufacturer;
    QString type;
    QString path;
    QByteArray state;
    QByteArray controllerState;
    int latency = 0;
    bool bypass = false;
    bool missing = false;
    bool crashed = false;
    std::map<uint32_t, double> paramCache;
};

// ---------------- Automation ----------------
struct ParamTarget {
    enum Kind : uint8_t { None = 0, ChannelVolume, ChannelPan, MixerVolume, MixerPan, MixerSend, EffectParam, InstrumentParam, Transport };
    Kind kind = None;
    Id ownerId = 0;
    Id slotId = 0;
    uint32_t paramId = 0;
    bool operator==(const ParamTarget& o) const {
        return kind == o.kind && ownerId == o.ownerId && slotId == o.slotId && paramId == o.paramId;
    }
};
struct AutomationPoint {
    Tick time = 0;
    float value = 0.f;
    CurveType curve = CurveType::Linear;
    float tension = 0.f;
};
float evalAutomation(const std::vector<AutomationPoint>& points, Tick t);   // allocation-free, RT-safe
struct AutomationLane {
    Id id = 0;
    QString name;
    QColor color = QColor(0xe0, 0x9a, 0x40);
    ParamTarget target;
    std::vector<AutomationPoint> points;
    Tick length = PPQ * 4;
    Id patternId = 0;
    float valueAt(Tick t) const;
};

struct Marker {
    Id id = 0;
    QString name;
    Tick position = 0;
};
struct MidiMapping {
    int cc = -1;
    int midiChannel = -1;
    ParamTarget target;
    QString name;
};

struct TransportSettings {
    double bpm = 140.0;
    TimeSignature timeSig;
    bool metronome = false, countIn = false, loopEnabled = false;
    Tick loopStart = 0, loopEnd = PPQ * 16;
    bool songMode = false;
    bool snap = true;
    int snapDivision = 4;   // 4 = 1/4 beat
    bool punchEnabled = false;
    Tick punchIn = 0, punchOut = PPQ * 16;
    bool loopRecord = false;
    int recordMode = 0;   // 0 = audio+notes, 1 = notes only, 2 = audio only, 3 = automation
    int countInBars = 1;
    Tick position = 0;
};

struct ProjectMetadata {
    QString title = "Untitled";
    QString author;
    QString comments;
    QString created;
    QString modified;
    QString genre;
};
struct ProjectSettings {
    double sampleRate = 44100;
    int bitDepth = 24;
    QString projectDir;
    QStringList tags;
};

struct Project {
    ProjectMetadata meta;
    TransportSettings transport;
    ProjectSettings settings;
    std::vector<Pattern> patterns;
    std::vector<Channel> channels;
    std::vector<PlaylistTrack> playlistTracks;
    std::vector<Clip> clips;
    std::vector<MixerTrack> mixerTracks;
    std::vector<PluginInstanceData> plugins;
    std::vector<AutomationLane> automationLanes;
    std::vector<AudioSource> audioSources;
    std::vector<Marker> markers;
    std::vector<MidiMapping> midiMappings;
    Id nextId = 1;
    Id selectedPatternId = 0;   // persisted UI convenience (which pattern the rack edits)
    Id selectedChannelId = 0;

    Id newId() { return nextId++; }

    template <class T> static T* findIn(std::vector<T>& v, Id id) {
        for (auto& e : v)
            if (e.id == id) return &e;
        return nullptr;
    }
    template <class T> static const T* findIn(const std::vector<T>& v, Id id) {
        for (auto& e : v)
            if (e.id == id) return &e;
        return nullptr;
    }
    Pattern* pattern(Id id) { return findIn(patterns, id); }
    const Pattern* pattern(Id id) const { return findIn(patterns, id); }
    Channel* channel(Id id) { return findIn(channels, id); }
    const Channel* channel(Id id) const { return findIn(channels, id); }
    PlaylistTrack* track(Id id) { return findIn(playlistTracks, id); }
    const PlaylistTrack* track(Id id) const { return findIn(playlistTracks, id); }
    Clip* clip(Id id) { return findIn(clips, id); }
    const Clip* clip(Id id) const { return findIn(clips, id); }
    MixerTrack* mixerTrack(Id id) { return findIn(mixerTracks, id); }
    const MixerTrack* mixerTrack(Id id) const { return findIn(mixerTracks, id); }
    PluginInstanceData* plugin(Id id) { return findIn(plugins, id); }
    const PluginInstanceData* plugin(Id id) const { return findIn(plugins, id); }
    AutomationLane* lane(Id id) { return findIn(automationLanes, id); }
    const AutomationLane* lane(Id id) const { return findIn(automationLanes, id); }
    AudioSource* audioSource(Id id) { return findIn(audioSources, id); }
    const AudioSource* audioSource(Id id) const { return findIn(audioSources, id); }
    MixerTrack* master() {
        for (auto& t : mixerTracks)
            if (t.isMaster) return &t;
        return nullptr;
    }
    Id masterId() const {
        for (auto& t : mixerTracks)
            if (t.isMaster) return t.id;
        return 0;
    }
    int mixerIndex(Id id) const {
        for (size_t i = 0; i < mixerTracks.size(); ++i)
            if (mixerTracks[i].id == id) return (int)i;
        return -1;
    }
    Tick clipLength(const Clip& c) const {
        if (c.type == ClipType::Pattern && c.length <= 0) {
            const Pattern* p = pattern(c.sourceId);
            return p ? p->effectiveLength(transport.timeSig) : PPQ * 4;
        }
        return c.length;
    }
    Tick songLength() const {
        Tick e = 0;
        for (auto& c : clips) e = std::max(e, c.start + clipLength(c));
        return e;
    }
    bool routingWouldCycle(Id from, Id to) const;   // true if routing from->to would create a cycle
};

// Flags describing which part of the project changed so observers can refresh selectively.
enum ChangeFlag : uint32_t {
    ChangeNone = 0,
    ChangePatterns = 1,
    ChangeChannels = 2,
    ChangePlaylist = 4,
    ChangeMixer = 8,
    ChangePlugins = 16,
    ChangeAutomation = 32,
    ChangeTransport = 64,
    ChangeAudio = 128,
    ChangeMeta = 256,
    ChangeStructure = 512,
    ChangeAll = 0xffffffff
};

Project makeDefaultProject(
    bool demo = false);   // demo = the 4 drums + synth with a beat and melody (tests / --demo); default = FL-style empty project

}   // namespace mt
