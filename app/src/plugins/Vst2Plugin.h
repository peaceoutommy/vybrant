// VST2 adapter: wraps a legacy AEffect behind the common IProcessor interface.
#pragma once
#include "dsp/Processor.h"
#include "plugins/Vst2Abi.h"
#include <QString>
#include <atomic>
#include <vector>
#include <functional>

namespace mt {

struct Vst2Info {
    QString name;
    QString vendor;
    QString product;
    int32_t uniqueId = 0;
    int version = 0;
    bool isSynth = false;
    bool hasEditor = false;
    int numInputs = 0, numOutputs = 0;
    int numParams = 0;
};

class Vst2Plugin : public IProcessor {
public:
    static std::unique_ptr<Vst2Plugin> create(const QString& path, QString* error = nullptr);
    static bool probe(const QString& path, Vst2Info& info, QString* error = nullptr);
    ~Vst2Plugin() override;
    QString key() const override { return "vst2:" + QString::number((uint32_t)m_info.uniqueId, 16); }
    QString displayName() const override { return m_info.name; }
    ProcessorKind kind() const override { return m_info.isSynth ? ProcessorKind::Instrument : ProcessorKind::Effect; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process(float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx) override;
    ParameterSet& params() override { return m_params; }
    int latencySamples() const override { return m_effect ? m_effect->initialDelay : 0; }
    QJsonObject saveState() const override;
    void restoreState(const QJsonObject& s) override;
    QWidget* createEditor(QWidget* parent) override;
    bool hasCustomEditor() const override { return m_info.hasEditor; }
    bool isPlugin() const override { return true; }
    void setActive(bool a) override;
    bool latencyChanged() override { return m_latencyDirty.exchange(false); }
    QString formatValue(uint32_t id) const override;
    QString pluginFormat() const override { return "vst2"; }
    const MidiEventList* outputMidi() const override { return &m_outMidi; }
    void syncFromPlugin();   // UI thread: read parameter values from the plugin (e.g. after editor moves)
    std::function<void(uint32_t, float)> onParamEdited;
    const Vst2Info& info() const { return m_info; }
    intptr_t dispatch(int32_t opcode, int32_t index = 0, intptr_t value = 0, void* ptr = nullptr, float opt = 0.f) const;

private:
    Vst2Plugin() = default;
    bool init(const QString& path, QString* error);
    static intptr_t hostCallback(vst2::AEffect* e, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
    void* m_handle = nullptr;
    vst2::AEffect* m_effect = nullptr;
    Vst2Info m_info;
    ParameterSet m_params;
    std::vector<float> m_lastSent;
    double m_sr = 44100;
    int m_maxBlock = 512;
    bool m_active = false;
    std::atomic<bool> m_latencyDirty{false};
    std::vector<std::vector<float>> m_inBufs, m_outBufs;
    std::vector<float*> m_inPtrs, m_outPtrs;
    std::vector<uint8_t> m_eventsMem;
    std::vector<vst2::VstMidiEvent> m_midiEvents;
    vst2::VstTimeInfo m_timeInfo{};
    MidiEventList m_outMidi;
    QString m_path;
};

}   // namespace mt
