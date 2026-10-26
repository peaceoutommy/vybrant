// Out-of-process plugins: each bridged plugin runs in its own `vybrant --plugin-bridge` process, so a plugin that
// crashes (or corrupts memory) takes only that process down; the app keeps running and the plugin goes silent.
// Audio is handed over per block through shared memory and a pair of process-shared semaphores (no added latency);
// everything else (create, state, value text, editor) goes over a local socket as JSON lines. Editors are X11 windows
// of the plugin process embedded into ours.
#pragma once
#include "dsp/Processor.h"
#include "project/Project.h"
#include <QObject>
#include <QTimer>
#include <atomic>
#include <functional>
#include <map>
#include <memory>

class QLocalServer;
class QLocalSocket;
class QProcess;

namespace mt {

namespace bridge {
struct Shm;
}

class BridgedPlugin : public QObject, public IProcessor {
    Q_OBJECT
public:
    static std::unique_ptr<BridgedPlugin> create(const PluginInstanceData& data, QString* error);
    ~BridgedPlugin() override;
    static inline std::function<void(Id pluginInstanceId, const QString& name)> onCrashed;   // UI thread

    QString key() const override { return m_key; }
    QString displayName() const override { return m_name; }
    ProcessorKind kind() const override { return m_kind; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process(float* left, float* right, int numFrames, const MidiEventList& midi, const ProcessContext& ctx) override;
    ParameterSet& params() override { return m_params; }
    int latencySamples() const override { return m_latency.load(); }
    int tailSamples() const override { return m_tail; }
    QJsonObject saveState() const override;
    void restoreState(const QJsonObject& s) override;
    QWidget* createEditor(QWidget* parent) override;
    bool hasCustomEditor() const override { return m_hasEditor; }
    bool isPlugin() const override { return true; }
    bool wantsSidechain() const override { return m_wantsSidechain; }
    const MidiEventList* outputMidi() const override { return &m_outMidi; }
    bool latencyChanged() override { return m_latency.load() != m_reportedLatency.exchange(m_latency.load()); }
    QString formatValue(uint32_t paramId) const override;
    QString pluginFormat() const override { return m_format; }
    int auxOutputCount() const override { return m_dead.load() ? 0 : (int)m_auxNames.size(); }   // a dead one must not replay its last block
    const float* auxOutput(int index, int side) const override;
    QString auxOutputName(int index) const override { return m_auxNames[index]; }
    bool dead() const { return m_dead.load(); }
    std::function<void(uint32_t, float)> onParamEdited;   // the plugin's own editor moved a parameter (UI thread)

private:
    BridgedPlugin() = default;
    QJsonObject call(const QJsonObject& request, int timeoutMs = 10000) const;   // UI thread; empty object when the plugin is gone
    void send(const QJsonObject& message) const;   // no answer expected
    void pump() const;   // read what the plugin process sent: answers for call(), editor size events
    bool childExited() const;
    void died();
    PluginInstanceData m_data;
    QString m_key, m_name, m_format, m_shmName;
    ProcessorKind m_kind = ProcessorKind::Effect;
    ParameterSet m_params;
    QStringList m_auxNames;
    bool m_hasEditor = false, m_wantsSidechain = false;
    int m_tail = 0;
    std::atomic<int> m_latency{0};
    mutable std::atomic<int> m_reportedLatency{0};
    std::atomic<bool> m_dead{false}, m_edited{false}, m_reported{false};
    bridge::Shm* m_shm = nullptr;
    QProcess* m_proc = nullptr;
    QLocalServer* m_server = nullptr;
    QLocalSocket* m_sock = nullptr;
    mutable QByteArray m_inbox;
    mutable QString m_waitingFor;
    mutable QJsonObject m_answer;
    int m_pidfd = -1, m_pid = 0;
    MidiEventList m_outMidi;
    mutable QJsonObject m_lastState;   // what a dead plugin saves: its last known state
    mutable std::map<uint32_t, std::pair<float, QString>> m_valueText;   // value text cache: one round trip per change
    QTimer m_editTimer;
    std::function<void(int, int)> m_editorSized;   // the plugin process resized its editor
    bool m_shuttingDown = false;
};

// Entry point of the plugin process (`vybrant --plugin-bridge <socket> <shm>`)
int pluginBridgeMain(const QString& socketName, const QString& shmName);

}   // namespace mt
