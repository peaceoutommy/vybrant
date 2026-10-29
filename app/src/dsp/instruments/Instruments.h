// Native instruments. All share IProcessor so the engine treats them exactly like hosted plugins.
#pragma once
#include "dsp/Processor.h"
#include "dsp/DspUtils.h"
#include "audio/AudioFile.h"
#include <array>

namespace mt {

// Lock-free handoff of an immutable object from the UI thread to the RT thread.
template <class T> struct SwapHolder {
    std::atomic<const T*> current{nullptr};
    std::vector<std::shared_ptr<const T>> held;   // UI-thread owned
    std::atomic<int> rtTicks{0};
    void set(std::shared_ptr<const T> p) {
        held.push_back(p);
        current.store(p.get(), std::memory_order_release);
        rtTicks.store(0);
    }
    void clear() { set(nullptr); }
    inline void rtTick() { rtTicks.fetch_add(1, std::memory_order_relaxed); }
    void prune(bool force = false) {
        if (held.size() <= 1) return;
        if (!force && rtTicks.load() < 2) return;
        const T* cur = current.load();
        held.erase(std::remove_if(held.begin(), held.end(), [&](auto& p) { return p.get() != cur; }), held.end());
    }
    inline const T* get() const { return current.load(std::memory_order_acquire); }
    std::shared_ptr<const T> shared() const {
        const T* c = current.load();
        for (auto& h : held)
            if (h.get() == c) return h;
        return nullptr;
    }
};

// Instrument base with polyphonic voice management helpers
class InstrumentBase : public NativeProcessor {
public:
    ProcessorKind kind() const override { return ProcessorKind::Instrument; }
    void collectGarbage(bool force) {
        for (auto* h : m_holders) h->prune(force);
    }

protected:
    // Sample-accurate MIDI: hands every event due at frame i to onEvent (call once per frame; mi starts at 0 each block)
    template <class F> static void eventsAt(const MidiEventList& midi, int& mi, int i, F&& onEvent) {
        while (mi < midi.count && midi.events[mi].sampleOffset <= i) onEvent(midi.events[mi++]);
    }
    // A free voice, else the first one that 'worse' ranks below all others (oldest, quietest, furthest along): the one to steal
    template <class V, size_t N, class Worse> static V* pickVoice(std::array<V, N>& voices, Worse worse) {
        for (auto& v : voices)
            if (!v.active) return &v;
        V* w = &voices[0];
        for (auto& v : voices)
            if (worse(v, *w)) w = &v;
        return w;
    }
    std::vector<SwapHolder<AudioAsset>*> m_holders;
    float m_bend = 0.f;   // semitones from pitch wheel
    float m_modWheel = 0.f;
};

// ---------------- Sampler ----------------
class Sampler : public InstrumentBase {
public:
    enum P : uint32_t {
        pRoot = 1,
        pAttack,
        pDecay,
        pSustain,
        pRelease,
        pStart,
        pLoopMode,
        pTune,
        pFine,
        pCutoff,
        pReso,
        pFilterType,
        pReverse,
        pLevel,
        pPan,
        pVelSens,
        pPitchBendRange,
        pLoopStart,
        pLoopEnd,
        pGlide,
        pEnvOn,
        pDeclick,
        pStretchMode,   // StretchMode: resample, or a Rubber Band stretch rendered by App (setStretched)
        pTime,   // length factor
        pFitTempo,
        pSourceBpm
    };
    // Declicking modes (after FL Studio's Sampler): fade-out length when a voice is cut, fade-in length at the start; 'transient' only ramps the first few samples in
    static void declickTimes(int mode, float& inSec, float& outSec, bool& transientIn);
    Sampler();
    QString key() const override { return "native.sampler"; }
    QString displayName() const override { return "Sampler"; }
    void process(float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx) override;
    void reset() override;
    QJsonObject saveState() const override;
    void restoreState(const QJsonObject& s) override;
    void setSample(AudioAssetPtr a, const QString& path) {
        m_sample.set(a);
        m_path = path;
    }
    QString samplePath() const { return m_path; }
    AudioAssetPtr sample() const { return m_sample.shared(); }
    // UI thread: the sample rendered for the stretch modes (Tune, Time and tempo fit applied); key identifies the settings
    void setStretched(AudioAssetPtr a, const QString& key) {
        m_stretched.set(a);
        m_stretchKey = key;
    }
    QString stretchKey() const { return m_stretchKey; }
    // Length factor from Time and the tempo fit (an 83 BPM loop at 120 BPM: 83/120)
    double timeRatio(double bpm) const {
        double src = m_params.get(pSourceBpm);
        return m_params.get(pTime) * (m_params.getBool(pFitTempo) && src > 0 && bpm > 0 ? src / bpm : 1.0);
    }
    QString pendingPath;   // path recorded in state that still needs loading (resolved by the registry)
private:
    struct Voice {
        bool active = false;
        int note = 0;
        double pos = 0;
        double inc = 0;
        float vel = 1;
        float pan = 0;
        dsp::ADSR env;
        dsp::SVF fl, fr;
        int dir = 1;
        float glideFrom = 0;
        float glideT = 1;
        int noteId = -1;
        int fadeInLen = 0, fadeInPos = 0;
        int fadeOutLen = 0, fadeOutPos = -1;
    };   // declick ramps (cosine S-shape); fadeOutPos >= 0 while the voice is being cut
    void cutVoice(Voice& v);   // stop a voice through the declick fade-out
    std::array<Voice, 32> m_voices;
    SwapHolder<AudioAsset> m_sample;
    SwapHolder<AudioAsset> m_stretched;
    QString m_path, m_stretchKey;
    // what the voices play: the rendered stretch when a stretch mode has one ready, otherwise the sample (resampled)
    const AudioAsset* playAsset() const {
        const AudioAsset* s = m_params.getInt(pStretchMode) != 0 ? m_stretched.get() : nullptr;
        return s ? s : m_sample.get();
    }
    void noteOn(const MidiEvent& e);
    void noteOff(const MidiEvent& e);
    int m_lastNote = -1;
};

// ---------------- Drum Sampler (16 pads) ----------------
class DrumSampler : public InstrumentBase {
public:
    static constexpr int kPads = 16;
    enum P : uint32_t {
        pLevel = 1,
        pRoot,
        pAttack,
        pDecay,
        pRelease,
        pPitch,
        pCutoff,
        pVelSens,
        pPan,
        pPadBase = 100 /* + pad*8: level, pan, pitch, decay, choke, reverse, start, tune */
    };
    DrumSampler();
    QString key() const override { return "native.drumsampler"; }
    QString displayName() const override { return "Drum Sampler"; }
    void process(float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx) override;
    void reset() override;
    QJsonObject saveState() const override;
    void restoreState(const QJsonObject& s) override;
    void setPadSample(int pad, AudioAssetPtr a, const QString& path) {
        m_pads[pad].sample.set(a);
        m_pads[pad].path = path;
    }
    QString padPath(int pad) const { return m_pads[pad].path; }
    AudioAssetPtr padSample(int pad) const { return m_pads[pad].sample.shared(); }
    void setBuiltin(const QString& name);   // synthesize a default drum sound
    QString builtinName() const { return m_builtin; }
    static std::shared_ptr<AudioAsset> synthesizeDrum(const QString& name, double sr);
    static uint32_t padParam(int pad, int which) { return pPadBase + pad * 8 + which; }

protected:
    void onPrepare() override;

private:
    struct Pad {
        SwapHolder<AudioAsset> sample;
        QString path;
    };
    struct Voice {
        bool active = false;
        int pad = -1;
        int note = 0;
        double pos = 0;
        double inc = 0;
        float vel = 1;
        dsp::ADSR env;
        float pan = 0;
        dsp::Biquad fl, fr;
    };
    std::array<Pad, kPads> m_pads;
    std::array<Voice, 32> m_voices;
    QString m_builtin;
    void trigger(const MidiEvent& e);
};

// ---------------- Subtractive Synth ----------------
class Synth : public InstrumentBase {
public:
    enum P : uint32_t {
        pOsc1Wave = 1,
        pOsc1Level,
        pOsc1Octave,
        pOsc1Semi,
        pOsc1Fine,
        pOsc1PW,
        pOsc2Wave,
        pOsc2Level,
        pOsc2Octave,
        pOsc2Semi,
        pOsc2Fine,
        pOsc2PW,
        pUnison,
        pDetune,
        pNoise,
        pSub,
        pFilterType,
        pCutoff,
        pReso,
        pFilterEnv,
        pFilterKeyTrack,
        pFA,
        pFD,
        pFS,
        pFR,
        pAA,
        pAD,
        pAS,
        pAR,
        pLfoRate,
        pLfoShape,
        pLfoPitch,
        pLfoFilter,
        pLfoSync,
        pGlide,
        pLevel,
        pVoices,
        pMono,
        pSpread,
        pBendRange,
        pDrive
    };
    Synth();
    QString key() const override { return "native.synth"; }
    QString displayName() const override { return "Synth"; }
    void process(float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx) override;
    void reset() override;

private:
    struct Voice {
        bool active = false;
        int note = 0;
        float vel = 1;
        dsp::Oscillator o1[8], o2[8], sub;
        dsp::Oscillator noise;
        dsp::ADSR amp, fenv;
        dsp::SVF fl, fr;
        float freq = 440;
        float glideFrom = 440;
        float glideT = 1;
        int64_t age = 0;
    };
    std::array<Voice, 16> m_voices;
    dsp::LFO m_lfo;
    int m_lastNote = -1;
    int64_t m_counter = 0;
    void noteOn(const MidiEvent& e);
    void noteOff(const MidiEvent& e);
    Voice* allocVoice();
};

// ---------------- Wavetable Synth ----------------
class WavetableSynth : public InstrumentBase {
public:
    static constexpr int kTableSize = 2048;
    static constexpr int kTables = 8;
    enum P : uint32_t {
        pPosition = 1,
        pPosEnv,
        pPosLfo,
        pOctave,
        pSemi,
        pFine,
        pUnison,
        pDetune,
        pFilterType,
        pCutoff,
        pReso,
        pFilterEnv,
        pFA,
        pFD,
        pFS,
        pFR,
        pAA,
        pAD,
        pAS,
        pAR,
        pLfoRate,
        pLfoShape,
        pLevel,
        pBank,
        pGlide,
        pSpread
    };
    WavetableSynth();
    QString key() const override { return "native.wavetable"; }
    QString displayName() const override { return "Wavetable"; }
    void process(float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx) override;
    void reset() override;
    const float* table(int bank, int idx) const { return m_tables[bank][idx].data(); }

private:
    struct Voice {
        bool active = false;
        int note = 0;
        float vel = 1;
        double phase[8] = {0};
        dsp::ADSR amp, fenv;
        dsp::SVF fl, fr;
        float freq = 440;
        float glideFrom = 440;
        float glideT = 1;
        int64_t age = 0;
    };
    std::array<Voice, 16> m_voices;
    dsp::LFO m_lfo;
    int64_t m_counter = 0;
    int m_lastNote = -1;
    std::vector<std::vector<float>> m_tables[3];   // 3 banks of kTables tables
    void buildTables();
    inline float readTable(int bank, float pos, double phase) const;
};

// ---------------- FM Synth (4 operators) ----------------
class FMSynth : public InstrumentBase {
public:
    enum P : uint32_t {
        pAlgorithm = 1,
        pFeedback,
        pLevel,
        pAA,
        pAD,
        pAS,
        pAR,
        pLfoRate,
        pLfoDepth,
        pGlide,
        pBendRange,
        pOpBase = 100 /* + op*8: ratio, level, detune, A, D, S, R, fixed */
    };
    FMSynth();
    QString key() const override { return "native.fm"; }
    QString displayName() const override { return "FM Synth"; }
    void process(float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx) override;
    void reset() override;
    static uint32_t opParam(int op, int which) { return pOpBase + op * 8 + which; }

private:
    struct Voice {
        bool active = false;
        int note = 0;
        float vel = 1;
        double ph[4] = {0};
        dsp::ADSR env[4];
        dsp::ADSR amp;
        float freq = 440;
        float fb = 0;
        int64_t age = 0;
    };
    std::array<Voice, 16> m_voices;
    dsp::LFO m_lfo;
    int64_t m_counter = 0;
};

// ---------------- Tone / noise generator (audio generator channel) ----------------
class ToneGenerator : public InstrumentBase {
public:
    enum P : uint32_t { pWave = 1, pFreq, pLevel, pGate, pAttack, pRelease };
    ToneGenerator();
    QString key() const override { return "native.tonegen"; }
    QString displayName() const override { return "Tone Generator"; }
    void process(float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx) override;

private:
    dsp::Oscillator m_osc;
    dsp::ADSR m_env;
    int m_held = 0;
    float m_freqOverride = 0;

protected:
    void onPrepare() override { m_env.set(m_sr, 0.01f, 0.1f, 1.f, 0.1f); }
};

// ---------------- MIDI out (no audio; the engine forwards events to the MIDI engine) ----------------
class MidiOutInstrument : public InstrumentBase {
public:
    enum P : uint32_t { pPort = 1, pChannel, pProgram, pBank };
    MidiOutInstrument();
    QString key() const override { return "native.midiout"; }
    QString displayName() const override { return "MIDI Out"; }
    void process(float*, float*, int, const MidiEventList& midi, const ProcessContext&) override;
    const MidiEventList* outputMidi() const override { return &m_out; }

private:
    MidiEventList m_out;
};

}   // namespace mt
