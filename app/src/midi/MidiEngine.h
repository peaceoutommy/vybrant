// MIDI engine: device enumeration, input feeding the audio engine's lock-free queue, output drained from the engine, MIDI clock,
// and controller-learn support. Devices go through the ALSA sequencer on Linux and WinMM on Windows.
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

struct MidiPortInfo {
    int client = 0;
    int port = 0;
    QString name;
    bool input = false;
    bool output = false;
    QString id() const { return QString("%1:%2").arg(client).arg(port); }
};

class MidiEngine : public QObject {
    Q_OBJECT
public:
    explicit MidiEngine(AudioEngine& engine, QObject* parent = nullptr);
    ~MidiEngine();
    bool start();
    void stop();
    std::vector<MidiPortInfo> ports() const;
    QStringList enabledInputs() const { return m_enabledInputs; }
    void setEnabledInputs(const QStringList& ids);
    QStringList enabledOutputs() const { return m_enabledOutputs; }
    void setEnabledOutputs(const QStringList& ids);
    void setLearning(bool on) { m_learning.store(on); }
    void setClockFollow(bool on) { m_clockIn.store(on); }   // follow incoming MIDI clock: tempo, start / stop, song position
    int activityCounter() const { return m_activity.load(); }
    void loadSettings();
    void saveSettings() const;
Q_SIGNALS:
    void ccReceived(int cc, int channel, int value);
    void clockTransport(int what, double tick);   // MIDI clock in: 0 start, 1 continue, 2 stop, 3 song position at 'tick' (UI thread)

private:
    void threadLoop();
    void handleAlsaEvent(void* ev);
    void handleClock(int status, int songPos);   // a MIDI realtime / song position status byte
    void received(const MidiEvent& e);   // from the device thread: to the engine, and to controller learn
    void flushOutput();
    void connectInput(const MidiPortInfo& p, bool connect);
    void connectOutput(const MidiPortInfo& p, bool connect);
    AudioEngine& m_engine;
    void* m_seq = nullptr;
    int m_inPort = -1, m_outPort = -1;
    std::thread m_thread;
    std::atomic<bool> m_running{false}, m_learning{false}, m_clockIn{false};
    // MIDI clock in (MIDI thread only)
    static constexpr int kClockWindow = 96;   // tempo = average over the last 4 beats of clocks
    double m_clockTimes[kClockWindow] = {};
    int64_t m_clockN = 0, m_clockCount = 0;
    double m_clockBase = 0;
    bool m_clockRunning = false, m_clockSpp = false;
    std::atomic<int> m_activity{0};
    QStringList m_enabledInputs, m_enabledOutputs;
    std::vector<std::pair<int, int>> m_outputTargets;
    mutable std::mutex m_mutex;
    int64_t m_lastLearnEmit = 0;
};

}   // namespace mt
