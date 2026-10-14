// LV2 host adapter (through lilv): exposes an LV2 plugin through the common IProcessor interface. X11 UIs are embedded directly
// (no suil): the UI binary is loaded and instantiated with ui:parent set to the PluginEmbedWidget window.
#pragma once
#include "dsp/Processor.h"
#include <lilv/lilv.h>
#include <lv2/atom/forge.h>
#include <lv2/data-access/data-access.h>
#include <lv2/options/options.h>
#include <lv2/ui/ui.h>
#include <lv2/worker/worker.h>
#include <QPointer>
#include <atomic>
#include <functional>
#include <memory>
#include <semaphore>
#include <string>
#include <thread>
#include <vector>

namespace mt {

struct PluginInfo;
class PluginEmbedWidget;

// SPSC ring of {tag, size, bytes} messages between the audio thread and the UI / worker threads; push fails when full
class Lv2Ring {
public:
    explicit Lv2Ring(size_t capacity)
        : m_buf(capacity) {}
    size_t capacity() const { return m_buf.size(); }
    bool push(uint32_t tag, const void* data, uint32_t size);
    bool pop(uint32_t& tag, uint32_t& size, void* out);   // 'out' must hold capacity() bytes

private:
    void copyIn(size_t pos, const void* src, size_t n);
    void copyOut(size_t pos, void* dst, size_t n) const;
    std::vector<uint8_t> m_buf;
    alignas(64) std::atomic<size_t> m_read{0};
    alignas(64) std::atomic<size_t> m_write{0};
};

class Lv2Plugin : public IProcessor {
public:
    static std::unique_ptr<Lv2Plugin> create(const QString& uri, const QString& bundlePath, QString* error = nullptr);
    static std::vector<PluginInfo> scan();   // every plugin in $LV2_PATH, ~/.lv2, /usr/local/lib/lv2 and /usr/lib/lv2
    ~Lv2Plugin() override;
    QString key() const override { return "lv2:" + m_uri; }
    QString displayName() const override { return m_name; }
    ProcessorKind kind() const override { return m_isInstrument ? ProcessorKind::Instrument : ProcessorKind::Effect; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx) override;
    ParameterSet& params() override { return m_params; }
    int latencySamples() const override { return m_latency.load(); }
    QJsonObject saveState() const override;
    void restoreState(const QJsonObject& s) override;
    QWidget* createEditor(QWidget* parent) override;
    bool hasCustomEditor() const override { return m_ui != nullptr; }
    bool isPlugin() const override { return true; }
    bool wantsSidechain() const override { return !m_sideIns.empty(); }
    bool latencyChanged() override { return m_latencyDirty.exchange(false); }
    QString formatValue(uint32_t id) const override;
    QString pluginFormat() const override { return "lv2"; }
    std::function<void(uint32_t, float)> onParamEdited;   // UI -> app (plugin editor moved a parameter)

private:
    enum class PortType { Audio, Control, Atom, Other };
    struct Port {
        PortType type = PortType::Other;
        bool input = false, midi = false, time = false;
        int param = -1;   // control inputs: index in m_params (the parameter id is the port index)
        std::string symbol;
        std::vector<std::pair<float, QString>> scalePoints;
    };
    Lv2Plugin();
    void init(const LilvPlugin* plugin);
    bool instantiate();
    void freeInstance();
    void runBlock(float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx);
    void lockRun();
    void unlockRun() { m_busy.clear(std::memory_order_release); }
    QByteArray stateBytes();
    void applyState(const QByteArray& state);
    void workLoop();
    bool openUi(unsigned long xid, int& width, int& height);
    void closeUi();
    void idleUi();
    static LV2_Worker_Status scheduleWork(LV2_Worker_Schedule_Handle h, uint32_t size, const void* data);
    static LV2_Worker_Status workRespond(LV2_Worker_Respond_Handle h, uint32_t size, const void* data);
    static const void* portValue(const char* symbol, void* user, uint32_t* size, uint32_t* type);
    static void setPortValue(const char* symbol, void* user, const void* value, uint32_t size, uint32_t type);
    static void uiWrite(LV2UI_Controller c, uint32_t port, uint32_t size, uint32_t protocol, const void* buffer);
    static int uiRequestResize(LV2UI_Feature_Handle h, int width, int height);

    const LilvPlugin* m_plugin = nullptr;
    LilvInstance* m_instance = nullptr;
    LilvUIs* m_uis = nullptr;
    const LilvUI* m_ui = nullptr;   // the X11 UI, if the plugin has one
    QString m_uri, m_name;
    bool m_isInstrument = false;
    ParameterSet m_params;
    std::vector<Port> m_ports;
    std::vector<uint32_t> m_controlIns, m_controlOuts, m_mainIns, m_sideIns, m_audioOuts, m_atomIns, m_atomOuts;
    int m_midiIn = -1, m_latencyPort = -1;
    bool m_wantsTime = false;
    std::vector<float> m_control;   // control port buffers (RT thread only)
    std::unique_ptr<std::atomic<float>[]> m_outValues;   // control outputs mirrored for the UI
    std::vector<std::vector<float>> m_audio;   // audio / CV / unknown port buffers
    std::vector<std::vector<uint64_t>> m_atom;   // atom port buffers (8-byte aligned)
    std::vector<uint64_t> m_posBuf;   // time:Position object forged once per block
    LV2_Atom_Forge m_forge;
    double m_sr = 0;
    int m_maxBlock = 0;
    std::atomic<int> m_latency{0};
    std::atomic<bool> m_latencyDirty{false};
    std::atomic_flag m_busy;   // held by process() and by the UI thread while it restores state or re-instantiates
    QByteArray m_pendingState;   // restored before the first prepare(): applied once the instance exists
    float m_portTmp = 0;
    // features (data must outlive the instance)
    float m_optSampleRate = 0, m_optUpdateRate = 30;
    int32_t m_optMinBlock = 1, m_optMaxBlock = 0, m_optSeqSize = 0;
    LV2_Options_Option m_options[7];
    LV2_Worker_Schedule m_schedule;
    LV2_Feature m_featureList[5];
    const LV2_Feature* m_features[6];
    // worker
    const LV2_Worker_Interface* m_worker = nullptr;
    Lv2Ring m_workRequests{1 << 16}, m_workResponses{1 << 16};
    std::counting_semaphore<> m_workSem{0};
    std::thread m_workThread;
    std::atomic<bool> m_workQuit{false};
    std::vector<uint8_t> m_rtMsg;   // RT scratch for popped messages
    // X11 UI
    Lv2Ring m_toUi{1 << 18}, m_fromUi{1 << 16};
    std::atomic<bool> m_uiOpen{false};
    void* m_uiLib = nullptr;
    const LV2UI_Descriptor* m_uiDesc = nullptr;
    LV2UI_Handle m_uiHandle = nullptr;
    const LV2UI_Idle_Interface* m_uiIdle = nullptr;
    const LV2UI_Resize* m_uiResize = nullptr;
    QPointer<PluginEmbedWidget> m_uiWidget;
    int m_uiW = 0, m_uiH = 0;
    std::vector<float> m_uiSent;   // control values the UI last saw
    std::vector<uint8_t> m_uiMsg;
    LV2UI_Resize m_hostResize;
    LV2_Extension_Data_Feature m_dataAccess;
    LV2_Feature m_uiFeatureList[5];
    const LV2_Feature* m_uiFeatures[9];
};

}   // namespace mt
