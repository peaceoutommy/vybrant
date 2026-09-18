#include "dsp/instruments/Instruments.h"
#include <QJsonArray>

namespace mt {
using namespace dsp;

static inline void panGains(float pan, float& gl, float& gr) {
    float a = (pan + 1.f) * 0.25f * kPi;
    gl = std::cos(a);
    gr = std::sin(a);
}

// ============================================================ Sampler
Sampler::Sampler() {
    auto& p = m_params;
    p.add(pRoot, "Root Note", 0, 127, 60, ParamUnit::Semitones);
    p.add(pAttack, "Attack", 0.f, 5, 0.002f, ParamUnit::Seconds, true);
    p.add(pDecay, "Decay", 0.f, 5, 0.1f, ParamUnit::Seconds, true);
    p.add(pSustain, "Sustain", 0, 1, 1);
    p.add(pRelease, "Release", 0.f, 8, 0.05f, ParamUnit::Seconds, true);
    p.add(pStart, "Start", 0, 1, 0, ParamUnit::Percent);
    p.addChoice(pLoopMode, "Loop", {"Off", "Forward", "Ping-Pong"});
    p.add(pTune, "Tune", -24, 24, 0, ParamUnit::Semitones);
    p.add(pFine, "Fine", -100, 100, 0, ParamUnit::Cents);
    p.add(pCutoff, "Cutoff", 20, 20000, 20000, ParamUnit::Hertz, true);
    p.add(pReso, "Resonance", 0, 1, 0);
    p.addChoice(pFilterType, "Filter", {"Low-pass", "High-pass", "Band-pass"});
    p.addToggle(pReverse, "Reverse");
    p.add(pLevel, "Level", 0, 2, 1);
    p.add(pPan, "Pan", -1, 1, 0);
    p.add(pVelSens, "Vel. Sensitivity", 0, 1, 1);
    p.add(pPitchBendRange, "Bend Range", 0, 24, 2, ParamUnit::Semitones);
    p.add(pLoopStart, "Loop Start", 0, 1, 0, ParamUnit::Percent);
    p.add(pLoopEnd, "Loop End", 0, 1, 1, ParamUnit::Percent);
    p.add(pGlide, "Glide", 0, 2, 0, ParamUnit::Seconds);
    p.addToggle(pEnvOn, "Envelope", false);
    p.addChoice(pDeclick, "Declicking",
        {"Out only (10 ms)", "Transient (10 ms)", "Transient bleeding (20 ms)", "Generic (20 ms)", "Smooth (100 ms)",
            "Crossfade (200 ms)"});
    m_holders.push_back(&m_sample);
}
void Sampler::onPrepare() { reset(); }
void Sampler::reset() {
    for (auto& v : m_voices) {
        v.active = false;
        v.env.kill();
        v.fl.reset();
        v.fr.reset();
    }
}

// FL Studio's Sampler declicking table: fade-out on any cut (note-off, cut itself, stolen voice), optional fade-in
void Sampler::declickTimes(int mode, float& inSec, float& outSec, bool& transientIn) {
    transientIn = false;
    switch (mode) {
    default:
    case 0:
        inSec = 0.f;
        outSec = 0.010f;
        break;
    case 1:
        inSec = 0.f;
        outSec = 0.010f;
        transientIn = true;
        break;
    case 2:
        inSec = 0.f;
        outSec = 0.020f;
        transientIn = true;
        break;
    case 3:
        inSec = 0.020f;
        outSec = 0.020f;
        break;
    case 4:
        inSec = 0.100f;
        outSec = 0.100f;
        break;
    case 5:
        inSec = 0.200f;
        outSec = 0.200f;
        break;
    }
}
void Sampler::cutVoice(Voice& v) {
    if (!v.active || v.fadeOutPos >= 0) return;
    float in, out;
    bool tr;
    declickTimes(m_params.getInt(pDeclick), in, out, tr);
    v.fadeOutLen = std::max(1, (int)(out * m_sr));
    v.fadeOutPos = 0;
}
void Sampler::noteOn(const MidiEvent& e) {
    const AudioAsset* a = m_sample.get();
    if (!a || a->frames < 2) return;
    Voice* v = nullptr;
    for (auto& vv : m_voices)
        if (!vv.active) {
            v = &vv;
            break;
        }
    if (!v) {
        v = &m_voices[0];
        float best = 2;
        for (auto& vv : m_voices)
            if (vv.env.value < best) {
                best = vv.env.value;
                v = &vv;
            }
    }
    auto& p = m_params;
    {
        float in, out;
        bool tr;
        declickTimes(p.getInt(pDeclick), in, out, tr);
        v->fadeInLen = tr ? 8 : (int)(in * m_sr);
        v->fadeInPos = 0;
        v->fadeOutLen = 0;
        v->fadeOutPos = -1;
    }
    v->active = true;
    v->note = e.data1;
    v->noteId = e.noteId;
    v->vel = 1.f - p.get(pVelSens) * (1.f - e.velocity);
    v->pan = e.pan;
    if (p.getBool(pEnvOn)) v->env.set(m_sr, p.get(pAttack), p.get(pDecay), p.get(pSustain), p.get(pRelease));
    else v->env.set(m_sr, 0.f, 0.f, 1.f, 0.f);   // envelope off: the sound plays as it is
    v->env.noteOn();
    bool rev = p.getBool(pReverse);
    v->dir = rev ? -1 : 1;
    v->pos = rev ? (double)(a->frames - 2) * (1.0 - p.get(pStart)) : (double)a->frames * p.get(pStart);
    float glide = p.get(pGlide);
    v->glideFrom = (glide > 0 && m_lastNote >= 0) ? (float)(m_lastNote - e.data1) : 0.f;
    v->glideT = glide > 0 ? 0.f : 1.f;
    v->fl.reset();
    v->fr.reset();
    m_lastNote = e.data1;
}
// A sequenced note-off carries the id of the note that started the voice: only that voice is released (live MIDI has no id and releases by pitch)
void Sampler::noteOff(const MidiEvent& e) {
    bool envOn = m_params.getBool(pEnvOn);
    if (!envOn && m_params.getInt(pLoopMode) == 0)
        return;   // envelope off: one-shot, the sample plays to its end (looping samples still stop on note-off)
    float in, out;
    bool tr;
    declickTimes(m_params.getInt(pDeclick), in, out, tr);
    for (auto& v : m_voices)
        if (v.active && v.note == e.data1 && v.env.stage != ADSR::Release && (e.noteId < 0 || v.noteId < 0 || v.noteId == e.noteId)) {
            if (envOn && m_params.get(pRelease) >= out) v.env.noteOff();
            else cutVoice(v);
        }   // a release shorter than the declick fade would click: the fade takes over
}

void Sampler::process(float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx) {
    m_sample.rtTick();
    const AudioAsset* a = m_sample.get();
    auto& p = m_params;
    int mi = 0;
    float level = p.get(pLevel);
    float gpl, gpr;
    panGains(p.get(pPan), gpl, gpr);
    float cutoff = p.get(pCutoff), reso = p.get(pReso);
    int ftype = p.getInt(pFilterType);
    bool useFilter = cutoff < 19000.f || ftype != 0;
    int loopMode = p.getInt(pLoopMode);
    float bendRange = p.get(pPitchBendRange);
    for (int i = 0; i < n; ++i) {
        while (mi < midi.count && midi.events[mi].sampleOffset <= i) {
            const auto& e = midi.events[mi++];
            if (e.type == MidiEvent::NoteOn) noteOn(e);
            else if (e.type == MidiEvent::NoteOff) noteOff(e);
            else if (e.type == MidiEvent::AllNotesOff) {
                for (auto& v : m_voices) {
                    if (e.data1 == 1) cutVoice(v); /* choke ('Cut itself'): the declick fade-out */
                    else v.env.noteOff();
                }
            } else if (e.type == MidiEvent::PitchBend) m_bend = e.pitchBend / 8192.f * bendRange;
            else if (e.type == MidiEvent::CC && e.data1 == 1) m_modWheel = e.data2 / 127.f;
        }
        if (!a) continue;
        float sl = 0, sr = 0;
        for (auto& v : m_voices) {
            if (!v.active) continue;
            float env = v.env.next();
            if (!v.env.active()) {
                v.active = false;
                continue;
            }
            float glideSemi = 0;
            if (v.glideT < 1.f) {
                v.glideT += 1.f / (p.get(pGlide) * (float)m_sr);
                glideSemi = v.glideFrom * (1.f - std::min(v.glideT, 1.f));
            }
            double semis = (v.note - p.get(pRoot)) + p.get(pTune) + p.get(pFine) / 100.0 + m_bend + glideSemi;
            v.inc = std::pow(2.0, semis / 12.0) * (a->sampleRate / m_sr);
            int64_t ls = (int64_t)(p.get(pLoopStart) * a->frames), le = std::max<int64_t>(ls + 2, (int64_t)(p.get(pLoopEnd) * a->frames));
            float dg = 1.f;
            if (v.fadeInPos < v.fadeInLen) {
                dg = 0.5f - 0.5f * std::cos(3.14159265f * (float)v.fadeInPos / (float)v.fadeInLen);
                ++v.fadeInPos;
            }
            if (v.fadeOutPos >= 0) {
                if (v.fadeOutPos >= v.fadeOutLen) {
                    v.active = false;
                    v.env.kill();
                    continue;
                }
                dg *= 0.5f + 0.5f * std::cos(3.14159265f * (float)v.fadeOutPos / (float)v.fadeOutLen);
                ++v.fadeOutPos;
            }
            float xl = readInterp(a->L(), a->frames, v.pos) * env * v.vel * dg,
                  xr = readInterp(a->R(), a->frames, v.pos) * env * v.vel * dg;
            if (useFilter) {
                v.fl.set(m_sr, cutoff, reso);
                v.fr.set(m_sr, cutoff, reso);
                xl = v.fl.process(xl, ftype);
                xr = v.fr.process(xr, ftype);
            }
            float vl, vr;
            panGains(v.pan, vl, vr);
            sl += xl * vl * 1.4142f;
            sr += xr * vr * 1.4142f;
            v.pos += v.inc * v.dir;
            if (loopMode == 1) {
                if (v.dir > 0 && v.pos >= le) v.pos = ls + (v.pos - le);
                if (v.dir < 0 && v.pos <= ls) v.pos = le - (ls - v.pos);
            } else if (loopMode == 2) {
                if (v.pos >= le) {
                    v.pos = le - 1;
                    v.dir = -1;
                }
                if (v.pos <= ls) {
                    v.pos = ls + 1;
                    v.dir = 1;
                }
            } else if (v.pos >= a->frames - 1 || v.pos < 0) {
                v.active = false;
                v.env.kill();
            }
        }
        L[i] += sl * level * gpl * 1.4142f;
        R[i] += sr * level * gpr * 1.4142f;
    }
}
QJsonObject Sampler::saveState() const {
    auto o = IProcessor::saveState();
    o["sample"] = m_path;
    return o;
}
void Sampler::restoreState(const QJsonObject& s) {
    IProcessor::restoreState(s);
    pendingPath = s["sample"].toString();
    // projects saved before the envelope switch existed: keep their sound by enabling it when the envelope was clearly in use
    auto p = s["params"].toObject();
    if (!p.contains(QString::number(pEnvOn))) {
        auto& ps = m_params;
        bool used = false;
        for (uint32_t id : {(uint32_t)pAttack, (uint32_t)pDecay, (uint32_t)pSustain, (uint32_t)pRelease})
            if (std::fabs(ps.get(id) - ps.desc(ps.indexOf(id)).def) > 1e-5f) used = true;
        if (used) ps.set(pEnvOn, 1.f);
    }
}

// ============================================================ DrumSampler
DrumSampler::DrumSampler() {
    auto& p = m_params;
    p.add(pLevel, "Level", 0, 2, 1);
    p.add(pRoot, "Root Note", 0, 127, 60, ParamUnit::Semitones);
    p.add(pAttack, "Attack", 0.f, 2, 0.001f, ParamUnit::Seconds, true);
    p.add(pDecay, "Decay", 0.f, 10, 10, ParamUnit::Seconds, true);
    p.add(pRelease, "Release", 0.f, 4, 0.02f, ParamUnit::Seconds, true);
    p.add(pPitch, "Pitch", -24, 24, 0, ParamUnit::Semitones);
    p.add(pCutoff, "Cutoff", 20, 20000, 20000, ParamUnit::Hertz, true);
    p.add(pVelSens, "Vel. Sensitivity", 0, 1, 1);
    p.add(pPan, "Pan", -1, 1, 0);
    for (int i = 0; i < kPads; ++i) {
        std::string nm = "Pad " + std::to_string(i + 1) + " ";
        p.add(padParam(i, 0), (nm + "Level").c_str(), 0, 2, 1);
        p.add(padParam(i, 1), (nm + "Pan").c_str(), -1, 1, 0);
        p.add(padParam(i, 2), (nm + "Pitch").c_str(), -24, 24, 0, ParamUnit::Semitones);
        p.add(padParam(i, 3), (nm + "Decay").c_str(), 0.01f, 10, 10, ParamUnit::Seconds, true);
        p.add(padParam(i, 4), (nm + "Choke").c_str(), 0, 4, 0);
        p.addToggle(padParam(i, 5), (nm + "Reverse").c_str());
        p.add(padParam(i, 6), (nm + "Start").c_str(), 0, 1, 0, ParamUnit::Percent);
        p.add(padParam(i, 7), (nm + "Tune").c_str(), -100, 100, 0, ParamUnit::Cents);
        m_holders.push_back(&m_pads[i].sample);
    }
}
void DrumSampler::onPrepare() {
    reset();
    if (!m_builtin.isEmpty()) setBuiltin(m_builtin);
}
void DrumSampler::reset() {
    for (auto& v : m_voices) {
        v.active = false;
        v.env.kill();
    }
}

std::shared_ptr<AudioAsset> DrumSampler::synthesizeDrum(const QString& name, double sr) {
    auto a = std::make_shared<AudioAsset>();
    a->path = "builtin:" + name;
    a->sampleRate = sr;
    a->originalSampleRate = sr;
    a->channels = 1;
    double len = 0.6;
    Rng rng;
    rng.s = 12345;
    QString n = name.toLower();
    if (n.contains("hat")) len = 0.12;
    else if (n.contains("clap")) len = 0.3;
    else if (n.contains("snare")) len = 0.25;
    int64_t frames = (int64_t)(len * sr);
    a->frames = frames;
    a->left.resize(frames);
    Biquad hp, bp;
    hp.set(Biquad::HighPass, sr, 6000, 0.7f);
    bp.set(Biquad::BandPass, sr, 1800, 1.2f);
    double ph = 0;
    for (int64_t i = 0; i < frames; ++i) {
        double t = i / sr;
        float s = 0;
        if (n.contains("kick")) {
            double f = 40 + 120 * std::exp(-t * 28);
            ph += f / sr;
            s = (float)std::sin(ph * 2 * M_PI) * (float)std::exp(-t * 7) * 1.1f;
            s = fastTanh(s * 1.8f);
            if (t < 0.004) s += rng.bipolar() * 0.3f * (float)(1 - t / 0.004);
        } else if (n.contains("hat")) {
            s = hp.process(rng.bipolar()) * (float)std::exp(-t * 45) * 0.7f;
        } else if (n.contains("clap")) {
            float e = 0;
            for (int k = 0; k < 4; ++k) {
                double tt = t - k * 0.008;
                if (tt >= 0) e += (float)std::exp(-tt * (k == 3 ? 12 : 90));
            }
            s = bp.process(rng.bipolar()) * e * 0.8f;
        } else if (n.contains("snare")) {
            double f = 180 * std::exp(-t * 10) + 120;
            ph += f / sr;
            float tone = (float)std::sin(ph * 2 * M_PI) * (float)std::exp(-t * 18);
            float noise = hp.process(rng.bipolar()) * (float)std::exp(-t * 14);
            s = tone * 0.6f + noise * 0.9f;
        } else {
            double f = 220;
            ph += f / sr;
            s = (float)std::sin(ph * 2 * M_PI) * (float)std::exp(-t * 6);
        }
        a->left[i] = clamp(s * 0.8f, -1.f, 1.f);
    }
    a->buildPeaks();
    return a;
}
void DrumSampler::setBuiltin(const QString& name) {
    m_builtin = name;
    m_pads[0].sample.set(synthesizeDrum(name, m_sr));
    m_pads[0].path = "builtin:" + name;
}

void DrumSampler::trigger(const MidiEvent& e) {
    auto& p = m_params;
    int root = p.getInt(pRoot);
    int pad = e.data1 - 36;   // C1 = pad 1
    bool padHit = pad >= 0 && pad < kPads && m_pads[pad].sample.get();
    if (!padHit) pad = 0;
    const AudioAsset* a = m_pads[pad].sample.get();
    if (!a || a->frames < 2) return;
    int choke = p.getInt(padParam(pad, 4));
    if (choke > 0)
        for (auto& v : m_voices)
            if (v.active && v.pad >= 0 && p.getInt(padParam(v.pad, 4)) == choke) v.env.noteOff();
    Voice* v = nullptr;
    for (auto& vv : m_voices)
        if (!vv.active) {
            v = &vv;
            break;
        }
    if (!v) {
        v = &m_voices[0];
        for (auto& vv : m_voices)
            if (vv.pos > v->pos) v = &vv;
    }
    v->active = true;
    v->pad = pad;
    v->note = e.data1;
    v->vel = 1.f - p.get(pVelSens) * (1.f - e.velocity);
    float decay = std::min(p.get(pDecay), p.get(padParam(pad, 3)));
    v->env.set(m_sr, p.get(pAttack), decay, 0.f, p.get(pRelease));
    v->env.noteOn();
    bool rev = p.getBool(padParam(pad, 5));
    float start = p.get(padParam(pad, 6));
    v->pos = rev ? (a->frames - 2) * (1.0 - start) : a->frames * start;
    double semis = p.get(pPitch) + p.get(padParam(pad, 2)) + p.get(padParam(pad, 7)) / 100.0 + (padHit ? 0 : (e.data1 - root));
    v->inc = std::pow(2.0, semis / 12.0) * (a->sampleRate / m_sr) * (rev ? -1 : 1);
    v->pan = clamp(e.pan + p.get(padParam(pad, 1)), -1.f, 1.f);
    float cutoff = p.get(pCutoff);
    v->fl.set(Biquad::LowPass, m_sr, cutoff, 0.7f);
    v->fr = v->fl;
}

void DrumSampler::process(float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx) {
    for (auto& pd : m_pads) pd.sample.rtTick();
    auto& p = m_params;
    int mi = 0;
    float level = p.get(pLevel);
    float gpl, gpr;
    panGains(p.get(pPan), gpl, gpr);
    bool filt = p.get(pCutoff) < 19000.f;
    for (int i = 0; i < n; ++i) {
        while (mi < midi.count && midi.events[mi].sampleOffset <= i) {
            const auto& e = midi.events[mi++];
            if (e.type == MidiEvent::NoteOn) trigger(e);
            else if (e.type == MidiEvent::AllNotesOff)
                for (auto& v : m_voices) {
                    if (e.data1 == 1) v.env.r = 0.006f;
                    v.env.noteOff();
                }
        }
        float sl = 0, sr = 0;
        for (auto& v : m_voices) {
            if (!v.active) continue;
            const AudioAsset* a = m_pads[v.pad].sample.get();
            if (!a) {
                v.active = false;
                continue;
            }
            float env = v.env.next();
            if (!v.env.active() || v.pos >= a->frames - 1 || v.pos < 0) {
                v.active = false;
                continue;
            }
            float g = env * v.vel * p.get(padParam(v.pad, 0));
            float xl = readInterp(a->L(), a->frames, v.pos) * g, xr = readInterp(a->R(), a->frames, v.pos) * g;
            if (filt) {
                xl = v.fl.process(xl);
                xr = v.fr.process(xr);
            }
            float vl, vr;
            panGains(v.pan, vl, vr);
            sl += xl * vl * 1.4142f;
            sr += xr * vr * 1.4142f;
            v.pos += v.inc;
        }
        L[i] += sl * level * gpl * 1.4142f;
        R[i] += sr * level * gpr * 1.4142f;
    }
}
QJsonObject DrumSampler::saveState() const {
    auto o = IProcessor::saveState();
    QJsonArray pads;
    for (auto& pd : m_pads) pads.append(pd.path);
    o["pads"] = pads;
    o["builtin"] = m_builtin;
    return o;
}
void DrumSampler::restoreState(const QJsonObject& s) {
    IProcessor::restoreState(s);
    m_builtin = s["builtin"].toString();
    auto pads = s["pads"].toArray();
    for (int i = 0; i < kPads && i < pads.size(); ++i) m_pads[i].path = pads[i].toString();
    if (!m_builtin.isEmpty() && (pads.isEmpty() || pads[0].toString().startsWith("builtin:"))) setBuiltin(m_builtin);
}

// ============================================================ Synth
static const std::vector<std::string> kWaves = {"Sine", "Saw", "Square", "Triangle", "Noise", "Pulse"};
Synth::Synth() {
    auto& p = m_params;
    p.addChoice(pOsc1Wave, "Osc 1 Wave", kWaves, 1);
    p.add(pOsc1Level, "Osc 1 Level", 0, 1, 0.8f);
    p.add(pOsc1Octave, "Osc 1 Octave", -3, 3, 0);
    p.add(pOsc1Semi, "Osc 1 Semi", -12, 12, 0, ParamUnit::Semitones);
    p.add(pOsc1Fine, "Osc 1 Fine", -100, 100, 0, ParamUnit::Cents);
    p.add(pOsc1PW, "Osc 1 PW", 0.05f, 0.95f, 0.5f);
    p.addChoice(pOsc2Wave, "Osc 2 Wave", kWaves, 1);
    p.add(pOsc2Level, "Osc 2 Level", 0, 1, 0.0f);
    p.add(pOsc2Octave, "Osc 2 Octave", -3, 3, -1);
    p.add(pOsc2Semi, "Osc 2 Semi", -12, 12, 0, ParamUnit::Semitones);
    p.add(pOsc2Fine, "Osc 2 Fine", -100, 100, 7, ParamUnit::Cents);
    p.add(pOsc2PW, "Osc 2 PW", 0.05f, 0.95f, 0.5f);
    p.add(pUnison, "Unison", 1, 8, 1);
    p.add(pDetune, "Detune", 0, 100, 12, ParamUnit::Cents);
    p.add(pNoise, "Noise", 0, 1, 0);
    p.add(pSub, "Sub Osc", 0, 1, 0);
    p.addChoice(pFilterType, "Filter", {"Low-pass", "High-pass", "Band-pass", "Notch"});
    p.add(pCutoff, "Cutoff", 20, 20000, 8000, ParamUnit::Hertz, true);
    p.add(pReso, "Resonance", 0, 1, 0.2f);
    p.add(pFilterEnv, "Filter Env", -1, 1, 0.4f);
    p.add(pFilterKeyTrack, "Key Track", 0, 1, 0.5f);
    p.add(pFA, "F. Attack", 0.001f, 5, 0.01f, ParamUnit::Seconds, true);
    p.add(pFD, "F. Decay", 0.001f, 5, 0.3f, ParamUnit::Seconds, true);
    p.add(pFS, "F. Sustain", 0, 1, 0.3f);
    p.add(pFR, "F. Release", 0.001f, 8, 0.3f, ParamUnit::Seconds, true);
    p.add(pAA, "Attack", 0.f, 5, 0.005f, ParamUnit::Seconds, true);
    p.add(pAD, "Decay", 0.f, 5, 0.2f, ParamUnit::Seconds, true);
    p.add(pAS, "Sustain", 0, 1, 0.7f);
    p.add(pAR, "Release", 0.f, 8, 0.25f, ParamUnit::Seconds, true);
    p.add(pLfoRate, "LFO Rate", 0.01f, 30, 4, ParamUnit::Hertz, true);
    p.addChoice(pLfoShape, "LFO Shape", {"Sine", "Triangle", "Saw", "Square", "S&H"});
    p.add(pLfoPitch, "LFO > Pitch", 0, 12, 0, ParamUnit::Semitones);
    p.add(pLfoFilter, "LFO > Filter", 0, 1, 0);
    p.addToggle(pLfoSync, "LFO Sync");
    p.add(pGlide, "Glide", 0, 2, 0, ParamUnit::Seconds);
    p.add(pLevel, "Level", 0, 2, 0.6f);
    p.add(pVoices, "Polyphony", 1, 16, 8);
    p.addToggle(pMono, "Mono");
    p.add(pSpread, "Stereo Spread", 0, 1, 0.5f);
    p.add(pBendRange, "Bend Range", 0, 24, 2, ParamUnit::Semitones);
    p.add(pDrive, "Drive", 0, 1, 0);
}
void Synth::onPrepare() { reset(); }
void Synth::reset() {
    for (auto& v : m_voices) {
        v.active = false;
        v.amp.kill();
        v.fenv.kill();
        v.fl.reset();
        v.fr.reset();
    }
}
Synth::Voice* Synth::allocVoice() {
    int maxV = m_params.getBool(pMono) ? 1 : m_params.getInt(pVoices);
    int activeCount = 0;
    Voice* oldest = nullptr;
    Voice* free = nullptr;
    for (auto& v : m_voices) {
        if (v.active) {
            activeCount++;
            if (!oldest || v.age < oldest->age) oldest = &v;
        } else if (!free) free = &v;
    }
    if (activeCount >= maxV || !free) return oldest ? oldest : &m_voices[0];
    return free;
}
void Synth::noteOn(const MidiEvent& e) {
    auto& p = m_params;
    Voice* v = allocVoice();
    bool retrig = !v->active || !p.getBool(pMono);
    float target = midiToHz(e.data1);
    float glide = p.get(pGlide);
    v->glideFrom = (glide > 0 && m_lastNote >= 0) ? (v->active ? v->freq : midiToHz(m_lastNote)) : target;
    v->glideT = glide > 0 ? 0.f : 1.f;
    v->active = true;
    v->note = e.data1;
    v->vel = e.velocity;
    v->freq = target;
    v->age = m_counter++;
    if (retrig) {
        v->amp.set(m_sr, p.get(pAA), p.get(pAD), p.get(pAS), p.get(pAR));
        v->amp.noteOn();
        v->fenv.set(m_sr, p.get(pFA), p.get(pFD), p.get(pFS), p.get(pFR));
        v->fenv.noteOn();
        Rng r;
        r.s = 1000 + e.data1 * 7 + (uint32_t)m_counter;
        for (int i = 0; i < 8; ++i) {
            v->o1[i].phase = r.uniform();
            v->o2[i].phase = r.uniform();
        }
    }
    m_lastNote = e.data1;
}
void Synth::noteOff(const MidiEvent& e) {
    for (auto& v : m_voices)
        if (v.active && v.note == e.data1) {
            v.amp.noteOff();
            v.fenv.noteOff();
        }
}

void Synth::process(float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx) {
    auto& p = m_params;
    int mi = 0;
    int w1 = p.getInt(pOsc1Wave), w2 = p.getInt(pOsc2Wave);
    float l1 = p.get(pOsc1Level), l2 = p.get(pOsc2Level), noiseL = p.get(pNoise), subL = p.get(pSub);
    int unison = p.getInt(pUnison);
    float detune = p.get(pDetune) / 100.f;
    float spread = p.get(pSpread);
    double o1mult = std::pow(2.0, p.get(pOsc1Octave) + (p.get(pOsc1Semi) + p.get(pOsc1Fine) / 100.0) / 12.0),
           o2mult = std::pow(2.0, p.get(pOsc2Octave) + (p.get(pOsc2Semi) + p.get(pOsc2Fine) / 100.0) / 12.0);
    float pw1 = p.get(pOsc1PW), pw2 = p.get(pOsc2PW);
    int ftype = p.getInt(pFilterType);
    float cutoff = p.get(pCutoff), reso = p.get(pReso), fenvAmt = p.get(pFilterEnv), keytrack = p.get(pFilterKeyTrack);
    float lfoRate = p.get(pLfoRate);
    if (p.getBool(pLfoSync)) lfoRate = (float)(ctx.bpm / 60.0) * std::max(0.25f, std::round(lfoRate)) / 4.f;
    int lfoShape = p.getInt(pLfoShape);
    float lfoPitch = p.get(pLfoPitch), lfoFilt = p.get(pLfoFilter);
    float level = p.get(pLevel), drive = p.get(pDrive), bendRange = p.get(pBendRange), glide = p.get(pGlide);
    for (int i = 0; i < n; ++i) {
        while (mi < midi.count && midi.events[mi].sampleOffset <= i) {
            const auto& e = midi.events[mi++];
            if (e.type == MidiEvent::NoteOn) noteOn(e);
            else if (e.type == MidiEvent::NoteOff) noteOff(e);
            else if (e.type == MidiEvent::AllNotesOff)
                for (auto& v : m_voices) {
                    if (e.data1 == 1) v.amp.r = 0.006f;
                    v.amp.noteOff();
                    v.fenv.noteOff();
                }
            else if (e.type == MidiEvent::PitchBend) m_bend = e.pitchBend / 8192.f * bendRange;
            else if (e.type == MidiEvent::CC && e.data1 == 1) m_modWheel = e.data2 / 127.f;
        }
        float lfo = m_lfo.next(lfoRate, m_sr, lfoShape);
        float sl = 0, sr = 0;
        for (auto& v : m_voices) {
            if (!v.active) continue;
            float amp = v.amp.next();
            if (!v.amp.active()) {
                v.active = false;
                continue;
            }
            float fe = v.fenv.next();
            float f = v.freq;
            if (v.glideT < 1.f) {
                v.glideT += 1.f / (glide * (float)m_sr);
                f = v.glideFrom * std::pow(v.freq / v.glideFrom, std::min(v.glideT, 1.f));
            }
            f *= std::pow(2.f, (m_bend + lfo * lfoPitch * (0.3f + 0.7f * m_modWheel + (lfoPitch > 0 ? 0 : 0))) / 12.f);
            float vl = 0, vr = 0;
            for (int u = 0; u < unison; ++u) {
                float d = unison > 1 ? ((float)u / (unison - 1) - 0.5f) * 2.f : 0.f;
                float mult = std::pow(2.f, d * detune / 12.f);
                float panL = 0.5f + d * spread * 0.5f, panR = 0.5f - d * spread * 0.5f;
                v.o1[u].pw = pw1;
                v.o2[u].pw = pw2;
                v.o1[u].setFreq((float)(f * o1mult * mult), m_sr);
                v.o2[u].setFreq((float)(f * o2mult * mult), m_sr);
                float s = v.o1[u].next(w1) * l1 + v.o2[u].next(w2) * l2;
                vl += s * panL;
                vr += s * panR;
            }
            float norm = 1.f / std::sqrt((float)unison);
            vl *= norm;
            vr *= norm;
            if (subL > 0) {
                v.sub.setFreq(f * 0.5f, m_sr);
                float s = v.sub.next(Oscillator::Sine) * subL;
                vl += s;
                vr += s;
            }
            if (noiseL > 0) {
                float s = v.noise.next(Oscillator::Noise) * noiseL;
                vl += s;
                vr += s;
            }
            float fc = cutoff * std::pow(2.f, fenvAmt * fe * 6.f + lfo * lfoFilt * 3.f + keytrack * (v.note - 60) / 12.f);
            v.fl.set(m_sr, fc, reso);
            v.fr.set(m_sr, fc, reso);
            vl = v.fl.process(vl, ftype);
            vr = v.fr.process(vr, ftype);
            if (drive > 0) {
                float g = 1.f + drive * 8.f;
                vl = fastTanh(vl * g) / std::sqrt(g);
                vr = fastTanh(vr * g) / std::sqrt(g);
            }
            sl += vl * amp * v.vel;
            sr += vr * amp * v.vel;
        }
        L[i] += sl * level * 0.5f;
        R[i] += sr * level * 0.5f;
    }
}

// ============================================================ WavetableSynth
WavetableSynth::WavetableSynth() {
    auto& p = m_params;
    p.add(pPosition, "Position", 0, 1, 0.2f);
    p.add(pPosEnv, "Pos < Env", -1, 1, 0.3f);
    p.add(pPosLfo, "Pos < LFO", 0, 1, 0);
    p.add(pOctave, "Octave", -3, 3, 0);
    p.add(pSemi, "Semi", -12, 12, 0, ParamUnit::Semitones);
    p.add(pFine, "Fine", -100, 100, 0, ParamUnit::Cents);
    p.add(pUnison, "Unison", 1, 8, 2);
    p.add(pDetune, "Detune", 0, 100, 10, ParamUnit::Cents);
    p.addChoice(pFilterType, "Filter", {"Low-pass", "High-pass", "Band-pass", "Notch"});
    p.add(pCutoff, "Cutoff", 20, 20000, 12000, ParamUnit::Hertz, true);
    p.add(pReso, "Resonance", 0, 1, 0.1f);
    p.add(pFilterEnv, "Filter Env", -1, 1, 0.2f);
    p.add(pFA, "F. Attack", 0.001f, 5, 0.01f, ParamUnit::Seconds, true);
    p.add(pFD, "F. Decay", 0.001f, 5, 0.4f, ParamUnit::Seconds, true);
    p.add(pFS, "F. Sustain", 0, 1, 0.5f);
    p.add(pFR, "F. Release", 0.001f, 8, 0.3f, ParamUnit::Seconds, true);
    p.add(pAA, "Attack", 0.f, 5, 0.01f, ParamUnit::Seconds, true);
    p.add(pAD, "Decay", 0.f, 5, 0.3f, ParamUnit::Seconds, true);
    p.add(pAS, "Sustain", 0, 1, 0.8f);
    p.add(pAR, "Release", 0.f, 8, 0.3f, ParamUnit::Seconds, true);
    p.add(pLfoRate, "LFO Rate", 0.01f, 30, 1, ParamUnit::Hertz, true);
    p.addChoice(pLfoShape, "LFO Shape", {"Sine", "Triangle", "Saw", "Square", "S&H"});
    p.add(pLevel, "Level", 0, 2, 0.6f);
    p.addChoice(pBank, "Bank", {"Basic", "Harmonic", "Digital"});
    p.add(pGlide, "Glide", 0, 2, 0, ParamUnit::Seconds);
    p.add(pSpread, "Stereo Spread", 0, 1, 0.5f);
    buildTables();
}
void WavetableSynth::buildTables() {
    for (int b = 0; b < 3; ++b) {
        m_tables[b].assign(kTables, std::vector<float>(kTableSize));
        for (int t = 0; t < kTables; ++t) {
            auto& tb = m_tables[b][t];
            float m = (float)t / (kTables - 1);
            for (int i = 0; i < kTableSize; ++i) {
                double ph = (double)i / kTableSize;
                float s = 0;
                if (b == 0) {   // sine -> triangle -> saw -> square morph
                    float sine = std::sin((float)(ph * kTwoPi));
                    float tri = ph < 0.5 ? (float)(ph * 4 - 1) : (float)(3 - ph * 4);
                    float saw = (float)(2 * ph - 1);
                    float sq = ph < 0.5 ? 1.f : -1.f;
                    float x = m * 3;
                    if (x < 1) s = sine * (1 - x) + tri * x;
                    else if (x < 2) s = tri * (2 - x) + saw * (x - 1);
                    else s = saw * (3 - x) + sq * (x - 2);
                } else if (b == 1) {   // additive: growing harmonic content with odd/even balance
                    int harm = 1 + (int)(m * 24);
                    for (int h = 1; h <= harm; ++h) {
                        float a = 1.f / h * (h % 2 == 0 ? (0.3f + 0.7f * m) : 1.f);
                        s += a * std::sin((float)(ph * kTwoPi * h));
                    }
                    s *= 0.6f;
                } else {   // digital: formant / bell-ish
                    float f1 = std::sin((float)(ph * kTwoPi));
                    float f2 = std::sin((float)(ph * kTwoPi * (2 + (int)(m * 5))));
                    float f3 = std::sin((float)(ph * kTwoPi * (5 + (int)(m * 11))));
                    s = f1 * 0.6f + f2 * 0.3f * m + f3 * 0.2f * m * m;
                    s = std::sin(s * (1.f + m * 2.5f));
                }
                tb[i] = s;
            }
            float pk = 0;
            for (float v : tb) pk = std::max(pk, std::fabs(v));
            if (pk > 0)
                for (auto& v : tb) v /= pk;
        }
    }
}
inline float WavetableSynth::readTable(int bank, float pos, double phase) const {
    float fp = clamp(pos, 0.f, 0.9999f) * (kTables - 1);
    int t0 = (int)fp;
    float ft = fp - t0;
    int t1 = std::min(t0 + 1, kTables - 1);
    double ip = phase * kTableSize;
    int i0 = (int)ip & (kTableSize - 1);
    int i1 = (i0 + 1) & (kTableSize - 1);
    float fr = (float)(ip - (int)ip);
    const auto& a = m_tables[bank][t0];
    const auto& b = m_tables[bank][t1];
    float va = a[i0] + (a[i1] - a[i0]) * fr, vb = b[i0] + (b[i1] - b[i0]) * fr;
    return va + (vb - va) * ft;
}
void WavetableSynth::onPrepare() { reset(); }
void WavetableSynth::reset() {
    for (auto& v : m_voices) {
        v.active = false;
        v.amp.kill();
        v.fenv.kill();
    }
}
void WavetableSynth::process(float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx) {
    auto& p = m_params;
    int mi = 0;
    int bank = p.getInt(pBank);
    float pos = p.get(pPosition), posEnv = p.get(pPosEnv), posLfo = p.get(pPosLfo);
    double mult = std::pow(2.0, p.get(pOctave) + (p.get(pSemi) + p.get(pFine) / 100.0) / 12.0);
    int unison = p.getInt(pUnison);
    float detune = p.get(pDetune) / 100.f, spread = p.get(pSpread);
    int ftype = p.getInt(pFilterType);
    float cutoff = p.get(pCutoff), reso = p.get(pReso), fenvAmt = p.get(pFilterEnv);
    float lfoRate = p.get(pLfoRate);
    int lfoShape = p.getInt(pLfoShape);
    float level = p.get(pLevel), glide = p.get(pGlide);
    for (int i = 0; i < n; ++i) {
        while (mi < midi.count && midi.events[mi].sampleOffset <= i) {
            const auto& e = midi.events[mi++];
            if (e.type == MidiEvent::NoteOn) {
                Voice* v = nullptr;
                for (auto& vv : m_voices)
                    if (!vv.active) {
                        v = &vv;
                        break;
                    }
                if (!v) {
                    v = &m_voices[0];
                    for (auto& vv : m_voices)
                        if (vv.age < v->age) v = &vv;
                }
                float target = midiToHz(e.data1);
                v->glideFrom = (glide > 0 && m_lastNote >= 0) ? midiToHz(m_lastNote) : target;
                v->glideT = glide > 0 ? 0.f : 1.f;
                v->active = true;
                v->note = e.data1;
                v->vel = e.velocity;
                v->freq = target;
                v->age = m_counter++;
                v->amp.set(m_sr, p.get(pAA), p.get(pAD), p.get(pAS), p.get(pAR));
                v->amp.noteOn();
                v->fenv.set(m_sr, p.get(pFA), p.get(pFD), p.get(pFS), p.get(pFR));
                v->fenv.noteOn();
                Rng r;
                r.s = 77 + e.data1 + (uint32_t)m_counter;
                for (int u = 0; u < 8; ++u) v->phase[u] = r.uniform();
                m_lastNote = e.data1;
            } else if (e.type == MidiEvent::NoteOff) {
                for (auto& v : m_voices)
                    if (v.active && v.note == e.data1) {
                        v.amp.noteOff();
                        v.fenv.noteOff();
                    }
            } else if (e.type == MidiEvent::AllNotesOff)
                for (auto& v : m_voices) {
                    if (e.data1 == 1) v.amp.r = 0.006f;
                    v.amp.noteOff();
                    v.fenv.noteOff();
                }
            else if (e.type == MidiEvent::PitchBend) m_bend = e.pitchBend / 8192.f * 2.f;
        }
        float lfo = m_lfo.next(lfoRate, m_sr, lfoShape);
        float sl = 0, sr = 0;
        for (auto& v : m_voices) {
            if (!v.active) continue;
            float amp = v.amp.next();
            if (!v.amp.active()) {
                v.active = false;
                continue;
            }
            float fe = v.fenv.next();
            float f = v.freq;
            if (v.glideT < 1.f) {
                v.glideT += 1.f / (glide * (float)m_sr);
                f = v.glideFrom * std::pow(v.freq / v.glideFrom, std::min(v.glideT, 1.f));
            }
            f *= std::pow(2.f, m_bend / 12.f) * (float)mult;
            float wpos = clamp(pos + posEnv * fe + posLfo * lfo * 0.5f, 0.f, 1.f);
            float vl = 0, vr = 0;
            for (int u = 0; u < unison; ++u) {
                float d = unison > 1 ? ((float)u / (unison - 1) - 0.5f) * 2.f : 0.f;
                float fu = f * std::pow(2.f, d * detune / 12.f);
                float s = readTable(bank, wpos, v.phase[u]);
                v.phase[u] += fu / m_sr;
                if (v.phase[u] >= 1.0) v.phase[u] -= 1.0;
                vl += s * (0.5f + d * spread * 0.5f);
                vr += s * (0.5f - d * spread * 0.5f);
            }
            float norm = 1.f / std::sqrt((float)unison);
            vl *= norm;
            vr *= norm;
            float fc = cutoff * std::pow(2.f, fenvAmt * fe * 6.f);
            v.fl.set(m_sr, fc, reso);
            v.fr.set(m_sr, fc, reso);
            vl = v.fl.process(vl, ftype);
            vr = v.fr.process(vr, ftype);
            sl += vl * amp * v.vel;
            sr += vr * amp * v.vel;
        }
        L[i] += sl * level * 0.5f;
        R[i] += sr * level * 0.5f;
    }
}

// ============================================================ FMSynth
FMSynth::FMSynth() {
    auto& p = m_params;
    p.addChoice(
        pAlgorithm, "Algorithm", {"1>2>3>4", "(1+2)>3>4", "1>2, 3>4", "1>(2+3+4)", "(1>2)+(3>4)", "1>2>3, 4", "1+2+3+4", "1>2>(3+4)"});
    p.add(pFeedback, "Feedback", 0, 1, 0.1f);
    p.add(pLevel, "Level", 0, 2, 0.6f);
    p.add(pAA, "Attack", 0.f, 5, 0.005f, ParamUnit::Seconds, true);
    p.add(pAD, "Decay", 0.f, 5, 0.5f, ParamUnit::Seconds, true);
    p.add(pAS, "Sustain", 0, 1, 0.6f);
    p.add(pAR, "Release", 0.f, 8, 0.4f, ParamUnit::Seconds, true);
    p.add(pLfoRate, "LFO Rate", 0.01f, 30, 5, ParamUnit::Hertz, true);
    p.add(pLfoDepth, "Vibrato", 0, 1, 0);
    p.add(pGlide, "Glide", 0, 2, 0, ParamUnit::Seconds);
    p.add(pBendRange, "Bend Range", 0, 24, 2, ParamUnit::Semitones);
    const float defRatio[4] = {1, 2, 1, 3};
    const float defLevel[4] = {1, 0.5f, 0.3f, 0.2f};
    for (int o = 0; o < 4; ++o) {
        std::string nm = "Op " + std::to_string(o + 1) + " ";
        p.add(opParam(o, 0), (nm + "Ratio").c_str(), 0.25f, 16, defRatio[o], ParamUnit::Ratio);
        p.add(opParam(o, 1), (nm + "Level").c_str(), 0, 1, defLevel[o]);
        p.add(opParam(o, 2), (nm + "Detune").c_str(), -100, 100, 0, ParamUnit::Cents);
        p.add(opParam(o, 3), (nm + "Attack").c_str(), 0.001f, 5, 0.005f, ParamUnit::Seconds, true);
        p.add(opParam(o, 4), (nm + "Decay").c_str(), 0.001f, 5, 0.6f, ParamUnit::Seconds, true);
        p.add(opParam(o, 5), (nm + "Sustain").c_str(), 0, 1, o == 0 ? 0.8f : 0.3f);
        p.add(opParam(o, 6), (nm + "Release").c_str(), 0.001f, 8, 0.3f, ParamUnit::Seconds, true);
        p.addToggle(opParam(o, 7), (nm + "Fixed").c_str());
    }
}
void FMSynth::onPrepare() { reset(); }
void FMSynth::reset() {
    for (auto& v : m_voices) {
        v.active = false;
        v.amp.kill();
        for (auto& e : v.env) e.kill();
    }
}
void FMSynth::process(float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx) {
    auto& p = m_params;
    int mi = 0;
    int alg = p.getInt(pAlgorithm);
    float fbAmt = p.get(pFeedback) * 1.5f;
    float level = p.get(pLevel);
    float lfoRate = p.get(pLfoRate), vib = p.get(pLfoDepth);
    float bendRange = p.get(pBendRange);
    float ratio[4], lvl[4], det[4];
    bool fixed[4];
    for (int o = 0; o < 4; ++o) {
        ratio[o] = p.get(opParam(o, 0));
        lvl[o] = p.get(opParam(o, 1));
        det[o] = p.get(opParam(o, 2));
        fixed[o] = p.getBool(opParam(o, 7));
    }
    for (int i = 0; i < n; ++i) {
        while (mi < midi.count && midi.events[mi].sampleOffset <= i) {
            const auto& e = midi.events[mi++];
            if (e.type == MidiEvent::NoteOn) {
                Voice* v = nullptr;
                for (auto& vv : m_voices)
                    if (!vv.active) {
                        v = &vv;
                        break;
                    }
                if (!v) {
                    v = &m_voices[0];
                    for (auto& vv : m_voices)
                        if (vv.age < v->age) v = &vv;
                }
                v->active = true;
                v->note = e.data1;
                v->vel = e.velocity;
                v->freq = midiToHz(e.data1);
                v->age = m_counter++;
                v->fb = 0;
                for (int o = 0; o < 4; ++o) {
                    v->ph[o] = 0;
                    v->env[o].set(m_sr, p.get(opParam(o, 3)), p.get(opParam(o, 4)), p.get(opParam(o, 5)), p.get(opParam(o, 6)));
                    v->env[o].noteOn();
                }
                v->amp.set(m_sr, p.get(pAA), p.get(pAD), p.get(pAS), p.get(pAR));
                v->amp.noteOn();
            } else if (e.type == MidiEvent::NoteOff) {
                for (auto& v : m_voices)
                    if (v.active && v.note == e.data1) {
                        v.amp.noteOff();
                        for (auto& en : v.env) en.noteOff();
                    }
            } else if (e.type == MidiEvent::AllNotesOff)
                for (auto& v : m_voices) {
                    if (e.data1 == 1) v.amp.r = 0.006f;
                    v.amp.noteOff();
                    for (auto& en : v.env) en.noteOff();
                }
            else if (e.type == MidiEvent::PitchBend) m_bend = e.pitchBend / 8192.f * bendRange;
        }
        float lfo = m_lfo.next(lfoRate, m_sr, 0);
        float s = 0;
        for (auto& v : m_voices) {
            if (!v.active) continue;
            float amp = v.amp.next();
            if (!v.amp.active()) {
                v.active = false;
                continue;
            }
            float base = v.freq * std::pow(2.f, (m_bend + lfo * vib * 0.5f) / 12.f);
            float out[4];
            float en[4];
            for (int o = 0; o < 4; ++o) en[o] = v.env[o].next() * lvl[o];
            auto op = [&](int o, float mod) {
                float f = fixed[o] ? ratio[o] * 100.f : base * ratio[o];
                f *= std::pow(2.f, det[o] / 1200.f);
                v.ph[o] += f / m_sr;
                if (v.ph[o] >= 1.0) v.ph[o] -= 1.0;
                return std::sin((float)(v.ph[o] * kTwoPi) + mod * kTwoPi) * en[o];
            };
            float fb = v.fb * fbAmt;
            float y;
            switch (alg) {
            case 0:
                out[0] = op(0, fb);
                out[1] = op(1, out[0]);
                out[2] = op(2, out[1]);
                out[3] = op(3, out[2]);
                y = out[3];
                break;
            case 1:
                out[0] = op(0, fb);
                out[1] = op(1, 0);
                out[2] = op(2, out[0] + out[1]);
                out[3] = op(3, out[2]);
                y = out[3];
                break;
            case 2:
                out[0] = op(0, fb);
                out[1] = op(1, out[0]);
                out[2] = op(2, 0);
                out[3] = op(3, out[2]);
                y = (out[1] + out[3]) * 0.6f;
                break;
            case 3:
                out[0] = op(0, fb);
                out[1] = op(1, out[0]);
                out[2] = op(2, out[0]);
                out[3] = op(3, out[0]);
                y = (out[1] + out[2] + out[3]) * 0.45f;
                break;
            case 4:
                out[0] = op(0, fb);
                out[1] = op(1, out[0]);
                out[2] = op(2, 0);
                out[3] = op(3, out[2]);
                y = (out[1] + out[3]) * 0.6f;
                break;
            case 5:
                out[0] = op(0, fb);
                out[1] = op(1, out[0]);
                out[2] = op(2, out[1]);
                out[3] = op(3, 0);
                y = (out[2] + out[3]) * 0.6f;
                break;
            case 6:
                out[0] = op(0, fb);
                out[1] = op(1, 0);
                out[2] = op(2, 0);
                out[3] = op(3, 0);
                y = (out[0] + out[1] + out[2] + out[3]) * 0.35f;
                break;
            default:
                out[0] = op(0, fb);
                out[1] = op(1, out[0]);
                out[2] = op(2, out[1]);
                out[3] = op(3, out[1]);
                y = (out[2] + out[3]) * 0.6f;
                break;
            }
            v.fb = out[0];
            s += y * amp * v.vel;
        }
        L[i] += s * level * 0.5f;
        R[i] += s * level * 0.5f;
    }
}

// ============================================================ ToneGenerator
ToneGenerator::ToneGenerator() {
    auto& p = m_params;
    p.addChoice(pWave, "Wave", {"Sine", "Saw", "Square", "Triangle", "White Noise"});
    p.add(pFreq, "Frequency", 20, 20000, 440, ParamUnit::Hertz, true);
    p.add(pLevel, "Level", 0, 1, 0.5f);
    p.addToggle(pGate, "Always On", false);
    p.add(pAttack, "Attack", 0.f, 2, 0.01f, ParamUnit::Seconds, true);
    p.add(pRelease, "Release", 0.f, 4, 0.1f, ParamUnit::Seconds, true);
}
void ToneGenerator::process(float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx) {
    auto& p = m_params;
    int mi = 0;
    int wave = p.getInt(pWave);
    float level = p.get(pLevel);
    bool always = p.getBool(pGate);
    m_env.set(m_sr, p.get(pAttack), 0.01f, 1.f, p.get(pRelease));
    for (int i = 0; i < n; ++i) {
        while (mi < midi.count && midi.events[mi].sampleOffset <= i) {
            const auto& e = midi.events[mi++];
            if (e.type == MidiEvent::NoteOn) {
                m_held++;
                m_freqOverride = midiToHz(e.data1);
                if (m_held == 1) m_env.noteOn();
            } else if (e.type == MidiEvent::NoteOff) {
                m_held = std::max(0, m_held - 1);
                if (m_held == 0) m_env.noteOff();
            } else if (e.type == MidiEvent::AllNotesOff) {
                m_held = 0;
                m_env.noteOff();
            }
        }
        if (always && !m_env.active()) m_env.noteOn();
        if (always && m_env.stage == ADSR::Release) m_env.noteOn();
        float env = m_env.next();
        if (env <= 0.f) continue;
        float f = (m_held > 0) ? m_freqOverride : p.get(pFreq);
        m_osc.setFreq(f, m_sr);
        int w = wave == 4 ? Oscillator::Noise : wave;
        float s = m_osc.next(w) * env * level;
        L[i] += s;
        R[i] += s;
    }
}

// ============================================================ MidiOutInstrument
MidiOutInstrument::MidiOutInstrument() {
    auto& p = m_params;
    p.add(pPort, "Port", 0, 16, 0);
    p.add(pChannel, "Channel", 1, 16, 1);
    p.add(pProgram, "Program", 0, 127, 0);
    p.add(pBank, "Bank", 0, 127, 0);
}
void MidiOutInstrument::process(float*, float*, int, const MidiEventList& midi, const ProcessContext&) {
    m_out.clear();
    uint8_t ch = (uint8_t)(m_params.getInt(pChannel) - 1);
    for (int i = 0; i < midi.count; ++i) {
        MidiEvent e = midi.events[i];
        e.channel = ch;
        m_out.add(e);
    }
}

}   // namespace mt
