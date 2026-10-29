// Native effect library. Every effect exposes its parameters through ParameterSet so all are automatable.
#pragma once
#include "dsp/Processor.h"
#include "dsp/DspUtils.h"
#include "dsp/Fft.h"

namespace mt {

class EffectBase : public NativeProcessor {
public:
    ProcessorKind kind() const override { return ProcessorKind::Effect; }
};

#define MT_EFFECT(ClassName, KeyStr, NameStr)                                                                                              \
    class ClassName : public EffectBase {                                                                                                  \
    public:                                                                                                                                \
        ClassName();                                                                                                                       \
        QString key() const override { return KeyStr; }                                                                                    \
        QString displayName() const override { return NameStr; }                                                                           \
        void process(float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx) override;                            \
        void reset() override;                                                                                                             \
                                                                                                                                           \
    private:

// Tempo-sync helper: returns delay time in seconds for a sync choice index (0 = use ms)
double syncDivisionSeconds(int choice, double bpm);
extern const std::vector<std::string> kSyncChoices;

// ---------------- Dynamics ----------------
// What the dynamics editors draw, posted by process() once per block, in dB: the peaks since the editor last took them (it
// swaps them back to the floor), and where the detector sits on the transfer curve now (x = level in, y = level out)
struct DynamicsMeter {
    static constexpr float kFloor = -96.f;   // gainToDb(silence)
    std::atomic<float> in{kFloor}, out{kFloor}, gr{0}, x{kFloor}, y{kFloor};
    void post(float inDb, float outDb, float grDb, float xDb, float yDb) {
        constexpr auto rlx = std::memory_order_relaxed;
        if (inDb > in.load(rlx)) in.store(inDb, rlx);
        if (outDb > out.load(rlx)) out.store(outDb, rlx);
        if (std::fabs(grDb) > std::fabs(gr.load(rlx))) gr.store(grDb, rlx);   // the deepest change (a transient boost counts too)
        x.store(xDb, rlx);
        y.store(yDb, rlx);
    }
};

MT_EFFECT(Compressor, "native.compressor", "Compressor")
public:
enum P : uint32_t { pThreshold = 1, pRatio, pAttack, pRelease, pKnee, pMakeup, pSidechain, pMix, pDetector, pAutoMakeup };
// the static curve: gain change (dB) for a detector level 'over' dB above the threshold
static float gainDb(float over, float ratio, float knee) {
    if (knee > 0 && over > -knee / 2 && over < knee / 2) {
        float x = over + knee / 2;
        return (1.f / ratio - 1.f) * x * x / (2.f * knee);
    }
    return over > 0 ? over * (1.f / ratio - 1.f) : 0.f;
}
DynamicsMeter meter;

private:
dsp::EnvelopeFollower m_env;

public:
bool wantsSidechain() const override { return true; }
};

MT_EFFECT(Limiter, "native.limiter", "Limiter")
protected:
void onPrepare() override;

public:
enum P : uint32_t { pCeiling = 1, pRelease, pLookahead, pInputGain };
int latencySamples() const override { return m_latency; }
DynamicsMeter meter;

private:
dsp::DelayLine m_dl, m_dr;
float m_env = 0;
int m_latency = 0;
std::vector<float> m_peakWin;
size_t m_peakPos = 0;
}
;

MT_EFFECT(Gate, "native.gate", "Gate")
public:
enum P : uint32_t { pThreshold = 1, pAttack, pHold, pRelease, pRange, pSidechain, pHysteresis };
bool wantsSidechain() const override { return true; }
DynamicsMeter meter;

private:
dsp::EnvelopeFollower m_env;
float m_gain = 0;
int m_holdCounter = 0;
bool m_open = false;
}
;

MT_EFFECT(Expander, "native.expander", "Expander")
public:
enum P : uint32_t { pThreshold = 1, pRatio, pAttack, pRelease, pKnee, pRange };
// the static curve: gain change (dB) for a detector level 'under' dB below the threshold
static float gainDb(float under, float ratio, float knee, float range) {
    float gr = 0;
    if (under > 0) {
        gr = -under * (ratio - 1.f);
        if (knee > 0 && under < knee) gr *= under / knee;
    }
    return std::max(gr, range);
}
DynamicsMeter meter;

private:
dsp::EnvelopeFollower m_env;
}
;

MT_EFFECT(Clipper, "native.clipper", "Clipper")
protected:
void onPrepare() override;

public:
enum P : uint32_t { pThreshold = 1, pSoftness, pInput, pOutput, pOversample };
static float shape(float x, float th, float soft);   // the clip curve, linear gain in and out
DynamicsMeter meter;

private:
dsp::Biquad m_aaL, m_aaR;
}
;

MT_EFFECT(TransientProcessor, "native.transient", "Transient Processor")
public:
enum P : uint32_t { pAttack = 1, pSustain, pSpeed, pOutput };
DynamicsMeter meter;

private:
dsp::EnvelopeFollower m_fast, m_slow;
}
;

// ---------------- EQ / Filtering ----------------
// The signal at one point of an effect, for its editor's live spectrum: process() writes mono samples into a ring, the UI
// reads the latest ones and runs the FFT itself. Relaxed atomics: a read racing a write sees a stale sample, nothing worse.
struct AnalysisTap {
    static constexpr int kSize = 8192;   // power of two; the UI reads at most half of it
    std::atomic<float> ring[kSize];
    std::atomic<uint32_t> written{0};
    std::atomic<float> sampleRate{44100};
    void push(const float* L, const float* R, int n, double sr) {
        uint32_t w = written.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) ring[(w + i) & (kSize - 1)].store((L[i] + R[i]) * 0.5f, std::memory_order_relaxed);
        written.store(w + (uint32_t)n, std::memory_order_release);
        sampleRate.store((float)sr, std::memory_order_relaxed);
    }
};

MT_EFFECT(ParametricEQ, "native.parametriceq", "Parametric EQ")
protected:
void onPrepare() override;

public:
static constexpr int kBands = 6;
enum P : uint32_t { pBandBase = 10 /* + band*5: enabled, type, freq, gain, q */, pOutput = 1 };
static uint32_t bp(int band, int which) { return pBandBase + band * 5 + which; }
AnalysisTap tapIn, tapOut;

private:
dsp::Biquad m_l[kBands], m_r[kBands];
float m_lastCfg[kBands][4] = {};
void updateBand(int b);
}
;

MT_EFFECT(ShelvingEQ, "native.shelvingeq", "Shelving EQ")
public:
enum P : uint32_t { pLowFreq = 1, pLowGain, pHighFreq, pHighGain, pMidFreq, pMidGain, pMidQ };
AnalysisTap tapIn, tapOut;

private:
dsp::Biquad m_ll, m_lr, m_hl, m_hr, m_ml, m_mr;
}
;

class FilterEffect : public EffectBase {
public:
    enum P : uint32_t { pCutoff = 1, pResonance, pSlope, pMix };
    explicit FilterEffect(dsp::Biquad::Type type, QString key, QString name);
    QString key() const override { return m_key; }
    QString displayName() const override { return m_name; }
    void process(float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx) override;
    void reset() override;
    dsp::Biquad::Type type() const { return m_type; }
    AnalysisTap tapIn, tapOut;

private:
    dsp::Biquad::Type m_type;
    QString m_key, m_name;
    dsp::Biquad m_l[4], m_r[4];
};
struct HighPassFilter : FilterEffect {
    HighPassFilter()
        : FilterEffect(dsp::Biquad::HighPass, "native.highpass", "High-pass Filter") {}
};
struct LowPassFilter : FilterEffect {
    LowPassFilter()
        : FilterEffect(dsp::Biquad::LowPass, "native.lowpass", "Low-pass Filter") {}
};
struct BandPassFilter : FilterEffect {
    BandPassFilter()
        : FilterEffect(dsp::Biquad::BandPass, "native.bandpass", "Band-pass Filter") {}
};
struct NotchFilter : FilterEffect {
    NotchFilter()
        : FilterEffect(dsp::Biquad::Notch, "native.notch", "Notch Filter") {}
};

// ---------------- Time-based ----------------
// Seconds since the last input transient: the delay and reverb editors run a cursor from it along their time axis. Fed at the
// top of process() on the audio thread, read by the UI.
struct HitClock {
    std::atomic<float> since{1e9f};
    float env = 0;
    void feed(const float* L, const float* R, int n, double sr) {
        float pk = 0;
        for (int i = 0; i < n; ++i) pk = std::max(pk, std::max(std::abs(L[i]), std::abs(R[i])));
        float s = since.load(std::memory_order_relaxed) + (float)(n / sr);
        if (pk > 0.02f && pk > env * 1.6f) s = 0;
        env = std::max(pk, env * std::exp(-(float)(n / (sr * 0.15))));
        since.store(s, std::memory_order_relaxed);
    }
};

MT_EFFECT(Delay, "native.delay", "Delay")
protected:
void onPrepare() override;

public:
enum P : uint32_t { pTime = 1, pSync, pFeedback, pLowCut, pHighCut, pMix, pStereoOffset, pModRate, pModDepth };
int tailSamples() const override { return (int)(m_sr * 4); }
float sinceHit() const { return m_hit.since.load(std::memory_order_relaxed); }

private:
HitClock m_hit;
dsp::DelayLine m_l, m_r;
dsp::Biquad m_lcL, m_lcR, m_hcL, m_hcR;
dsp::LFO m_lfo;
Smoother m_timeS;
}
;

MT_EFFECT(PingPongDelay, "native.pingpong", "Ping-Pong Delay")
protected:
void onPrepare() override;

public:
enum P : uint32_t { pTime = 1, pSync, pFeedback, pLowCut, pHighCut, pMix, pWidth };
int tailSamples() const override { return (int)(m_sr * 4); }
float sinceHit() const { return m_hit.since.load(std::memory_order_relaxed); }

private:
HitClock m_hit;
dsp::DelayLine m_l, m_r;
dsp::Biquad m_lcL, m_lcR, m_hcL, m_hcR;
Smoother m_timeS;
}
;

MT_EFFECT(Reverb, "native.reverb", "Reverb")
protected:
void onPrepare() override;

public:
enum P : uint32_t { pSize = 1, pDamping, pWidth, pPreDelay, pMix, pLowCut, pHighCut, pDecay };
int tailSamples() const override { return (int)(m_sr * 6); }
float sinceHit() const { return m_hit.since.load(std::memory_order_relaxed); }

private:
HitClock m_hit;
static constexpr int kCombs = 8, kAllpass = 4;
struct Comb {
    std::vector<float> buf;
    size_t idx = 0;
    float filt = 0;
};
Comb m_combL[kCombs], m_combR[kCombs];
std::vector<float> m_apL[kAllpass], m_apR[kAllpass];
size_t m_apIdx[2][kAllpass] = {};
dsp::DelayLine m_pre;
dsp::Biquad m_lc, m_hcL, m_hcR;
dsp::DelayLine m_preR;
}
;

MT_EFFECT(Echo, "native.echo", "Echo")
protected:
void onPrepare() override;

public:
enum P : uint32_t { pTime = 1, pSync, pFeedback, pTaps, pSpread, pDamp, pMix, pDecayShape };
int tailSamples() const override { return (int)(m_sr * 4); }
float sinceHit() const { return m_hit.since.load(std::memory_order_relaxed); }

private:
HitClock m_hit;
dsp::DelayLine m_l, m_r;
dsp::Biquad m_dampL, m_dampR;
}
;

// ---------------- Modulation ----------------
MT_EFFECT(Chorus, "native.chorus", "Chorus")
protected:
void onPrepare() override;

public:
enum P : uint32_t { pRate = 1, pDepth, pDelay, pVoices, pMix, pFeedback, pWidth };
float lfoPhase(int voice) const { return m_uiPhase[voice].load(std::memory_order_relaxed); }   // 0..1, for the editor

private:
dsp::DelayLine m_l, m_r;
dsp::LFO m_lfo[4];
std::atomic<float> m_uiPhase[4] = {};
}
;

MT_EFFECT(Flanger, "native.flanger", "Flanger")
protected:
void onPrepare() override;

public:
enum P : uint32_t { pRate = 1, pDepth, pDelay, pFeedback, pMix, pPhase, pSync };
float lfoPhase() const { return m_uiPhase.load(std::memory_order_relaxed); }   // 0..1, for the editor

private:
dsp::DelayLine m_l, m_r;
dsp::LFO m_lfo;
std::atomic<float> m_uiPhase{0};
float m_fbL = 0, m_fbR = 0;
}
;

MT_EFFECT(Phaser, "native.phaser", "Phaser")
public:
enum P : uint32_t { pRate = 1, pDepth, pStages, pFeedback, pCenter, pMix, pSpread, pSync };
float lfoPhase() const { return m_uiPhase.load(std::memory_order_relaxed); }   // 0..1, for the editor

private:
dsp::Biquad m_apL[12], m_apR[12];
dsp::LFO m_lfo;
std::atomic<float> m_uiPhase{0};
float m_fbL = 0, m_fbR = 0;
}
;

MT_EFFECT(Tremolo, "native.tremolo", "Tremolo")
public:
enum P : uint32_t { pRate = 1, pDepth, pShape, pSync, pStereoPhase, pSmooth };
float lfoPhase() const { return m_uiPhase.load(std::memory_order_relaxed); }   // 0..1, for the editor
float lfoHeld() const { return m_uiHeld.load(std::memory_order_relaxed); }   // the S&H shape's current value

private:
dsp::LFO m_lfo;
std::atomic<float> m_uiPhase{0}, m_uiHeld{0};
double m_phaseR = 0;
}
;

MT_EFFECT(Vibrato, "native.vibrato", "Vibrato")
protected:
void onPrepare() override;

public:
enum P : uint32_t { pRate = 1, pDepth, pShape, pSync, pStereoPhase };
float lfoPhase() const { return m_uiPhase.load(std::memory_order_relaxed); }   // 0..1, for the editor
float lfoHeld() const { return m_uiHeld.load(std::memory_order_relaxed); }   // the S&H shape's current value

private:
dsp::DelayLine m_l, m_r;
dsp::LFO m_lfo;
std::atomic<float> m_uiPhase{0}, m_uiHeld{0};
}
;

// ---------------- Distortion ----------------
// The newest input and output samples, for an editor's scope: the audio thread writes them, the UI copies them, no locks
struct StereoTap {
    static constexpr uint32_t kSize = 4096;   // a power of two
    std::atomic<float> buf[4][kSize];   // input L, input R, output L, output R
    std::atomic<uint32_t> pos{0};   // samples written so far (wraps)
    void push(float inL, float inR, float outL, float outR) {
        const uint32_t p = pos.load(std::memory_order_relaxed), i = p & (kSize - 1);
        buf[0][i].store(inL, std::memory_order_relaxed);
        buf[1][i].store(inR, std::memory_order_relaxed);
        buf[2][i].store(outL, std::memory_order_relaxed);
        buf[3][i].store(outR, std::memory_order_relaxed);
        pos.store(p + 1, std::memory_order_release);
    }
};

MT_EFFECT(Saturation, "native.saturation", "Saturation")
public:
enum P : uint32_t { pDrive = 1, pTone, pMix, pOutput, pType, pBias };
float transfer(float x) const;   // input -> output at the current settings, filters left out (the editor's curve)
StereoTap tap;

private:
dsp::Biquad m_toneL, m_toneR;
dsp::DcBlocker m_dcL, m_dcR;
}
;

MT_EFFECT(Waveshaper, "native.waveshaper", "Waveshaper")
public:
enum P : uint32_t { pCurve = 1, pAmount, pMix, pInput, pOutput, pSymmetry };
float transfer(float x) const;
StereoTap tap;

private:
dsp::DcBlocker m_dcL, m_dcR;
}
;

MT_EFFECT(BitCrusher, "native.bitcrusher", "Bit Crusher")
public:
enum P : uint32_t { pBits = 1, pDownsample, pMix, pDither, pJitter };
StereoTap tap;

private:
float m_holdL = 0, m_holdR = 0;
float m_counter = 0;
dsp::Rng m_rng;
}
;

MT_EFFECT(Distortion, "native.distortion", "Distortion")
public:
enum P : uint32_t { pDrive = 1, pType, pTone, pLevel, pMix, pLowCut };
float transfer(float x) const;
StereoTap tap;

private:
dsp::Biquad m_toneL, m_toneR, m_lcL, m_lcR;
dsp::DcBlocker m_dcL, m_dcR;
}
;

// ---------------- Utility ----------------
MT_EFFECT(GainUtility, "native.gain", "Gain")
protected:
void onPrepare() override;

public:
enum P : uint32_t { pGain = 1, pPan, pBalance, pMute };
StereoTap tap;

private:
Smoother m_g, m_p;
}
;

MT_EFFECT(StereoWidth, "native.stereowidth", "Stereo Width")
public:
enum P : uint32_t { pWidth = 1, pMonoBelow, pBassMonoFreq, pMidGain, pSideGain };
StereoTap tap;

private:
dsp::Biquad m_lpL, m_lpR, m_hpL, m_hpR;
}
;

MT_EFFECT(PhaseInvert, "native.phaseinvert", "Phase Inversion")
public:
enum P : uint32_t { pLeft = 1, pRight, pSwap };
StereoTap tap;
}
;

MT_EFFECT(MonoConversion, "native.mono", "Mono Conversion")
public:
enum P : uint32_t { pMode = 1, pMix };
StereoTap tap;
}
;

MT_EFFECT(SpectrumAnalyzer, "native.spectrum", "Spectrum Analyzer")
protected:
void onPrepare() override;

public:
static constexpr int kFft = 2048;
enum P : uint32_t { pDecay = 1, pRange, pSmooth };
// UI reads the latest magnitude spectrum (dB) - kFft/2 bins
void readSpectrum(std::vector<float>& out) const;

private:
std::vector<float> m_ring;
size_t m_ringPos = 0;
std::vector<float> m_window, m_in;
dsp::Fft m_fft{kFft};
std::vector<std::complex<float>> m_out;   // empty until prepared
std::vector<float> m_mag[2];
std::atomic<int> m_front{0};
int m_sinceFft = 0;
}
;

MT_EFFECT(MeterEffect, "native.meter", "Peak / RMS Meter")
public:
enum P : uint32_t { pHold = 1, pRmsWindow };
struct Levels {
    float peakL, peakR, rmsL, rmsR, holdL, holdR;
};
// UI: the peaks since the previous call (so a 30 Hz meter misses no block), the RMS and the held peaks
Levels levels() { return {m_pkL.exchange(0), m_pkR.exchange(0), m_rmsL.load(), m_rmsR.load(), m_holdL.load(), m_holdR.load()}; }

private:
std::atomic<float> m_pkL{0}, m_pkR{0}, m_rmsL{0}, m_rmsR{0}, m_holdL{0}, m_holdR{0};
float m_sqL = 0, m_sqR = 0;
float m_hl = 0, m_hr = 0;
int m_holdCount = 0;
}
;

#undef MT_EFFECT

}   // namespace mt
