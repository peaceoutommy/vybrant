#include "dsp/effects/Effects.h"

namespace mt {
using namespace dsp;

const std::vector<std::string> kSyncChoices = {
    "Off (ms)", "1/32", "1/16", "1/16T", "1/16D", "1/8", "1/8T", "1/8D", "1/4", "1/4T", "1/4D", "1/2", "1/2D", "1 bar", "2 bars"};
double syncDivisionSeconds(int c, double bpm) {
    double beat = 60.0 / std::max(bpm, 1.0);
    static const double mult[] = {0, 0.125, 0.25, 0.25 * 2 / 3, 0.25 * 1.5, 0.5, 0.5 * 2 / 3, 0.75, 1, 2.0 / 3, 1.5, 2, 3, 4, 8};
    if (c <= 0 || c >= (int)(sizeof(mult) / sizeof(mult[0]))) return 0;
    return beat * mult[c];
}
static inline void mixDry(float& out, float dry, float mix) { out = dry + (out - dry) * mix; }
static inline float sidechainSample(const ProcessContext& ctx, int i, float fallback) {
    return ctx.sidechainL ? (ctx.sidechainL[i] + (ctx.sidechainR ? ctx.sidechainR[i] : ctx.sidechainL[i])) * 0.5f : fallback;
}

// ============================================================ Compressor
Compressor::Compressor() {
    auto& p = m_params;
    p.add(pThreshold, "Threshold", -60, 0, -18, ParamUnit::Decibel);
    p.add(pRatio, "Ratio", 1, 20, 4, ParamUnit::Ratio);
    p.add(pAttack, "Attack", 0.1f, 200, 10, ParamUnit::Milliseconds, true);
    p.add(pRelease, "Release", 5, 2000, 100, ParamUnit::Milliseconds, true);
    p.add(pKnee, "Knee", 0, 24, 6, ParamUnit::Decibel);
    p.add(pMakeup, "Makeup", 0, 24, 0, ParamUnit::Decibel);
    p.addToggle(pSidechain, "Ext. Sidechain");
    p.add(pMix, "Mix", 0, 1, 1, ParamUnit::Percent);
    p.addChoice(pDetector, "Detector", {"Peak", "RMS"});
    p.addToggle(pAutoMakeup, "Auto Makeup");
}
void Compressor::reset() { m_env.env = 0; }
void Compressor::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext& ctx) {
    auto& p = m_params;
    float th = p.get(pThreshold), ratio = p.get(pRatio), knee = p.get(pKnee), mix = p.get(pMix);
    bool sc = p.getBool(pSidechain) && ctx.sidechainL;
    bool rms = p.getInt(pDetector) == 1;
    float makeup = p.getBool(pAutoMakeup) ? -th * (1.f - 1.f / ratio) * 0.5f : p.get(pMakeup);
    float mg = dbToGain(makeup);
    m_env.set(m_sr, p.get(pAttack), p.get(pRelease));
    float minGr = 0;
    for (int i = 0; i < n; ++i) {
        float det = sc ? sidechainSample(ctx, i, 0) : std::max(std::fabs(L[i]), std::fabs(R[i]));
        if (rms) det = det * det;
        float env = m_env.process(det);
        if (rms) env = std::sqrt(env);
        float db = gainToDb(env);
        float over = db - th;
        float gr;
        if (knee > 0 && over > -knee / 2 && over < knee / 2) {
            float x = over + knee / 2;
            gr = (1.f / ratio - 1.f) * x * x / (2.f * knee);
        } else if (over > 0) gr = over * (1.f / ratio - 1.f);
        else gr = 0;
        minGr = std::min(minGr, gr);
        float g = dbToGain(gr) * mg;
        float dl = L[i], dr = R[i];
        L[i] *= g;
        R[i] *= g;
        mixDry(L[i], dl, mix);
        mixDry(R[i], dr, mix);
    }
    m_gr.store(minGr);
}

// ============================================================ Limiter (lookahead brickwall)
Limiter::Limiter() {
    auto& p = m_params;
    p.add(pCeiling, "Ceiling", -24, 0, -0.3f, ParamUnit::Decibel);
    p.add(pRelease, "Release", 1, 1000, 50, ParamUnit::Milliseconds, true);
    p.add(pLookahead, "Lookahead", 0, 10, 2, ParamUnit::Milliseconds);
    p.add(pInputGain, "Input Gain", -12, 24, 0, ParamUnit::Decibel);
}
void Limiter::onPrepare() {
    m_dl.allocate((size_t)(m_sr * 0.02) + 64);
    m_dr.allocate((size_t)(m_sr * 0.02) + 64);
    m_latency = (int)(m_params.get(pLookahead) * 0.001 * m_sr);
    m_peakWin.assign(std::max(1, m_latency), 0.f);
    reset();
}
void Limiter::reset() {
    m_dl.clear();
    m_dr.clear();
    m_env = 0;
    std::fill(m_peakWin.begin(), m_peakWin.end(), 0.f);
}
void Limiter::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext&) {
    auto& p = m_params;
    float ceil = dbToGain(p.get(pCeiling));
    float ing = dbToGain(p.get(pInputGain));
    int la = (int)(p.get(pLookahead) * 0.001 * m_sr);
    if (la != m_latency) {
        m_latency = la;
        m_peakWin.assign(std::max(1, la), 0.f);
        m_peakPos = 0;
    }   // note: size change only happens when user edits param (rare); vector kept at prepare size in practice
    float relCoeff = std::exp(-1.f / (float)(m_sr * p.get(pRelease) * 0.001f));
    float minGr = 0;
    for (int i = 0; i < n; ++i) {
        float l = L[i] * ing, r = R[i] * ing;
        float pk = std::max(std::fabs(l), std::fabs(r));
        m_dl.push(l);
        m_dr.push(r);
        // lookahead: the required gain is driven by the max peak within the window
        float need = pk > ceil ? ceil / pk : 1.f;
        if (m_latency > 0) {
            m_peakWin[m_peakPos] = need;
            m_peakPos = (m_peakPos + 1) % m_peakWin.size();
            float mn = 1.f;
            for (float v : m_peakWin) mn = std::min(mn, v);
            need = mn;
        }
        if (need < m_env || m_env == 0) m_env = need;
        else m_env = relCoeff * m_env + (1.f - relCoeff) * need;
        float g = std::min(m_env, 1.f);
        minGr = std::min(minGr, gainToDb(g));
        float ol = m_latency > 0 ? m_dl.readInt(m_latency) : l, orr = m_latency > 0 ? m_dr.readInt(m_latency) : r;
        L[i] = clamp(ol * g, -ceil, ceil);
        R[i] = clamp(orr * g, -ceil, ceil);
    }
    m_gr.store(minGr);
}

// ============================================================ Gate
Gate::Gate() {
    auto& p = m_params;
    p.add(pThreshold, "Threshold", -80, 0, -40, ParamUnit::Decibel);
    p.add(pAttack, "Attack", 0.1f, 100, 1, ParamUnit::Milliseconds, true);
    p.add(pHold, "Hold", 0, 500, 20, ParamUnit::Milliseconds);
    p.add(pRelease, "Release", 1, 2000, 80, ParamUnit::Milliseconds, true);
    p.add(pRange, "Range", -96, 0, -96, ParamUnit::Decibel);
    p.addToggle(pSidechain, "Ext. Sidechain");
    p.add(pHysteresis, "Hysteresis", 0, 12, 3, ParamUnit::Decibel);
}
void Gate::reset() {
    m_env.env = 0;
    m_gain = 0;
    m_open = false;
    m_holdCounter = 0;
}
void Gate::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext& ctx) {
    auto& p = m_params;
    float th = p.get(pThreshold), hyst = p.get(pHysteresis);
    float range = dbToGain(p.get(pRange));
    bool sc = p.getBool(pSidechain) && ctx.sidechainL;
    m_env.set(m_sr, 0.05f, 5.f);
    int hold = (int)(p.get(pHold) * 0.001 * m_sr);
    float att = 1.f - std::exp(-1.f / (float)(m_sr * p.get(pAttack) * 0.001f)),
          rel = 1.f - std::exp(-1.f / (float)(m_sr * p.get(pRelease) * 0.001f));
    for (int i = 0; i < n; ++i) {
        float det = sc ? sidechainSample(ctx, i, 0) : std::max(std::fabs(L[i]), std::fabs(R[i]));
        float db = gainToDb(m_env.process(det));
        if (!m_open && db > th) {
            m_open = true;
            m_holdCounter = hold;
        } else if (m_open && db < th - hyst) {
            if (m_holdCounter > 0) m_holdCounter--;
            else m_open = false;
        }
        float target = m_open ? 1.f : range;
        m_gain += (target - m_gain) * (target > m_gain ? att : rel);
        L[i] *= m_gain;
        R[i] *= m_gain;
    }
}

// ============================================================ Expander
Expander::Expander() {
    auto& p = m_params;
    p.add(pThreshold, "Threshold", -80, 0, -40, ParamUnit::Decibel);
    p.add(pRatio, "Ratio", 1, 10, 2, ParamUnit::Ratio);
    p.add(pAttack, "Attack", 0.1f, 200, 5, ParamUnit::Milliseconds, true);
    p.add(pRelease, "Release", 5, 2000, 150, ParamUnit::Milliseconds, true);
    p.add(pKnee, "Knee", 0, 24, 6, ParamUnit::Decibel);
    p.add(pRange, "Range", -60, 0, -40, ParamUnit::Decibel);
}
void Expander::reset() { m_env.env = 0; }
void Expander::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext&) {
    auto& p = m_params;
    float th = p.get(pThreshold), ratio = p.get(pRatio), knee = p.get(pKnee), range = p.get(pRange);
    m_env.set(m_sr, p.get(pAttack), p.get(pRelease));
    for (int i = 0; i < n; ++i) {
        float db = gainToDb(m_env.process(std::max(std::fabs(L[i]), std::fabs(R[i]))));
        float under = th - db;
        float gr = 0;
        if (under > 0) {
            gr = -under * (ratio - 1.f);
            if (knee > 0 && under < knee) gr *= under / knee;
        }
        gr = std::max(gr, range);
        float g = dbToGain(gr);
        L[i] *= g;
        R[i] *= g;
    }
}

// ============================================================ Clipper
Clipper::Clipper() {
    auto& p = m_params;
    p.add(pThreshold, "Threshold", -24, 0, -0.1f, ParamUnit::Decibel);
    p.add(pSoftness, "Softness", 0, 1, 0.2f);
    p.add(pInput, "Input", -12, 24, 0, ParamUnit::Decibel);
    p.add(pOutput, "Output", -24, 12, 0, ParamUnit::Decibel);
    p.addToggle(pOversample, "Oversample x2", true);
}
void Clipper::onPrepare() {
    m_aaL.set(Biquad::LowPass, m_sr * 2, (float)(m_sr * 0.45), 0.7f);
    m_aaR = m_aaL;
    reset();
}
void Clipper::reset() {
    m_aaL.reset();
    m_aaR.reset();
}
static inline float clipSample(float x, float th, float soft) {
    float a = std::fabs(x);
    if (soft <= 0.001f) return clamp(x, -th, th);
    float kneeStart = th * (1.f - soft);
    if (a <= kneeStart) return x;
    float t = (a - kneeStart) / (th - kneeStart + 1e-9f);
    float y = kneeStart + (th - kneeStart) * fastTanh(t);
    return x < 0 ? -y : y;
}
void Clipper::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext&) {
    auto& p = m_params;
    float th = dbToGain(p.get(pThreshold)), soft = p.get(pSoftness), ig = dbToGain(p.get(pInput)), og = dbToGain(p.get(pOutput));
    bool os = p.getBool(pOversample);
    for (int i = 0; i < n; ++i) {
        float l = L[i] * ig, r = R[i] * ig;
        if (os) {   // naive 2x: process sample and zero-stuffed midpoint through AA filter
            float l1 = m_aaL.process(clipSample(l, th, soft));
            float l2 = m_aaL.process(clipSample(l, th, soft));
            float r1 = m_aaR.process(clipSample(r, th, soft));
            float r2 = m_aaR.process(clipSample(r, th, soft));
            l = (l1 + l2) * 0.5f;
            r = (r1 + r2) * 0.5f;
        } else {
            l = clipSample(l, th, soft);
            r = clipSample(r, th, soft);
        }
        L[i] = l * og;
        R[i] = r * og;
    }
}

// ============================================================ Transient processor
TransientProcessor::TransientProcessor() {
    auto& p = m_params;
    p.add(pAttack, "Attack", -1, 1, 0);
    p.add(pSustain, "Sustain", -1, 1, 0);
    p.add(pSpeed, "Speed", 0.5f, 10, 2, ParamUnit::Milliseconds);
    p.add(pOutput, "Output", -24, 12, 0, ParamUnit::Decibel);
}
void TransientProcessor::reset() {
    m_fast.env = 0;
    m_slow.env = 0;
}
void TransientProcessor::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext&) {
    auto& p = m_params;
    float att = p.get(pAttack), sus = p.get(pSustain), og = dbToGain(p.get(pOutput));
    float spd = p.get(pSpeed);
    m_fast.set(m_sr, spd, 30);
    m_slow.set(m_sr, spd * 8, 150);
    for (int i = 0; i < n; ++i) {
        float x = std::max(std::fabs(L[i]), std::fabs(R[i]));
        float f = m_fast.process(x), s = m_slow.process(x);
        float diff = gainToDb(f + 1e-6f) - gainToDb(s + 1e-6f);   // >0 during transient
        float g = diff > 0 ? dbToGain(diff * att * 1.5f) : dbToGain(-diff * sus * 0.5f);
        g = clamp(g, 0.05f, 8.f);
        L[i] *= g * og;
        R[i] *= g * og;
    }
}

// ============================================================ Parametric EQ
ParametricEQ::ParametricEQ() {
    auto& p = m_params;
    p.add(pOutput, "Output", -24, 24, 0, ParamUnit::Decibel);
    const float defFreq[kBands] = {60, 200, 600, 1500, 4000, 10000};
    const int defType[kBands] = {5, 4, 4, 4, 4, 6};
    for (int b = 0; b < kBands; ++b) {
        std::string nm = "Band " + std::to_string(b + 1) + " ";
        p.addToggle(bp(b, 0), (nm + "On").c_str(), b == 0 || b == kBands - 1 ? false : false);
        p.addChoice(bp(b, 1), (nm + "Type").c_str(), {"Low-pass", "High-pass", "Band-pass", "Notch", "Peak", "Low Shelf", "High Shelf"},
            defType[b]);
        p.add(bp(b, 2), (nm + "Freq").c_str(), 20, 20000, defFreq[b], ParamUnit::Hertz, true);
        p.add(bp(b, 3), (nm + "Gain").c_str(), -24, 24, 0, ParamUnit::Decibel);
        p.add(bp(b, 4), (nm + "Q").c_str(), 0.1f, 18, 1, ParamUnit::Generic, true);
    }
}
void ParametricEQ::onPrepare() {
    reset();
    for (int b = 0; b < kBands; ++b) {
        m_lastCfg[b][0] = -1;
        updateBand(b);
    }
}
void ParametricEQ::reset() {
    for (int b = 0; b < kBands; ++b) {
        m_l[b].reset();
        m_r[b].reset();
    }
}
void ParametricEQ::updateBand(int b) {
    auto& p = m_params;
    float t = p.get(bp(b, 1)), f = p.get(bp(b, 2)), g = p.get(bp(b, 3)), q = p.get(bp(b, 4));
    if (t == m_lastCfg[b][0] && f == m_lastCfg[b][1] && g == m_lastCfg[b][2] && q == m_lastCfg[b][3]) return;
    m_lastCfg[b][0] = t;
    m_lastCfg[b][1] = f;
    m_lastCfg[b][2] = g;
    m_lastCfg[b][3] = q;
    m_l[b].set((Biquad::Type)(int)t, m_sr, f, q, g);
    m_r[b] = m_l[b];
    m_r[b].reset();
}
void ParametricEQ::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext&) {
    auto& p = m_params;
    bool on[kBands];
    for (int b = 0; b < kBands; ++b) {
        on[b] = p.getBool(bp(b, 0));
        if (on[b]) updateBand(b);
    }
    float og = dbToGain(p.get(pOutput));
    for (int i = 0; i < n; ++i) {
        float l = L[i], r = R[i];
        for (int b = 0; b < kBands; ++b)
            if (on[b]) {
                l = m_l[b].process(l);
                r = m_r[b].process(r);
            }
        L[i] = l * og;
        R[i] = r * og;
    }
}
float ParametricEQ::magnitudeDb(float hz) const {
    // Evaluate |H(e^jw)| of the cascade for the UI curve
    double w = 2 * M_PI * hz / m_sr;
    double cw = std::cos(w), sw = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
    double mag = 1;
    for (int b = 0; b < kBands; ++b) {
        if (!m_params.getBool(bp(b, 0))) continue;
        const auto& q = m_l[b];
        double nr = q.b0 + q.b1 * cw + q.b2 * c2, ni = -(q.b1 * sw + q.b2 * s2);
        double dr = 1 + q.a1 * cw + q.a2 * c2, di = -(q.a1 * sw + q.a2 * s2);
        mag *= std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di + 1e-20));
    }
    return (float)(20 * std::log10(mag + 1e-9)) + m_params.get(pOutput);
}

// ============================================================ Shelving EQ
ShelvingEQ::ShelvingEQ() {
    auto& p = m_params;
    p.add(pLowFreq, "Low Freq", 20, 1000, 100, ParamUnit::Hertz, true);
    p.add(pLowGain, "Low Gain", -18, 18, 0, ParamUnit::Decibel);
    p.add(pHighFreq, "High Freq", 1000, 20000, 8000, ParamUnit::Hertz, true);
    p.add(pHighGain, "High Gain", -18, 18, 0, ParamUnit::Decibel);
    p.add(pMidFreq, "Mid Freq", 100, 10000, 1000, ParamUnit::Hertz, true);
    p.add(pMidGain, "Mid Gain", -18, 18, 0, ParamUnit::Decibel);
    p.add(pMidQ, "Mid Q", 0.2f, 8, 0.8f);
}
void ShelvingEQ::reset() {
    m_ll.reset();
    m_lr.reset();
    m_hl.reset();
    m_hr.reset();
    m_ml.reset();
    m_mr.reset();
}
void ShelvingEQ::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext&) {
    auto& p = m_params;
    m_ll.set(Biquad::LowShelf, m_sr, p.get(pLowFreq), 0.7f, p.get(pLowGain));
    m_lr.b0 = m_ll.b0;
    m_lr.b1 = m_ll.b1;
    m_lr.b2 = m_ll.b2;
    m_lr.a1 = m_ll.a1;
    m_lr.a2 = m_ll.a2;
    m_hl.set(Biquad::HighShelf, m_sr, p.get(pHighFreq), 0.7f, p.get(pHighGain));
    m_hr.b0 = m_hl.b0;
    m_hr.b1 = m_hl.b1;
    m_hr.b2 = m_hl.b2;
    m_hr.a1 = m_hl.a1;
    m_hr.a2 = m_hl.a2;
    m_ml.set(Biquad::Peak, m_sr, p.get(pMidFreq), p.get(pMidQ), p.get(pMidGain));
    m_mr.b0 = m_ml.b0;
    m_mr.b1 = m_ml.b1;
    m_mr.b2 = m_ml.b2;
    m_mr.a1 = m_ml.a1;
    m_mr.a2 = m_ml.a2;
    for (int i = 0; i < n; ++i) {
        L[i] = m_hl.process(m_ml.process(m_ll.process(L[i])));
        R[i] = m_hr.process(m_mr.process(m_lr.process(R[i])));
    }
}

// ============================================================ Filters
FilterEffect::FilterEffect(Biquad::Type type, QString key, QString name)
    : m_type(type)
    , m_key(std::move(key))
    , m_name(std::move(name)) {
    auto& p = m_params;
    float def = type == Biquad::HighPass ? 120.f : (type == Biquad::LowPass ? 4000.f : 1000.f);
    p.add(pCutoff, "Cutoff", 20, 20000, def, ParamUnit::Hertz, true);
    p.add(pResonance, "Resonance", 0.1f, 10, 0.707f, ParamUnit::Generic, true);
    p.addChoice(pSlope, "Slope", {"12 dB", "24 dB", "36 dB", "48 dB"});
    p.add(pMix, "Mix", 0, 1, 1, ParamUnit::Percent);
}
void FilterEffect::reset() {
    for (int i = 0; i < 4; ++i) {
        m_l[i].reset();
        m_r[i].reset();
    }
}
void FilterEffect::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext&) {
    auto& p = m_params;
    int stages = p.getInt(pSlope) + 1;
    float mix = p.get(pMix);
    for (int s = 0; s < stages; ++s) {
        m_l[s].set(m_type, m_sr, p.get(pCutoff), p.get(pResonance));
        m_r[s].b0 = m_l[s].b0;
        m_r[s].b1 = m_l[s].b1;
        m_r[s].b2 = m_l[s].b2;
        m_r[s].a1 = m_l[s].a1;
        m_r[s].a2 = m_l[s].a2;
    }
    for (int i = 0; i < n; ++i) {
        float l = L[i], r = R[i];
        for (int s = 0; s < stages; ++s) {
            l = m_l[s].process(l);
            r = m_r[s].process(r);
        }
        mixDry(l, L[i], mix);
        mixDry(r, R[i], mix);
        L[i] = l;
        R[i] = r;
    }
}

// ============================================================ Delay
Delay::Delay() {
    auto& p = m_params;
    p.add(pTime, "Time", 1, 2000, 375, ParamUnit::Milliseconds, true);
    p.addChoice(pSync, "Sync", kSyncChoices, 7);
    p.add(pFeedback, "Feedback", 0, 1.1f, 0.4f, ParamUnit::Percent);
    p.add(pLowCut, "Low Cut", 20, 2000, 120, ParamUnit::Hertz, true);
    p.add(pHighCut, "High Cut", 500, 20000, 8000, ParamUnit::Hertz, true);
    p.add(pMix, "Mix", 0, 1, 0.3f, ParamUnit::Percent);
    p.add(pStereoOffset, "Stereo Offset", -50, 50, 0, ParamUnit::Milliseconds);
    p.add(pModRate, "Mod Rate", 0.05f, 5, 0.5f, ParamUnit::Hertz, true);
    p.add(pModDepth, "Mod Depth", 0, 1, 0);
}
void Delay::onPrepare() {
    m_l.allocate((size_t)(m_sr * 4.2));
    m_r.allocate((size_t)(m_sr * 4.2));
    m_timeS.prepare(m_sr, 60.f);
    m_timeS.setImmediate(0.375f * (float)m_sr);
    reset();
}
void Delay::reset() {
    m_l.clear();
    m_r.clear();
    m_lcL.reset();
    m_lcR.reset();
    m_hcL.reset();
    m_hcR.reset();
}
void Delay::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext& ctx) {
    auto& p = m_params;
    double t = syncDivisionSeconds(p.getInt(pSync), ctx.bpm);
    if (t <= 0) t = p.get(pTime) * 0.001;
    t = std::min(t, 4.0);
    m_timeS.setTarget((float)(t * m_sr));
    float fb = p.get(pFeedback), mix = p.get(pMix);
    float off = p.get(pStereoOffset) * 0.001f * (float)m_sr;
    float modD = p.get(pModDepth) * 0.002f * (float)m_sr;
    m_lcL.set(Biquad::HighPass, m_sr, p.get(pLowCut), 0.7f);
    m_lcR = m_lcL;
    m_hcL.set(Biquad::LowPass, m_sr, p.get(pHighCut), 0.7f);
    m_hcR = m_hcL;
    for (int i = 0; i < n; ++i) {
        float ts = m_timeS.next();
        float mod = m_lfo.next(p.get(pModRate), m_sr, 0) * modD;
        float dl = m_l.read(std::max(1.f, ts + mod + std::max(0.f, -off))), dr = m_r.read(std::max(1.f, ts - mod + std::max(0.f, off)));
        dl = m_hcL.process(m_lcL.process(dl));
        dr = m_hcR.process(m_lcR.process(dr));
        m_l.push(L[i] + dl * fb);
        m_r.push(R[i] + dr * fb);
        L[i] = L[i] + dl * mix;
        R[i] = R[i] + dr * mix;
    }
}

// ============================================================ Ping-pong delay
PingPongDelay::PingPongDelay() {
    auto& p = m_params;
    p.add(pTime, "Time", 1, 2000, 250, ParamUnit::Milliseconds, true);
    p.addChoice(pSync, "Sync", kSyncChoices, 5);
    p.add(pFeedback, "Feedback", 0, 1.1f, 0.5f, ParamUnit::Percent);
    p.add(pLowCut, "Low Cut", 20, 2000, 150, ParamUnit::Hertz, true);
    p.add(pHighCut, "High Cut", 500, 20000, 7000, ParamUnit::Hertz, true);
    p.add(pMix, "Mix", 0, 1, 0.3f, ParamUnit::Percent);
    p.add(pWidth, "Width", 0, 1, 1, ParamUnit::Percent);
}
void PingPongDelay::onPrepare() {
    m_l.allocate((size_t)(m_sr * 4.2));
    m_r.allocate((size_t)(m_sr * 4.2));
    m_timeS.prepare(m_sr, 60.f);
    m_timeS.setImmediate(0.25f * (float)m_sr);
    reset();
}
void PingPongDelay::reset() {
    m_l.clear();
    m_r.clear();
    m_lcL.reset();
    m_lcR.reset();
    m_hcL.reset();
    m_hcR.reset();
}
void PingPongDelay::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext& ctx) {
    auto& p = m_params;
    double t = syncDivisionSeconds(p.getInt(pSync), ctx.bpm);
    if (t <= 0) t = p.get(pTime) * 0.001;
    t = std::min(t, 4.0);
    m_timeS.setTarget((float)(t * m_sr));
    float fb = p.get(pFeedback), mix = p.get(pMix), width = p.get(pWidth);
    m_lcL.set(Biquad::HighPass, m_sr, p.get(pLowCut), 0.7f);
    m_lcR = m_lcL;
    m_hcL.set(Biquad::LowPass, m_sr, p.get(pHighCut), 0.7f);
    m_hcR = m_hcL;
    for (int i = 0; i < n; ++i) {
        float ts = m_timeS.next();
        float dl = m_hcL.process(m_lcL.process(m_l.read(ts))), dr = m_hcR.process(m_lcR.process(m_r.read(ts)));
        float in = (L[i] + R[i]) * 0.5f;
        m_l.push(in + dr * fb);
        m_r.push(dl * fb);   // cross-feed creates the ping-pong
        float wl = dl * (0.5f + width * 0.5f) + dr * (0.5f - width * 0.5f), wr = dr * (0.5f + width * 0.5f) + dl * (0.5f - width * 0.5f);
        L[i] += wl * mix;
        R[i] += wr * mix;
    }
}

// ============================================================ Reverb (comb/allpass network)
Reverb::Reverb() {
    auto& p = m_params;
    p.add(pSize, "Size", 0, 1, 0.6f, ParamUnit::Percent);
    p.add(pDamping, "Damping", 0, 1, 0.4f, ParamUnit::Percent);
    p.add(pWidth, "Width", 0, 1, 1, ParamUnit::Percent);
    p.add(pPreDelay, "Pre-delay", 0, 200, 10, ParamUnit::Milliseconds);
    p.add(pMix, "Mix", 0, 1, 0.25f, ParamUnit::Percent);
    p.add(pLowCut, "Low Cut", 20, 1000, 100, ParamUnit::Hertz, true);
    p.add(pHighCut, "High Cut", 1000, 20000, 9000, ParamUnit::Hertz, true);
    p.add(pDecay, "Decay", 0.1f, 20, 2.5f, ParamUnit::Seconds, true);
}
void Reverb::onPrepare() {
    static const int combLens[kCombs] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static const int apLens[kAllpass] = {556, 441, 341, 225};
    double scale = m_sr / 44100.0;
    const int spread = 23;
    for (int c = 0; c < kCombs; ++c) {
        m_combL[c].buf.assign((size_t)(combLens[c] * scale * 1.5) + 8, 0.f);
        m_combR[c].buf.assign((size_t)((combLens[c] + spread) * scale * 1.5) + 8, 0.f);
    }
    for (int a = 0; a < kAllpass; ++a) {
        m_apL[a].assign((size_t)(apLens[a] * scale) + 8, 0.f);
        m_apR[a].assign((size_t)((apLens[a] + spread) * scale) + 8, 0.f);
    }
    m_pre.allocate((size_t)(m_sr * 0.25));
    m_preR.allocate((size_t)(m_sr * 0.25));
    reset();
}
void Reverb::reset() {
    for (int c = 0; c < kCombs; ++c) {
        std::fill(m_combL[c].buf.begin(), m_combL[c].buf.end(), 0.f);
        std::fill(m_combR[c].buf.begin(), m_combR[c].buf.end(), 0.f);
        m_combL[c].filt = m_combR[c].filt = 0;
        m_combL[c].idx = m_combR[c].idx = 0;
    }
    for (int a = 0; a < kAllpass; ++a) {
        std::fill(m_apL[a].begin(), m_apL[a].end(), 0.f);
        std::fill(m_apR[a].begin(), m_apR[a].end(), 0.f);
        m_apIdx[0][a] = m_apIdx[1][a] = 0;
    }
    m_pre.clear();
    m_preR.clear();
    m_lc.reset();
    m_hcL.reset();
    m_hcR.reset();
}
void Reverb::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext&) {
    auto& p = m_params;
    float size = p.get(pSize), damp = p.get(pDamping) * 0.6f, width = p.get(pWidth), mix = p.get(pMix);
    float pre = p.get(pPreDelay) * 0.001f * (float)m_sr + 1.f;
    // feedback derived from decay time so 'Decay' is meaningful: g = 10^(-3 * loopTime / RT60)
    float decay = p.get(pDecay);
    float lenScale = 0.5f + size;
    m_lc.set(Biquad::HighPass, m_sr, p.get(pLowCut), 0.7f);
    m_hcL.set(Biquad::LowPass, m_sr, p.get(pHighCut), 0.7f);
    m_hcR = m_hcL;
    for (int i = 0; i < n; ++i) {
        float in = m_lc.process((L[i] + R[i]) * 0.5f) * 0.3f;
        m_pre.push(in);
        float x = m_pre.read(pre);
        float outL = 0, outR = 0;
        for (int c = 0; c < kCombs; ++c) {
            auto proc = [&](Comb& cb) {
                size_t len = (size_t)((cb.buf.size() - 8) / 1.5 * lenScale);
                if (len < 8) len = 8;
                size_t idx = cb.idx % len;
                float y = cb.buf[idx];
                cb.filt = y * (1 - damp) + cb.filt * damp;
                float loopSec = (float)len / (float)m_sr;
                float g = std::pow(10.f, -3.f * loopSec / decay);
                cb.buf[idx] = flushDenormal(x + cb.filt * g);
                cb.idx = (idx + 1) % len;
                return y;
            };
            outL += proc(m_combL[c]);
            outR += proc(m_combR[c]);
        }
        for (int a = 0; a < kAllpass; ++a) {
            auto ap = [&](std::vector<float>& buf, size_t& idx, float v) {
                float b = buf[idx];
                float y = -v + b;
                buf[idx] = flushDenormal(v + b * 0.5f);
                idx = (idx + 1) % buf.size();
                return y;
            };
            outL = ap(m_apL[a], m_apIdx[0][a], outL);
            outR = ap(m_apR[a], m_apIdx[1][a], outR);
        }
        outL = m_hcL.process(outL);
        outR = m_hcR.process(outR);
        float wl = outL * (0.5f + width * 0.5f) + outR * (0.5f - width * 0.5f),
              wr = outR * (0.5f + width * 0.5f) + outL * (0.5f - width * 0.5f);
        L[i] = L[i] * (1 - mix) + wl * mix;
        R[i] = R[i] * (1 - mix) + wr * mix;
    }
}

// ============================================================ Echo (multi-tap)
Echo::Echo() {
    auto& p = m_params;
    p.add(pTime, "Time", 1, 2000, 300, ParamUnit::Milliseconds, true);
    p.addChoice(pSync, "Sync", kSyncChoices, 5);
    p.add(pFeedback, "Feedback", 0, 0.95f, 0.3f, ParamUnit::Percent);
    p.add(pTaps, "Taps", 1, 8, 4);
    p.add(pSpread, "Stereo Spread", 0, 1, 0.5f);
    p.add(pDamp, "Damping", 500, 20000, 6000, ParamUnit::Hertz, true);
    p.add(pMix, "Mix", 0, 1, 0.3f, ParamUnit::Percent);
    p.add(pDecayShape, "Tap Decay", 0, 1, 0.6f);
}
void Echo::onPrepare() {
    m_l.allocate((size_t)(m_sr * 4.2) * 2);
    m_r.allocate((size_t)(m_sr * 4.2) * 2);
    reset();
}
void Echo::reset() {
    m_l.clear();
    m_r.clear();
    m_dampL.reset();
    m_dampR.reset();
}
void Echo::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext& ctx) {
    auto& p = m_params;
    double t = syncDivisionSeconds(p.getInt(pSync), ctx.bpm);
    if (t <= 0) t = p.get(pTime) * 0.001;
    t = std::min(t, 1.0);
    float ts = (float)(t * m_sr);
    int taps = p.getInt(pTaps);
    float fb = p.get(pFeedback), spread = p.get(pSpread), mix = p.get(pMix), shape = p.get(pDecayShape);
    m_dampL.set(Biquad::LowPass, m_sr, p.get(pDamp), 0.7f);
    m_dampR = m_dampL;
    for (int i = 0; i < n; ++i) {
        float wl = 0, wr = 0;
        for (int k = 1; k <= taps; ++k) {
            float g = std::pow(1.f - shape, (float)(k - 1));
            float pan = ((k % 2) ? -1.f : 1.f) * spread * (float)k / taps;
            float dl = m_l.read(ts * k), dr = m_r.read(ts * k);
            wl += dl * g * (0.5f - pan * 0.5f) + dr * g * (0.5f + pan * 0.5f) * 0.0f;
            wr += dr * g * (0.5f + pan * 0.5f);
        }
        wl = m_dampL.process(wl);
        wr = m_dampR.process(wr);
        m_l.push(L[i] + wl * fb);
        m_r.push(R[i] + wr * fb);
        L[i] += wl * mix;
        R[i] += wr * mix;
    }
}

// ============================================================ Chorus
Chorus::Chorus() {
    auto& p = m_params;
    p.add(pRate, "Rate", 0.05f, 10, 0.8f, ParamUnit::Hertz, true);
    p.add(pDepth, "Depth", 0, 1, 0.5f);
    p.add(pDelay, "Delay", 1, 40, 12, ParamUnit::Milliseconds);
    p.add(pVoices, "Voices", 1, 4, 2);
    p.add(pMix, "Mix", 0, 1, 0.5f, ParamUnit::Percent);
    p.add(pFeedback, "Feedback", 0, 0.9f, 0);
    p.add(pWidth, "Width", 0, 1, 1);
}
void Chorus::onPrepare() {
    m_l.allocate((size_t)(m_sr * 0.1));
    m_r.allocate((size_t)(m_sr * 0.1));
    for (int i = 0; i < 4; ++i) m_lfo[i].phase = i * 0.25;
    reset();
}
void Chorus::reset() {
    m_l.clear();
    m_r.clear();
}
void Chorus::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext&) {
    auto& p = m_params;
    float rate = p.get(pRate), depth = p.get(pDepth) * 0.004f * (float)m_sr, base = p.get(pDelay) * 0.001f * (float)m_sr;
    int voices = p.getInt(pVoices);
    float mix = p.get(pMix), fb = p.get(pFeedback), width = p.get(pWidth);
    for (int i = 0; i < n; ++i) {
        float wl = 0, wr = 0;
        for (int v = 0; v < voices; ++v) {
            float m = m_lfo[v].next(rate * (1.f + v * 0.13f), m_sr, 0);
            float d = base + (m + 1.f) * depth;
            float dl = m_l.read(d), dr = m_r.read(base + (-m + 1.f) * depth);
            float pan = voices > 1 ? ((float)v / (voices - 1) - 0.5f) * width : 0;
            wl += dl * (0.5f - pan) + dr * (0.5f + pan) * (1 - width);
            wr += dr * (0.5f + pan) + dl * (0.5f - pan) * (1 - width);
        }
        wl /= voices;
        wr /= voices;
        m_l.push(L[i] + wl * fb);
        m_r.push(R[i] + wr * fb);
        L[i] = L[i] * (1 - mix * 0.5f) + wl * mix;
        R[i] = R[i] * (1 - mix * 0.5f) + wr * mix;
    }
}

// ============================================================ Flanger
Flanger::Flanger() {
    auto& p = m_params;
    p.add(pRate, "Rate", 0.02f, 10, 0.3f, ParamUnit::Hertz, true);
    p.add(pDepth, "Depth", 0, 1, 0.7f);
    p.add(pDelay, "Delay", 0.1f, 10, 1, ParamUnit::Milliseconds);
    p.add(pFeedback, "Feedback", -0.95f, 0.95f, 0.5f);
    p.add(pMix, "Mix", 0, 1, 0.5f, ParamUnit::Percent);
    p.add(pPhase, "Stereo Phase", 0, 180, 90, ParamUnit::Degrees);
    p.addChoice(pSync, "Sync", kSyncChoices, 0);
}
void Flanger::onPrepare() {
    m_l.allocate((size_t)(m_sr * 0.05));
    m_r.allocate((size_t)(m_sr * 0.05));
    reset();
}
void Flanger::reset() {
    m_l.clear();
    m_r.clear();
    m_fbL = m_fbR = 0;
}
void Flanger::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext& ctx) {
    auto& p = m_params;
    float rate = p.get(pRate);
    double sync = syncDivisionSeconds(p.getInt(pSync), ctx.bpm);
    if (sync > 0) rate = (float)(1.0 / sync);
    float depth = p.get(pDepth) * 0.005f * (float)m_sr, base = p.get(pDelay) * 0.001f * (float)m_sr + 1;
    float fb = p.get(pFeedback), mix = p.get(pMix);
    float ph = p.get(pPhase) / 360.f;
    for (int i = 0; i < n; ++i) {
        float m = m_lfo.next(rate, m_sr, 0);
        float m2 = std::sin((float)((m_lfo.phase + ph) * kTwoPi));
        float dl = m_l.read(base + (m + 1.f) * depth), dr = m_r.read(base + (m2 + 1.f) * depth);
        m_l.push(L[i] + dl * fb);
        m_r.push(R[i] + dr * fb);
        L[i] = L[i] * (1 - mix * 0.5f) + dl * mix;
        R[i] = R[i] * (1 - mix * 0.5f) + dr * mix;
    }
}

// ============================================================ Phaser
Phaser::Phaser() {
    auto& p = m_params;
    p.add(pRate, "Rate", 0.02f, 10, 0.5f, ParamUnit::Hertz, true);
    p.add(pDepth, "Depth", 0, 1, 0.8f);
    p.add(pStages, "Stages", 2, 12, 6);
    p.add(pFeedback, "Feedback", -0.95f, 0.95f, 0.3f);
    p.add(pCenter, "Center", 100, 8000, 1000, ParamUnit::Hertz, true);
    p.add(pMix, "Mix", 0, 1, 0.5f, ParamUnit::Percent);
    p.add(pSpread, "Stereo Spread", 0, 1, 0.5f);
    p.addChoice(pSync, "Sync", kSyncChoices, 0);
}
void Phaser::reset() {
    for (int i = 0; i < 12; ++i) {
        m_apL[i].reset();
        m_apR[i].reset();
    }
    m_fbL = m_fbR = 0;
}
void Phaser::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext& ctx) {
    auto& p = m_params;
    float rate = p.get(pRate);
    double sync = syncDivisionSeconds(p.getInt(pSync), ctx.bpm);
    if (sync > 0) rate = (float)(1.0 / sync);
    float depth = p.get(pDepth);
    int stages = p.getInt(pStages);
    float fb = p.get(pFeedback), center = p.get(pCenter), mix = p.get(pMix), spread = p.get(pSpread);
    for (int i = 0; i < n; ++i) {
        float m = m_lfo.next(rate, m_sr, 0);
        float m2 = std::sin((float)((m_lfo.phase + spread * 0.5) * kTwoPi));
        if ((i & 7) == 0) {
            float fl = center * std::pow(2.f, m * depth * 2.f), fr = center * std::pow(2.f, m2 * depth * 2.f);
            for (int s = 0; s < stages; ++s) {
                m_apL[s].set(Biquad::AllPass, m_sr, fl * (1.f + s * 0.3f), 0.5f);
                m_apR[s].set(Biquad::AllPass, m_sr, fr * (1.f + s * 0.3f), 0.5f);
            }
        }
        float xl = L[i] + m_fbL * fb, xr = R[i] + m_fbR * fb;
        for (int s = 0; s < stages; ++s) {
            xl = m_apL[s].process(xl);
            xr = m_apR[s].process(xr);
        }
        m_fbL = xl;
        m_fbR = xr;
        L[i] = L[i] * (1 - mix * 0.5f) + xl * mix * 0.5f;
        R[i] = R[i] * (1 - mix * 0.5f) + xr * mix * 0.5f;
    }
}

// ============================================================ Tremolo
Tremolo::Tremolo() {
    auto& p = m_params;
    p.add(pRate, "Rate", 0.1f, 30, 5, ParamUnit::Hertz, true);
    p.add(pDepth, "Depth", 0, 1, 0.6f);
    p.addChoice(pShape, "Shape", {"Sine", "Triangle", "Saw", "Square", "S&H"});
    p.addChoice(pSync, "Sync", kSyncChoices, 0);
    p.add(pStereoPhase, "Stereo Phase", 0, 180, 0, ParamUnit::Degrees);
    p.add(pSmooth, "Smooth", 0, 1, 0.3f);
}
void Tremolo::reset() { m_lfo.phase = 0; }
void Tremolo::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext& ctx) {
    auto& p = m_params;
    float rate = p.get(pRate);
    double sync = syncDivisionSeconds(p.getInt(pSync), ctx.bpm);
    if (sync > 0) rate = (float)(1.0 / sync);
    float depth = p.get(pDepth);
    int shape = p.getInt(pShape);
    float ph = p.get(pStereoPhase) / 360.f;
    for (int i = 0; i < n; ++i) {
        float m = m_lfo.next(rate, m_sr, shape);
        double p2 = std::fmod(m_lfo.phase + ph, 1.0);
        float m2;
        switch (shape) {
        case 1: m2 = (float)(p2 < 0.5 ? p2 * 4 - 1 : 3 - p2 * 4); break;
        case 2: m2 = (float)(p2 * 2 - 1); break;
        case 3: m2 = p2 < 0.5 ? 1.f : -1.f; break;
        case 4: m2 = m; break;
        default: m2 = std::sin((float)(p2 * kTwoPi));
        }
        L[i] *= 1.f - depth * (0.5f - m * 0.5f);
        R[i] *= 1.f - depth * (0.5f - m2 * 0.5f);
    }
}

// ============================================================ Vibrato
Vibrato::Vibrato() {
    auto& p = m_params;
    p.add(pRate, "Rate", 0.1f, 20, 5, ParamUnit::Hertz, true);
    p.add(pDepth, "Depth", 0, 1, 0.3f);
    p.addChoice(pShape, "Shape", {"Sine", "Triangle", "Saw", "Square", "S&H"});
    p.addChoice(pSync, "Sync", kSyncChoices, 0);
    p.add(pStereoPhase, "Stereo Phase", 0, 180, 0, ParamUnit::Degrees);
}
void Vibrato::onPrepare() {
    m_l.allocate((size_t)(m_sr * 0.05));
    m_r.allocate((size_t)(m_sr * 0.05));
    reset();
}
void Vibrato::reset() {
    m_l.clear();
    m_r.clear();
}
void Vibrato::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext& ctx) {
    auto& p = m_params;
    float rate = p.get(pRate);
    double sync = syncDivisionSeconds(p.getInt(pSync), ctx.bpm);
    if (sync > 0) rate = (float)(1.0 / sync);
    float depth = p.get(pDepth) * 0.01f * (float)m_sr;
    int shape = p.getInt(pShape);
    float ph = p.get(pStereoPhase) / 360.f;
    float base = depth + 2;
    for (int i = 0; i < n; ++i) {
        m_l.push(L[i]);
        m_r.push(R[i]);
        float m = m_lfo.next(rate, m_sr, shape);
        float m2 = std::sin((float)((m_lfo.phase + ph) * kTwoPi));
        L[i] = m_l.read(base + m * depth);
        R[i] = m_r.read(base + m2 * depth);
    }
}

// ============================================================ Saturation
Saturation::Saturation() {
    auto& p = m_params;
    p.add(pDrive, "Drive", 0, 1, 0.3f);
    p.add(pTone, "Tone", 500, 20000, 8000, ParamUnit::Hertz, true);
    p.add(pMix, "Mix", 0, 1, 1, ParamUnit::Percent);
    p.add(pOutput, "Output", -24, 12, 0, ParamUnit::Decibel);
    p.addChoice(pType, "Type", {"Tape", "Tube", "Transistor"});
    p.add(pBias, "Bias", 0, 1, 0);
}
void Saturation::reset() {
    m_toneL.reset();
    m_toneR.reset();
}
void Saturation::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext&) {
    auto& p = m_params;
    float drive = 1.f + p.get(pDrive) * 15.f, mix = p.get(pMix), og = dbToGain(p.get(pOutput));
    int type = p.getInt(pType);
    float bias = p.get(pBias) * 0.3f;
    m_toneL.set(Biquad::LowPass, m_sr, p.get(pTone), 0.7f);
    m_toneR = m_toneL;
    float comp = 1.f / std::sqrt(drive);
    auto shape = [&](float x) {
        x = x * drive + bias;
        switch (type) {
        case 1: return (fastTanh(x) + 0.15f * x * x * (x > 0 ? 1 : -1));
        case 2: return x / (1.f + std::fabs(x)) * 1.5f;
        default: return fastTanh(x);
        }
    };
    for (int i = 0; i < n; ++i) {
        float l = m_dcL.process(m_toneL.process(shape(L[i]))) * comp, r = m_dcR.process(m_toneR.process(shape(R[i]))) * comp;
        mixDry(l, L[i], mix);
        mixDry(r, R[i], mix);
        L[i] = l * og;
        R[i] = r * og;
    }
}

// ============================================================ Waveshaper
Waveshaper::Waveshaper() {
    auto& p = m_params;
    p.addChoice(pCurve, "Curve", {"Soft", "Hard", "Fold", "Sine", "Chebyshev", "Rectify"});
    p.add(pAmount, "Amount", 0, 1, 0.5f);
    p.add(pMix, "Mix", 0, 1, 1, ParamUnit::Percent);
    p.add(pInput, "Input", -12, 24, 0, ParamUnit::Decibel);
    p.add(pOutput, "Output", -24, 12, 0, ParamUnit::Decibel);
    p.add(pSymmetry, "Asymmetry", -1, 1, 0);
}
void Waveshaper::reset() {}
void Waveshaper::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext&) {
    auto& p = m_params;
    int curve = p.getInt(pCurve);
    float amt = p.get(pAmount), mix = p.get(pMix), ig = dbToGain(p.get(pInput)), og = dbToGain(p.get(pOutput)), asym = p.get(pSymmetry);
    auto f = [&](float x) {
        x *= ig;
        x += asym * 0.2f;
        float k = 1.f + amt * 20.f;
        switch (curve) {
        case 1: return clamp(x * k, -1.f, 1.f) / std::sqrt(k) * 2.f;
        case 2: {
            float y = x * k;
            y = std::fmod(y + 1.f, 4.f);
            if (y < 0) y += 4.f;
            return (y < 2 ? y - 1 : 3 - y) * 0.7f;
        }
        case 3: return std::sin(x * k * 0.5f);
        case 4: {
            float y = clamp(x * (1 + amt * 3), -1.f, 1.f);
            return (2 * y * y - 1) * amt + y * (1 - amt);
        }
        case 5: return std::fabs(x * (1 + amt * 3)) * 2.f - 1.f;
        default: return fastTanh(x * k) / std::sqrt(k) * 2.f;
        }
    };
    for (int i = 0; i < n; ++i) {
        float l = m_dcL.process(f(L[i])), r = m_dcR.process(f(R[i]));
        mixDry(l, L[i], mix);
        mixDry(r, R[i], mix);
        L[i] = l * og;
        R[i] = r * og;
    }
}

// ============================================================ BitCrusher
BitCrusher::BitCrusher() {
    auto& p = m_params;
    p.add(pBits, "Bits", 1, 16, 8);
    p.add(pDownsample, "Downsample", 1, 64, 4);
    p.add(pMix, "Mix", 0, 1, 1, ParamUnit::Percent);
    p.addToggle(pDither, "Dither");
    p.add(pJitter, "Jitter", 0, 1, 0);
}
void BitCrusher::reset() {
    m_counter = 0;
    m_holdL = m_holdR = 0;
}
void BitCrusher::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext&) {
    auto& p = m_params;
    float bits = p.get(pBits), ds = p.get(pDownsample), mix = p.get(pMix);
    bool dith = p.getBool(pDither);
    float jit = p.get(pJitter);
    float levels = std::pow(2.f, bits) * 0.5f;
    for (int i = 0; i < n; ++i) {
        m_counter += 1.f;
        if (m_counter >= ds * (1.f + jit * m_rng.uniform())) {
            m_counter = 0;
            float dl = dith ? (m_rng.bipolar() / levels) * 0.5f : 0.f;
            m_holdL = std::round((L[i] + dl) * levels) / levels;
            m_holdR = std::round((R[i] + dl) * levels) / levels;
        }
        float l = m_holdL, r = m_holdR;
        mixDry(l, L[i], mix);
        mixDry(r, R[i], mix);
        L[i] = l;
        R[i] = r;
    }
}

// ============================================================ Distortion
Distortion::Distortion() {
    auto& p = m_params;
    p.add(pDrive, "Drive", 0, 1, 0.5f);
    p.addChoice(pType, "Type", {"Overdrive", "Fuzz", "Hard Clip", "Foldback"});
    p.add(pTone, "Tone", 500, 20000, 5000, ParamUnit::Hertz, true);
    p.add(pLevel, "Level", -24, 12, -6, ParamUnit::Decibel);
    p.add(pMix, "Mix", 0, 1, 1, ParamUnit::Percent);
    p.add(pLowCut, "Low Cut", 20, 1000, 60, ParamUnit::Hertz, true);
}
void Distortion::reset() {
    m_toneL.reset();
    m_toneR.reset();
    m_lcL.reset();
    m_lcR.reset();
}
void Distortion::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext&) {
    auto& p = m_params;
    float drive = 1.f + p.get(pDrive) * 60.f;
    int type = p.getInt(pType);
    float og = dbToGain(p.get(pLevel)), mix = p.get(pMix);
    m_toneL.set(Biquad::LowPass, m_sr, p.get(pTone), 0.7f);
    m_toneR = m_toneL;
    m_lcL.set(Biquad::HighPass, m_sr, p.get(pLowCut), 0.7f);
    m_lcR = m_lcL;
    auto f = [&](float x) {
        x *= drive;
        switch (type) {
        case 1: return (x > 0 ? 1.f : -1.f) * (1.f - std::exp(-std::fabs(x)));
        case 2: return clamp(x, -1.f, 1.f);
        case 3: {
            float y = std::fmod(x + 1.f, 4.f);
            if (y < 0) y += 4.f;
            return y < 2 ? y - 1 : 3 - y;
        }
        default: return fastTanh(x);
        }
    };
    for (int i = 0; i < n; ++i) {
        float l = m_dcL.process(m_toneL.process(f(m_lcL.process(L[i])))), r = m_dcR.process(m_toneR.process(f(m_lcR.process(R[i]))));
        mixDry(l, L[i], mix);
        mixDry(r, R[i], mix);
        L[i] = l * og;
        R[i] = r * og;
    }
}

// ============================================================ Utilities
GainUtility::GainUtility() {
    auto& p = m_params;
    p.add(pGain, "Gain", -60, 24, 0, ParamUnit::Decibel);
    p.add(pPan, "Pan", -1, 1, 0);
    p.add(pBalance, "Balance", -1, 1, 0);
    p.addToggle(pMute, "Mute");
}
void GainUtility::onPrepare() {
    m_g.prepare(m_sr, 5);
    m_p.prepare(m_sr, 5);
    m_g.setImmediate(1);
    reset();
}
void GainUtility::reset() {}
void GainUtility::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext&) {
    auto& p = m_params;
    m_g.setTarget(p.getBool(pMute) ? 0.f : dbToGain(p.get(pGain)));
    m_p.setTarget(p.get(pPan));
    float bal = p.get(pBalance);
    for (int i = 0; i < n; ++i) {
        float g = m_g.next(), pan = m_p.next();
        float a = (pan + 1) * 0.25f * kPi;
        float gl = std::cos(a) * 1.4142f, gr = std::sin(a) * 1.4142f;
        float bl = bal > 0 ? 1 - bal : 1, br = bal < 0 ? 1 + bal : 1;
        L[i] *= g * gl * bl;
        R[i] *= g * gr * br;
    }
}
StereoWidth::StereoWidth() {
    auto& p = m_params;
    p.add(pWidth, "Width", 0, 2, 1);
    p.addToggle(pMonoBelow, "Bass Mono");
    p.add(pBassMonoFreq, "Bass Mono Freq", 40, 500, 120, ParamUnit::Hertz, true);
    p.add(pMidGain, "Mid Gain", -24, 12, 0, ParamUnit::Decibel);
    p.add(pSideGain, "Side Gain", -24, 12, 0, ParamUnit::Decibel);
}
void StereoWidth::reset() {
    m_lpL.reset();
    m_lpR.reset();
    m_hpL.reset();
    m_hpR.reset();
}
void StereoWidth::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext&) {
    auto& p = m_params;
    float w = p.get(pWidth);
    bool bm = p.getBool(pMonoBelow);
    float mg = dbToGain(p.get(pMidGain)), sg = dbToGain(p.get(pSideGain));
    float f = p.get(pBassMonoFreq);
    m_lpL.set(Biquad::LowPass, m_sr, f, 0.7f);
    m_lpR = m_lpL;
    m_hpL.set(Biquad::HighPass, m_sr, f, 0.7f);
    m_hpR = m_hpL;
    for (int i = 0; i < n; ++i) {
        float l = L[i], r = R[i];
        if (bm) {
            float lowM = (m_lpL.process(l) + m_lpR.process(r)) * 0.5f;
            l = m_hpL.process(l) + lowM;
            r = m_hpR.process(r) + lowM;
        }
        float m = (l + r) * 0.5f * mg, s = (l - r) * 0.5f * w * sg;
        L[i] = m + s;
        R[i] = m - s;
    }
}
PhaseInvert::PhaseInvert() {
    auto& p = m_params;
    p.addToggle(pLeft, "Invert Left");
    p.addToggle(pRight, "Invert Right");
    p.addToggle(pSwap, "Swap L/R");
}
void PhaseInvert::reset() {}
void PhaseInvert::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext&) {
    auto& p = m_params;
    float gl = p.getBool(pLeft) ? -1.f : 1.f, gr = p.getBool(pRight) ? -1.f : 1.f;
    bool sw = p.getBool(pSwap);
    for (int i = 0; i < n; ++i) {
        float l = L[i] * gl, r = R[i] * gr;
        if (sw) std::swap(l, r);
        L[i] = l;
        R[i] = r;
    }
}
MonoConversion::MonoConversion() {
    auto& p = m_params;
    p.addChoice(pMode, "Mode", {"Mono (L+R)", "Left", "Right", "Side"});
    p.add(pMix, "Mix", 0, 1, 1, ParamUnit::Percent);
}
void MonoConversion::reset() {}
void MonoConversion::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext&) {
    auto& p = m_params;
    int mode = p.getInt(pMode);
    float mix = p.get(pMix);
    for (int i = 0; i < n; ++i) {
        float m;
        switch (mode) {
        case 1: m = L[i]; break;
        case 2: m = R[i]; break;
        case 3: m = (L[i] - R[i]) * 0.5f; break;
        default: m = (L[i] + R[i]) * 0.5f;
        }
        L[i] = L[i] + (m - L[i]) * mix;
        R[i] = R[i] + (m - R[i]) * mix;
    }
}

// ============================================================ Spectrum analyzer
SpectrumAnalyzer::SpectrumAnalyzer() {
    auto& p = m_params;
    p.add(pDecay, "Decay", 0, 1, 0.8f);
    p.add(pRange, "Range", 30, 120, 90, ParamUnit::Decibel);
    p.add(pSmooth, "Smoothing", 0, 1, 0.3f);
}
void SpectrumAnalyzer::onPrepare() {
    m_ring.assign(kFft, 0.f);
    m_in.assign(kFft, 0.f);
    m_window.resize(kFft);
    for (int i = 0; i < kFft; ++i) m_window[i] = 0.5f * (1.f - std::cos(kTwoPi * i / (kFft - 1)));
    m_out.assign(kFft / 2 + 1, {});
    m_mag[0].assign(kFft / 2, -120.f);
    m_mag[1].assign(kFft / 2, -120.f);
    reset();
}
void SpectrumAnalyzer::reset() {
    m_ringPos = 0;
    m_sinceFft = 0;
}
void SpectrumAnalyzer::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext&) {
    if (m_out.empty()) return;
    float decay = m_params.get(pDecay);
    for (int i = 0; i < n; ++i) {
        m_ring[m_ringPos] = (L[i] + R[i]) * 0.5f;
        m_ringPos = (m_ringPos + 1) % kFft;
    }
    m_sinceFft += n;
    if (m_sinceFft >= kFft / 4) {
        m_sinceFft = 0;
        for (int i = 0; i < kFft; ++i) m_in[i] = m_ring[(m_ringPos + i) % kFft] * m_window[i];
        m_fft.forwardReal(m_in.data(), m_out.data());
        int back = 1 - m_front.load();
        auto& dst = m_mag[back];
        const auto& prev = m_mag[m_front.load()];
        for (int k = 0; k < kFft / 2; ++k) {
            float db = 20.f * std::log10(std::abs(m_out[k]) / (kFft / 4) + 1e-9f);
            dst[k] = std::max(db, prev[k] - (1.f - decay) * 60.f);
        }
        m_front.store(back);
    }
}
void SpectrumAnalyzer::readSpectrum(std::vector<float>& out) const {
    int f = m_front.load();
    out = m_mag[f];
}

// ============================================================ Meter
MeterEffect::MeterEffect() {
    auto& p = m_params;
    p.add(pHold, "Peak Hold", 0, 5, 1.5f, ParamUnit::Seconds);
    p.add(pRmsWindow, "RMS Window", 10, 1000, 300, ParamUnit::Milliseconds);
}
void MeterEffect::reset() {
    m_sqL = m_sqR = 0;
    m_hl = m_hr = 0;
}
void MeterEffect::process(float* L, float* R, int n, const MidiEventList&, const ProcessContext&) {
    float c = std::exp(-1.f / (float)(m_sr * m_params.get(pRmsWindow) * 0.001f));
    float pl = 0, pr = 0;
    int holdN = (int)(m_params.get(pHold) * m_sr);
    for (int i = 0; i < n; ++i) {
        float l = std::fabs(L[i]), r = std::fabs(R[i]);
        pl = std::max(pl, l);
        pr = std::max(pr, r);
        m_sqL = c * m_sqL + (1 - c) * L[i] * L[i];
        m_sqR = c * m_sqR + (1 - c) * R[i] * R[i];
    }
    if (pl >= m_hl) {
        m_hl = pl;
        m_holdCount = holdN;
    }
    if (pr >= m_hr) m_hr = pr;
    if (m_holdCount > 0) m_holdCount -= n;
    else {
        m_hl *= 0.98f;
        m_hr *= 0.98f;
    }
    m_pkL.store(pl);
    m_pkR.store(pr);
    m_rmsL.store(std::sqrt(m_sqL));
    m_rmsR.store(std::sqrt(m_sqR));
    m_holdL.store(m_hl);
    m_holdR.store(m_hr);
}

}   // namespace mt
