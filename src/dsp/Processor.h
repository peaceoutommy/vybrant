// IProcessor: the single interface the engine uses for native instruments, native effects and hosted plugins.
// process() is called on the real-time thread and must be lock-free / allocation-free.
#pragma once
#include "core/Parameter.h"
#include <QJsonObject>
#include <QString>
#include <cstdint>
#include <atomic>

class QWidget;

namespace mt {

struct MidiEvent {
    enum Type : uint8_t {
        NoteOn = 0,
        NoteOff,
        CC,
        PitchBend,
        ChannelPressure,
        PolyPressure,
        ProgramChange,
        AllNotesOff,
        Clock,
        Start,
        Stop,
        Continue
    };
    Type type = NoteOn;
    uint8_t channel = 0;
    uint8_t data1 = 0;   // note / cc number
    uint8_t data2 = 0;   // raw velocity / cc value (0..127)
    int sampleOffset = 0;
    float velocity = 1.f;   // 0..1 (note on/off)
    float pan = 0.f;   // per-note pan (-1..1)
    float finePitch = 0.f;   // cents
    int noteId = -1;
    int pitchBend = 0;   // -8192..8191
};

struct MidiEventList {
    static constexpr int kCapacity = 1024;
    MidiEvent events[kCapacity];
    int count = 0;
    void clear() { count = 0; }
    bool add(const MidiEvent& e) {
        if (count >= kCapacity) return false;
        events[count++] = e;
        return true;
    }
};

struct ProcessContext {
    double sampleRate = 44100;
    int blockSize = 512;
    double bpm = 120;
    int tsNum = 4, tsDen = 4;
    int64_t positionSamples = 0;
    double positionBeats = 0;
    bool playing = false;
    bool recording = false;
    bool looping = false;
    double loopStartBeats = 0, loopEndBeats = 0;
    const float* sidechainL = nullptr;
    const float* sidechainR = nullptr;   // optional external sidechain input
};

enum class ProcessorKind : uint8_t { Instrument, Effect, MidiEffect };

class IProcessor {
public:
    virtual ~IProcessor() = default;
    virtual QString key() const = 0;   // e.g. "native.compressor" or "vst3:<uid>"
    virtual QString displayName() const = 0;
    virtual ProcessorKind kind() const = 0;
    virtual void prepare(double sampleRate, int maxBlockSize) = 0;
    virtual void reset() {}   // clear voices / delay memory (RT)
    // Audio: 'buffers' is a stereo in-place pair (effects read+write, instruments write). numFrames <= maxBlockSize.
    virtual void process(float* left, float* right, int numFrames, const MidiEventList& midi, const ProcessContext& ctx) = 0;
    virtual ParameterSet& params() = 0;
    virtual int latencySamples() const { return 0; }
    virtual int tailSamples() const { return 0; }
    virtual QJsonObject saveState() const;   // default: parameters
    virtual void restoreState(const QJsonObject& s);   // default: parameters
    virtual QWidget* createEditor(QWidget* parent) { return nullptr; }   // nullptr => generic parameter editor
    virtual bool hasCustomEditor() const { return false; }
    virtual bool isPlugin() const { return false; }
    // Sidechain-capable effects opt in
    virtual bool wantsSidechain() const { return false; }
    // Bypass handled by the engine, but some processors need to know (e.g. to flush)
    virtual void setActive(bool) {}
    // Produced MIDI (plugins may emit) - optional
    virtual const MidiEventList* outputMidi() const { return nullptr; }
    // Latency changed since last query (plugins) - engine polls
    virtual bool latencyChanged() { return false; }
    // Human-readable value for the UI (UI thread only); default formats using the ParamDesc
    virtual QString formatValue(uint32_t paramId) const;
    // Plugins: raw persistent identity
    virtual QString pluginFormat() const { return QString(); }
};

// Base with parameter bookkeeping used by all native processors.
class NativeProcessor : public IProcessor {
public:
    ParameterSet& params() override { return m_params; }
    void prepare(double sr, int maxBlock) override {
        m_sr = sr;
        m_maxBlock = maxBlock;
        onPrepare();
    }

protected:
    virtual void onPrepare() { reset(); }   // a fresh start at the new rate; override when buffers must be (re)sized
    double m_sr = 44100;
    int m_maxBlock = 512;
    ParameterSet m_params;
};

// The UI installs a factory that builds custom editors for native processors (keeps DSP free of UI code).
using NativeEditorFactory = QWidget* (*)(IProcessor*, QWidget*);
NativeEditorFactory& nativeEditorFactory();

}   // namespace mt
