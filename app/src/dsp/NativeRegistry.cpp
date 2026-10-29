#include "dsp/NativeRegistry.h"
#include "dsp/instruments/Instruments.h"
#include "dsp/effects/Effects.h"

namespace mt {

NativeRegistry& NativeRegistry::instance() {
    static NativeRegistry r;
    return r;
}

template <class T> static NativeProcessorInfo mk(const char* cat) {
    T tmp;
    return {tmp.key(), tmp.displayName(), cat, tmp.kind(), [] { return std::unique_ptr<IProcessor>(new T()); }};
}

NativeRegistry::NativeRegistry() {
    m_infos = {
        mk<Sampler>("Instruments"),
        mk<DrumSampler>("Instruments"),
        mk<Synth>("Instruments"),
        mk<WavetableSynth>("Instruments"),
        mk<FMSynth>("Instruments"),
        mk<ToneGenerator>("Instruments"),
        mk<MidiOutInstrument>("Instruments"),
        mk<Compressor>("Dynamics"),
        mk<Limiter>("Dynamics"),
        mk<Gate>("Dynamics"),
        mk<Expander>("Dynamics"),
        mk<Clipper>("Dynamics"),
        mk<TransientProcessor>("Dynamics"),
        mk<ParametricEQ>("EQ / Filter"),
        mk<ShelvingEQ>("EQ / Filter"),
        mk<HighPassFilter>("EQ / Filter"),
        mk<LowPassFilter>("EQ / Filter"),
        mk<BandPassFilter>("EQ / Filter"),
        mk<NotchFilter>("EQ / Filter"),
        mk<Delay>("Time"),
        mk<PingPongDelay>("Time"),
        mk<Reverb>("Time"),
        mk<Echo>("Time"),
        mk<Chorus>("Modulation"),
        mk<Flanger>("Modulation"),
        mk<Phaser>("Modulation"),
        mk<Tremolo>("Modulation"),
        mk<Vibrato>("Modulation"),
        mk<Saturation>("Distortion"),
        mk<Waveshaper>("Distortion"),
        mk<BitCrusher>("Distortion"),
        mk<Distortion>("Distortion"),
        mk<GainUtility>("Utility"),
        mk<StereoWidth>("Utility"),
        mk<PhaseInvert>("Utility"),
        mk<MonoConversion>("Utility"),
        mk<SpectrumAnalyzer>("Utility"),
        mk<MeterEffect>("Utility"),
    };
}

std::unique_ptr<IProcessor> NativeRegistry::create(const QString& key) const {
    for (auto& i : m_infos)
        if (i.key == key) return i.factory();
    return nullptr;
}
const NativeProcessorInfo* NativeRegistry::info(const QString& key) const {
    for (auto& i : m_infos)
        if (i.key == key) return &i;
    return nullptr;
}
std::vector<const NativeProcessorInfo*> NativeRegistry::byKind(ProcessorKind k) const {
    std::vector<const NativeProcessorInfo*> v;
    for (auto& i : m_infos)
        if (i.kind == k) v.push_back(&i);
    return v;
}

}   // namespace mt
