// Parameter model shared by native instruments, native effects and hosted plugins.
// Values live in atomics so the UI thread can write and the audio thread can read without locks.
#pragma once
#include "core/Types.h"
#include <atomic>
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>

namespace mt {

enum class ParamUnit : uint8_t { Generic, Decibel, Hertz, Milliseconds, Seconds, Percent, Semitones, Cents, Ratio, Degrees, Choice, Toggle, Beats };

struct ParamDesc {
    uint32_t id = 0;               // stable id (never an index)
    std::string name;
    float min = 0.f, max = 1.f, def = 0.f;
    ParamUnit unit = ParamUnit::Generic;
    bool automatable = true;
    bool logarithmic = false;      // map normalized value logarithmically (Hz, ms); with min = 0 an exponential curve from 0 is used
    static constexpr float kExpCurve = 3000.f;   // e^k-1 style curve steepness for zero-based log params (5 s range: 10% of the knob = ~2 ms)
    int steps = 0;                 // >0 = discrete (Choice/Toggle)
    std::vector<std::string> choices;

    float toNormalized(float plain) const {
        plain = clamp(plain, min, max);
        if (logarithmic && min > 0.f) return std::log(plain / min) / std::log(max / min);
        if (logarithmic && max > 0.f) return std::log(1.f + plain / max * (kExpCurve - 1.f)) / std::log(kExpCurve);   // log-like curve that still reaches 0 (times: 0 ms is a valid setting)
        return (max > min) ? (plain - min) / (max - min) : 0.f;
    }
    float fromNormalized(float n) const {
        n = clamp(n, 0.f, 1.f);
        if (logarithmic && min > 0.f) return min * std::pow(max / min, n);
        if (logarithmic && max > 0.f) return max * (std::pow(kExpCurve, n) - 1.f) / (kExpCurve - 1.f);
        float v = min + n * (max - min);
        if (steps > 0) v = std::round(v);
        return v;
    }
};

// Read/write parameter storage with per-parameter atomics. Owned by a processor.
class ParameterSet {
public:
    uint32_t add(const ParamDesc& d) {
        m_descs.push_back(d);
        m_values.push_back(std::make_unique<std::atomic<float>>(d.def));
        m_index[d.id] = (int)m_descs.size() - 1;
        return d.id;
    }
    // Convenience builder
    uint32_t add(uint32_t id, const char* name, float min, float max, float def, ParamUnit unit = ParamUnit::Generic, bool log = false) {
        ParamDesc d; d.id = id; d.name = name; d.min = min; d.max = max; d.def = def; d.unit = unit; d.logarithmic = log; return add(d);
    }
    uint32_t addChoice(uint32_t id, const char* name, std::vector<std::string> choices, int def = 0) {
        ParamDesc d; d.id = id; d.name = name; d.min = 0; d.max = (float)choices.size() - 1; d.def = (float)def; d.unit = ParamUnit::Choice; d.steps = (int)choices.size(); d.choices = std::move(choices); return add(d);
    }
    uint32_t addToggle(uint32_t id, const char* name, bool def = false) {
        ParamDesc d; d.id = id; d.name = name; d.min = 0; d.max = 1; d.def = def ? 1.f : 0.f; d.unit = ParamUnit::Toggle; d.steps = 2; return add(d);
    }

    int count() const { return (int)m_descs.size(); }
    const ParamDesc& desc(int index) const { return m_descs[index]; }
    const ParamDesc* descById(uint32_t id) const { auto it = m_index.find(id); return it == m_index.end() ? nullptr : &m_descs[it->second]; }
    int indexOf(uint32_t id) const { auto it = m_index.find(id); return it == m_index.end() ? -1 : it->second; }

    // RT-safe reads
    float get(uint32_t id) const { int i = indexOf(id); return i < 0 ? 0.f : m_values[i]->load(std::memory_order_relaxed); }
    float getAt(int i) const { return m_values[i]->load(std::memory_order_relaxed); }
    bool  getBool(uint32_t id) const { return get(id) >= 0.5f; }
    int   getInt(uint32_t id) const { return (int)std::lround(get(id)); }
    float getNormalized(uint32_t id) const { int i = indexOf(id); return i < 0 ? 0.f : m_descs[i].toNormalized(getAt(i)); }

    void set(uint32_t id, float plain) { int i = indexOf(id); if (i >= 0) m_values[i]->store(clamp(plain, m_descs[i].min, m_descs[i].max), std::memory_order_relaxed); }
    void setNormalized(uint32_t id, float n) { int i = indexOf(id); if (i >= 0) m_values[i]->store(m_descs[i].fromNormalized(n), std::memory_order_relaxed); }
    void setAt(int i, float plain) { m_values[i]->store(clamp(plain, m_descs[i].min, m_descs[i].max), std::memory_order_relaxed); }
    void resetAll() { for (int i = 0; i < count(); ++i) setAt(i, m_descs[i].def); }

    std::atomic<float>* atomicFor(uint32_t id) { int i = indexOf(id); return i < 0 ? nullptr : m_values[i].get(); }

private:
    std::vector<ParamDesc> m_descs;
    std::vector<std::unique_ptr<std::atomic<float>>> m_values;
    std::unordered_map<uint32_t, int> m_index;
};

// Simple one-pole parameter smoother for the audio thread.
struct Smoother {
    float current = 0.f, target = 0.f, coeff = 0.001f;
    void prepare(double sr, float ms = 10.f) { coeff = 1.f - std::exp(-1.f / (float)(sr * ms * 0.001)); }
    void setImmediate(float v) { current = target = v; }
    void setTarget(float v) { target = v; }
    inline float next() { current += (target - current) * coeff; return current; }
    bool isSmoothing() const { return std::fabs(target - current) > 1e-6f; }
};

} // namespace mt
