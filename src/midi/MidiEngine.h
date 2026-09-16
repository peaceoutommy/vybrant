// ALSA sequencer based MIDI engine: device enumeration, input thread feeding the audio engine's lock-free queue,
// output drained from the engine, MIDI clock, and controller-learn support.
#pragma once
#include "dsp/Processor.h"
#include "core/RingBuffer.h"
#include <QObject>
#include <QStringList>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>

namespace mt {
class AudioEngine;

struct MidiPortInfo { int client = 0; int port = 0; QString name; bool input = false; bool output = false; QString id() const { return QString("%1:%2").arg(client).arg(port); } };

class MidiEngine : public QObject {
    Q_OBJECT
public:
    explicit MidiEngine(AudioEngine& engine, QObject* parent = nullptr);
    ~MidiEngine();
    bool start();
    void stop();
    bool isRunning() const { return m_running.load(); }
    std::vector<MidiPortInfo> ports() const;
    QStringList enabledInputs() const { return m_enabledInputs; }
    void setEnabledInputs(const QStringList& ids);
    QStringList enabledOutputs() const { return m_enabledOutputs; }
    void setEnabledOutputs(const QStringList& ids);
    void setLearning(bool on) { m_learning.store(on); }
    bool learning() const { return m_learning.load(); }
    int activityCounter() const { return m_activity.load(); }
    void sendNow(const MidiEvent& e, int port = -1);       // direct output (UI thread)
    void loadSettings(); void saveSettings() const;
Q_SIGNALS:
    void ccReceived(int cc, int channel, int value);
    void noteReceived(int note, int channel, bool on);
    void portsChanged();
private:
    void threadLoop();
    void handleAlsaEvent(void* ev);
    void flushOutput();
    void connectInput(const MidiPortInfo& p, bool connect);
    void connectOutput(const MidiPortInfo& p, bool connect);
    AudioEngine& m_engine; void* m_seq = nullptr; int m_inPort = -1, m_outPort = -1; std::thread m_thread; std::atomic<bool> m_running{false}, m_learning{false}; std::atomic<int> m_activity{0};
    QStringList m_enabledInputs, m_enabledOutputs; std::vector<std::pair<int, int>> m_outputTargets; mutable std::mutex m_mutex; int64_t m_lastLearnEmit = 0;
};

} // namespace mt
