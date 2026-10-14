// CLAP host adapter: exposes a CLAP plugin through the common IProcessor interface.
#pragma once
#include "dsp/Processor.h"
#include "plugins/ClapAbi.h"
#include <QPointer>
#include <QString>
#include <QTimer>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <vector>

class QSocketNotifier;

namespace mt {

class PluginEmbedWidget;

class ClapModule {
public:
    static std::shared_ptr<ClapModule> load(const QString& path, QString* error = nullptr);   // one shared load per path
    ~ClapModule();
    std::vector<const clap::clap_plugin_descriptor*> descriptors() const;
    const clap::clap_plugin_factory* factory() const { return m_factory; }

private:
    void* m_handle = nullptr;
    const clap::clap_plugin_entry* m_entry = nullptr;
    const clap::clap_plugin_factory* m_factory = nullptr;
};

class ClapPlugin : public IProcessor {
public:
    static std::unique_ptr<ClapPlugin> create(const QString& path, const QString& pluginId, QString* error = nullptr);
    ~ClapPlugin() override;
    QString key() const override { return "clap:" + m_id; }
    QString displayName() const override { return m_name; }
    ProcessorKind kind() const override { return m_isInstrument ? ProcessorKind::Instrument : ProcessorKind::Effect; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(float* L, float* R, int n, const MidiEventList& midi, const ProcessContext& ctx) override;
    ParameterSet& params() override { return m_params; }
    int latencySamples() const override { return m_latency.load(); }
    QJsonObject saveState() const override;
    void restoreState(const QJsonObject& s) override;
    QWidget* createEditor(QWidget* parent) override;
    bool hasCustomEditor() const override { return m_hasEditor; }
    bool isPlugin() const override { return true; }
    bool latencyChanged() override { return m_latencyDirty.exchange(false); }
    QString formatValue(uint32_t id) const override;
    QString pluginFormat() const override { return "clap"; }
    std::function<void(uint32_t, float)> onParamEdited;   // UI thread: the plugin (its GUI) changed a parameter

private:
    ClapPlugin() = default;
    bool init(const QString& path, const QString& pluginId, QString* error);
    void activate();
    void deactivate();
    void restartIfRequested();
    void refreshParamsFromPlugin();
    clap::clap_event_header* addEvent(uint32_t time, uint16_t type, uint32_t size);
    void addMidi(const MidiEvent& e, uint32_t time);
    std::shared_ptr<ClapModule> m_module;
    clap::clap_host m_host{};
    const clap::clap_plugin* m_plugin = nullptr;
    const clap::clap_plugin_params* m_paramsExt = nullptr;
    const clap::clap_plugin_state* m_state = nullptr;
    const clap::clap_plugin_latency* m_latencyExt = nullptr;
    const clap::clap_plugin_gui* m_gui = nullptr;
    const clap::clap_plugin_timer_support* m_timerExt = nullptr;
    const clap::clap_plugin_posix_fd_support* m_fdExt = nullptr;
    QString m_id, m_name;
    bool m_isInstrument = false, m_hasEditor = false, m_active = false, m_processing = false, m_guiCreated = false;
    bool m_clapNotes = false, m_midiNotes = false;   // dialects of the first note input port
    ParameterSet m_params;
    std::vector<uint32_t> m_paramIds;
    std::vector<void*> m_cookies;
    std::vector<float> m_lastSent;
    double m_sr = 44100;
    int m_maxBlock = 512;
    int64_t m_steadyTime = 0;
    std::atomic<int> m_latency{0};
    std::atomic<bool> m_latencyDirty{false};
    // audio ports: the main ports carry the track's stereo pair, any others get silent / ignored buffers
    std::vector<uint32_t> m_inChans, m_outChans;
    int m_mainIn = -1, m_mainOut = -1;
    std::vector<clap::clap_audio_buffer> m_inBufs, m_outBufs;
    std::vector<std::vector<float*>> m_inPtrs, m_outPtrs;
    std::vector<std::vector<float>> m_scratch;
    // input events, kept sorted by time; fixed capacity so process() never allocates
    union EventSlot {
        clap::clap_event_header header;
        clap::clap_event_note note;
        clap::clap_event_param_value param;
        clap::clap_event_midi midi;
    };
    std::vector<EventSlot> m_evStore;
    std::vector<const clap::clap_event_header*> m_evOrder;
    uint32_t m_evCount = 0;
    clap::clap_input_events m_inEvents{};
    clap::clap_output_events m_outEvents{};
    clap::clap_event_transport m_transport{};
    // requests from any thread, served on the UI thread by m_mainTimer
    QTimer m_mainTimer;
    std::atomic<bool> m_callbackRequested{false}, m_restartRequested{false}, m_edited{false}, m_inProcess{false};
    std::atomic<uint32_t> m_lastEdited{0};
    std::atomic<int> m_rt{0};   // restart handshake: 0 audio thread runs the plugin, 1 UI asks for it, 2 audio handed it over, 3 UI took it
    int m_restartWait = 0;
    std::map<clap::clap_id, QTimer*> m_timers;   // timer-support / posix-fd-support registrations
    std::map<int, std::vector<QSocketNotifier*>> m_fds;
    clap::clap_id m_nextTimerId = 1;
    QPointer<PluginEmbedWidget> m_embed;
    friend struct ClapHost;
};

}   // namespace mt
