// Small, allocation-free DSP building blocks used by instruments and effects.
#pragma once
#include "core/Types.h"
#include <vector>
#include <cmath>
#include <cstring>
#include <cstdint>

namespace mt::dsp {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

inline float fastTanh(float x) {
    if (x < -3.f) return -1.f;
    if (x > 3.f) return 1.f;
    float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}
inline float softClip(float x) { return fastTanh(x); }
inline float flushDenormal(float x) { return std::fabs(x) < 1e-20f ? 0.f : x; }

// Deterministic fast PRNG (xorshift) - safe on the audio thread
struct Rng {
    uint32_t s = 0x9E3779B9u;
    inline uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    inline float uniform() { return (next() & 0xFFFFFF) / 16777216.f; }   // 0..1
    inline float bipolar() { return uniform() * 2.f - 1.f; }
};

// Transposed direct-form II biquad
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;
    enum Type { LowPass, HighPass, BandPass, Notch, Peak, LowShelf, HighShelf, AllPass };
    void reset() { z1 = z2 = 0; }
    inline float process(float x) {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return flushDenormal(y);
    }
    void set(Type t, double sr, float freq, float q, float gainDb = 0.f) {
        freq = clamp(freq, 10.f, (float)(sr * 0.49));
        q = std::max(q, 0.05f);
        float A = std::pow(10.f, gainDb / 40.f);
        float w0 = kTwoPi * freq / (float)sr;
        float cw = std::cos(w0), sw = std::sin(w0);
        float alpha = sw / (2.f * q);
        float a0;
        switch (t) {
        case LowPass:
            b0 = (1 - cw) / 2;
            b1 = 1 - cw;
            b2 = (1 - cw) / 2;
            a0 = 1 + alpha;
            a1 = -2 * cw;
            a2 = 1 - alpha;
            break;
        case HighPass:
            b0 = (1 + cw) / 2;
            b1 = -(1 + cw);
            b2 = (1 + cw) / 2;
            a0 = 1 + alpha;
            a1 = -2 * cw;
            a2 = 1 - alpha;
            break;
        case BandPass:
            b0 = alpha;
            b1 = 0;
            b2 = -alpha;
            a0 = 1 + alpha;
            a1 = -2 * cw;
            a2 = 1 - alpha;
            break;
        case Notch:
            b0 = 1;
            b1 = -2 * cw;
            b2 = 1;
            a0 = 1 + alpha;
            a1 = -2 * cw;
            a2 = 1 - alpha;
            break;
        case AllPass:
            b0 = 1 - alpha;
            b1 = -2 * cw;
            b2 = 1 + alpha;
            a0 = 1 + alpha;
            a1 = -2 * cw;
            a2 = 1 - alpha;
            break;
        case Peak:
            b0 = 1 + alpha * A;
            b1 = -2 * cw;
            b2 = 1 - alpha * A;
            a0 = 1 + alpha / A;
            a1 = -2 * cw;
            a2 = 1 - alpha / A;
            break;
        case LowShelf: {
            float sa = 2 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1) - (A - 1) * cw + sa);
            b1 = 2 * A * ((A - 1) - (A + 1) * cw);
            b2 = A * ((A + 1) - (A - 1) * cw - sa);
            a0 = (A + 1) + (A - 1) * cw + sa;
            a1 = -2 * ((A - 1) + (A + 1) * cw);
            a2 = (A + 1) + (A - 1) * cw - sa;
            break;
        }
        case HighShelf: {
            float sa = 2 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1) + (A - 1) * cw + sa);
            b1 = -2 * A * ((A - 1) + (A + 1) * cw);
            b2 = A * ((A + 1) + (A - 1) * cw - sa);
            a0 = (A + 1) - (A - 1) * cw + sa;
            a1 = 2 * ((A - 1) - (A + 1) * cw);
            a2 = (A + 1) - (A - 1) * cw - sa;
            break;
        }
        default: a0 = 1; break;
        }
        b0 /= a0;
        b1 /= a0;
        b2 /= a0;
        a1 /= a0;
        a2 /= a0;
    }
};

// State-variable filter (Chamberlin, oversampled 2x) - good for synth voices
struct SVF {
    float low = 0, band = 0, high = 0, notch = 0;
    float f = 0.1f, q = 1.f;
    void reset() { low = band = high = notch = 0; }
    // Chamberlin SVF (2x oversampled) is only stable for f < ~0.8, i.e. cutoff below ~sr/4; clamp accordingly
    void set(double sr, float cutoff, float resonance) {
        cutoff = clamp(cutoff, 20.f, (float)(sr * 0.24));
        f = 2.f * std::sin(kPi * cutoff / (float)(sr * 2));
        q = 2.f - 2.f * clamp(resonance, 0.f, 0.98f);
    }
    inline float process(float in, int mode) {   // 0 LP 1 HP 2 BP 3 Notch
        for (int i = 0; i < 2; ++i) {
            low += f * band;
            high = in - low - q * band;
            band += f * high;
            notch = high + low;
        }
        low = flushDenormal(low);
        band = flushDenormal(band);
        switch (mode) {
        case 1: return high;
        case 2: return band;
        case 3: return notch;
        default: return low;
        }
    }
};

// Circular delay line with fractional read
struct DelayLine {
    std::vector<float> buf;
    size_t mask = 0;
    size_t w = 0;
    void allocate(size_t maxSamples) {
        size_t n = 1;
        while (n < maxSamples + 2) n <<= 1;
        buf.assign(n, 0.f);
        mask = n - 1;
        w = 0;
    }
    void clear() { std::fill(buf.begin(), buf.end(), 0.f); }
    inline void push(float x) {
        buf[w] = x;
        w = (w + 1) & mask;
    }
    inline float read(float delaySamples) const {
        delaySamples = clamp(delaySamples, 1.f, (float)(buf.size() - 2));
        float rp = (float)w - delaySamples;
        while (rp < 0) rp += (float)buf.size();
        size_t i0 = (size_t)rp;
        float frac = rp - (float)i0;
        size_t i1 = (i0 + 1) & mask;
        return buf[i0 & mask] + (buf[i1] - buf[i0 & mask]) * frac;
    }
    inline float readInt(int d) const { return buf[(w + buf.size() - (size_t)d) & mask]; }
    size_t capacity() const { return buf.size(); }
};

// Fixed integer delay used for plugin delay compensation
struct PdcDelay {
    std::vector<float> l, r;
    size_t w = 0;
    int delay = 0;
    void set(int samples) {
        delay = std::max(0, samples);
        l.assign(delay + 1, 0.f);
        r.assign(delay + 1, 0.f);
        w = 0;
    }
    inline void process(float* L, float* R, int n) {
        if (delay == 0) return;
        size_t cap = l.size();
        for (int i = 0; i < n; ++i) {
            float ol = l[w], orr = r[w];
            l[w] = L[i];
            r[w] = R[i];
            L[i] = ol;
            R[i] = orr;
            w = (w + 1) % cap;
        }
    }
};

// ADSR envelope (linear attack, exponential-ish decay/release)
struct ADSR {
    enum Stage { Idle, Attack, Decay, Sustain, Release };
    Stage stage = Idle;
    float value = 0.f;
    float a = 0.01f, d = 0.1f, s = 0.8f, r = 0.2f;
    double sr = 44100;
    float releaseFrom = 0.f;
    void set(double sampleRate, float attack, float decay, float sustain, float release) {
        sr = sampleRate;
        a = std::max(attack, 0.0005f);
        d = std::max(decay, 0.0005f);
        s = clamp(sustain, 0.f, 1.f);
        r = std::max(release, 0.0005f);
    }
    void noteOn() { stage = Attack; }
    void noteOff() {
        if (stage != Idle) {
            stage = Release;
            releaseFrom = value;
        }
    }
    void kill() {
        stage = Idle;
        value = 0;
    }
    bool active() const { return stage != Idle; }
    inline float next() {
        switch (stage) {
        case Attack:
            value += 1.f / (a * (float)sr);
            if (value >= 1.f) {
                value = 1.f;
                stage = Decay;
            }
            break;
        case Decay: {
            float rate = 1.f - std::exp(-4.f / (d * (float)sr));
            value += (s - value) * rate;
            if (value <= s + 0.0005f) {
                value = s;
                stage = Sustain;
            }
            break;
        }
        case Sustain: value = s; break;
        case Release: {
            float rate = 1.f - std::exp(-4.f / (r * (float)sr));
            value -= value * rate;
            if (value <= 0.0005f) {
                value = 0;
                stage = Idle;
            }
            break;
        }
        default: value = 0; break;
        }
        return value;
    }
};

// Basic band-limited-ish oscillator using polyBLEP for saw/square
struct Oscillator {
    enum Wave { Sine, Saw, Square, Triangle, Noise, Pulse };
    double phase = 0, inc = 0;
    float pw = 0.5f;
    Rng rng;
    float triState = 0;
    void setFreq(float hz, double sr) { inc = hz / sr; }
    static inline float polyBlep(double t, double dt) {
        if (t < dt) {
            t /= dt;
            return (float)(t + t - t * t - 1.0);
        }
        if (t > 1.0 - dt) {
            t = (t - 1.0) / dt;
            return (float)(t * t + t + t + 1.0);
        }
        return 0.f;
    }
    inline float next(int wave) {
        float out = 0.f;
        double t = phase;
        switch (wave) {
        case Sine: out = std::sin((float)(t * kTwoPi)); break;
        case Saw: out = (float)(2.0 * t - 1.0) - polyBlep(t, inc); break;
        case Square:
        case Pulse: {
            float w = wave == Square ? 0.5f : pw;
            out = t < w ? 1.f : -1.f;
            out += polyBlep(t, inc);
            out -= polyBlep(std::fmod(t + 1.0 - w, 1.0), inc);
            break;
        }
        case Triangle: {
            float sq = t < 0.5 ? 1.f : -1.f;
            sq += polyBlep(t, inc);
            sq -= polyBlep(std::fmod(t + 0.5, 1.0), inc);
            triState = (float)(inc * sq + (1.0 - inc) * triState);
            out = triState * 4.f;
            break;
        }
        case Noise: out = rng.bipolar(); break;
        }
        phase += inc;
        if (phase >= 1.0) phase -= 1.0;
        return out;
    }
};

struct LFO {
    double phase = 0;
    inline float next(double rateHz, double sr, int shape) {   // 0 sine 1 tri 2 saw 3 square 4 s&h
        float v;
        switch (shape) {
        case 1: v = (float)(phase < 0.5 ? phase * 4 - 1 : 3 - phase * 4); break;
        case 2: v = (float)(phase * 2 - 1); break;
        case 3: v = phase < 0.5 ? 1.f : -1.f; break;
        case 4: v = held; break;
        default: v = std::sin((float)(phase * kTwoPi));
        }
        double last = phase;
        phase += rateHz / sr;
        if (phase >= 1.0) {
            phase -= 1.0;
            held = rng.bipolar();
        }
        (void)last;
        return v;
    }
    float held = 0;
    Rng rng;
};

// Peak / RMS envelope follower for dynamics processors
struct EnvelopeFollower {
    float env = 0.f;
    float attCoeff = 0.f, relCoeff = 0.f;
    void set(double sr, float attackMs, float releaseMs) {
        attCoeff = std::exp(-1.f / (float)(sr * std::max(attackMs, 0.01f) * 0.001f));
        relCoeff = std::exp(-1.f / (float)(sr * std::max(releaseMs, 0.1f) * 0.001f));
    }
    inline float process(float x) {
        x = std::fabs(x);
        float c = x > env ? attCoeff : relCoeff;
        env = c * env + (1.f - c) * x;
        return flushDenormal(env);
    }
};

// Fade curve: t in 0..1 -> gain 0..1. shape 0 = linear, >0 = slow start (t^(1+3s)), <0 = fast start (1-(1-t)^(1+3|s|))
inline float fadeShape(float t, float shape) {
    t = clamp(t, 0.f, 1.f);
    if (shape > 0.001f) return std::pow(t, 1.f + 3.f * shape);
    if (shape < -0.001f) return 1.f - std::pow(1.f - t, 1.f - 3.f * shape);
    return t;
}

struct DcBlocker {
    float x1 = 0, y1 = 0;
    inline float process(float x) {
        float y = x - x1 + 0.995f * y1;
        x1 = x;
        y1 = flushDenormal(y);
        return y;
    }
};

// Linear-interpolated sample read helper
inline float readInterp(const float* data, int64_t frames, double pos) {
    if (pos < 0 || pos >= frames - 1) return (pos >= 0 && pos < frames) ? data[(int64_t)pos] : 0.f;
    int64_t i = (int64_t)pos;
    float f = (float)(pos - i);
    return data[i] + (data[i + 1] - data[i]) * f;
}

}   // namespace mt::dsp
