// VST3 host adapter: exposes a VST3 component/controller pair through the common IProcessor interface.
#pragma once
#include "dsp/Processor.h"
#include "plugins/Vst3Abi.h"
#include <memory>
#include <vector>
#include <QString>
#include <atomic>
#include <functional>

namespace mt {

struct Vst3ClassInfo {
    QString cid;
    QString name;
    QString vendor;
    QString category;
    QString subCategories;
    QString version;
    QString sdkVersion;
};

class Vst3Module {
public:
    static std::shared_ptr<Vst3Module> load(const QString& path, QString* error = nullptr);
    ~Vst3Module();
    vst3::IPluginFactory* factory() const { return m_factory; }
    std::vector<Vst3ClassInfo> classes() const;
    QString path() const { return m_path; }
    static QString resolveBinary(const QString& bundleOrSo);

private:
    void* m_handle = nullptr;
    vst3::IPluginFactory* m_factory = nullptr;
    vst3::ModuleExitProc m_exit = nullptr;
    QString m_path;
};

struct Vst3HostObjects;   // host-side implementations (component handler, host app, run loop ...)

class Vst3Plugin : public IProcessor {
public:
    static std::unique_ptr<Vst3Plugin> create(const QString& path, const QString& classId, QString* error = nullptr);
    ~Vst3Plugin() override;
    QString key() const override { return "vst3:" + m_classId; }
    QString displayName() const override { return m_name; }
    ProcessorKind kind() const override { return m_isInstrument ? ProcessorKind::Instrument : ProcessorKind::Effect; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process(float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx) override;
    ParameterSet& params() override { return m_params; }
    int latencySamples() const override { return m_latency.load(); }
    int tailSamples() const override { return m_tail; }
    QJsonObject saveState() const override;
    void restoreState(const QJsonObject& s) override;
    QWidget* createEditor(QWidget* parent) override;
    bool hasCustomEditor() const override { return m_hasEditor; }
    bool isPlugin() const override { return true; }
    bool wantsSidechain() const override { return m_numInputBuses > 1; }
    void setActive(bool a) override;
    bool latencyChanged() override { return m_latencyDirty.exchange(false); }
    QString formatValue(uint32_t id) const override;
    QString pluginFormat() const override { return "vst3"; }
    const MidiEventList* outputMidi() const override { return &m_outMidi; }
    int auxOutputCount() const override { return std::max(0, m_numOutputBuses - 1); }
    const float* auxOutput(int i, int side) const override { return m_outPtrs[i + 1][std::min<int>(side, m_outChans[i + 1] - 1)]; }
    QString auxOutputName(int i) const override { return m_outNames[i + 1]; }
    // Called on the UI thread: push parameter values changed by automation/engine to the controller so the editor follows
    void syncControllerFromParams();
    std::function<void(uint32_t, float)> onParamEdited;   // UI -> app (plugin editor moved a parameter)
    QString classId() const { return m_classId; }
    QString vendor() const { return m_vendor; }

private:
    Vst3Plugin();
    bool init(const QString& path, const QString& classId, QString* error);
    void buildParameterMirror();
    void setupBuses();
    void refreshParamsFromController();
    std::shared_ptr<Vst3Module> m_module;
    std::unique_ptr<Vst3HostObjects> m_host;
    vst3::IComponent* m_component = nullptr;
    vst3::IAudioProcessor* m_processor = nullptr;
    vst3::IEditController* m_controller = nullptr;
    vst3::IMidiMapping* m_midiMapping = nullptr;
    bool m_controllerIsComponent = false;
    QString m_classId, m_name, m_vendor;
    bool m_isInstrument = false, m_hasEditor = false, m_active = false, m_processing = false;
    ParameterSet m_params;
    std::vector<float> m_lastSent;
    std::vector<float> m_lastPushed; /* last values pushed to the edit controller (GUI) */
    std::vector<uint32_t> m_paramIds;
    int m_bypassParamIndex = -1;
    double m_sr = 44100;
    int m_maxBlock = 512;
    std::atomic<int> m_latency{0};
    int m_tail = 0;
    std::atomic<bool> m_latencyDirty{false};
    // bus buffers
    int m_numInputBuses = 0, m_numOutputBuses = 0;
    std::vector<vst3::AudioBusBuffers> m_inBuses, m_outBuses;
    std::vector<std::vector<float*>> m_inPtrs, m_outPtrs;
    QStringList m_outNames;
    std::vector<std::vector<float>> m_scratch;
    std::vector<int> m_inChans, m_outChans;
    std::vector<float> m_inL, m_inR;
    MidiEventList m_outMidi;
    friend struct Vst3HostObjects;
    friend struct ComponentHandler;
};

}   // namespace mt
