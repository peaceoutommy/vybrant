// Core types shared by every subsystem. Kept free of Qt so DSP code stays portable.
#pragma once
#include <cstdint>
#include <cmath>
#include <string>
#include <algorithm>

namespace mt {

using Id = uint64_t;            // Stable unique object id (0 = invalid)
using Tick = int64_t;           // Musical time in MIDI ticks
constexpr int PPQ = 960;        // Ticks per quarter note
constexpr Id kInvalidId = 0;

inline Tick beatsToTicks(double beats) { return (Tick)std::llround(beats * PPQ); }
inline double ticksToBeats(Tick t) { return (double)t / PPQ; }

inline double ticksToSeconds(Tick t, double bpm) { return (double)t / PPQ * 60.0 / bpm; }
inline Tick secondsToTicks(double s, double bpm) { return (Tick)std::llround(s * bpm / 60.0 * PPQ); }
inline double ticksToSamples(Tick t, double bpm, double sr) { return ticksToSeconds(t, bpm) * sr; }
inline Tick samplesToTicks(double samples, double bpm, double sr) { return secondsToTicks(samples / sr, bpm); }

inline float dbToGain(float db) { return db <= -96.f ? 0.f : std::pow(10.f, db / 20.f); }
inline float gainToDb(float g) { return g <= 1e-5f ? -96.f : 20.f * std::log10(g); }
inline float midiToHz(double note) { return (float)(440.0 * std::pow(2.0, (note - 69.0) / 12.0)); }
template <class T> inline T clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

enum class CurveType : uint8_t { Linear = 0, Stepped, Smooth, Bezier };

// Shared time-signature / snap helpers
struct TimeSignature { int numerator = 4; int denominator = 4; Tick ticksPerBar() const { return (Tick)PPQ * 4 / denominator * numerator; } Tick ticksPerBeat() const { return (Tick)PPQ * 4 / denominator; } };

} // namespace mt
