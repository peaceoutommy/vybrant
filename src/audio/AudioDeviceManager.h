// PortAudio wrapper: device enumeration and stream lifecycle. Supports ALSA / JACK / PipeWire (via PortAudio host APIs).
#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <vector>
#include <functional>
#include <atomic>

namespace mt {

struct AudioDeviceInfo {
    int index = -1;
    QString name;
    QString hostApi;
    int maxInputs = 0;
    int maxOutputs = 0;
    double defaultSampleRate = 44100;
    double lowLatency = 0;
};

struct AudioConfig {
    QString hostApi;
    int inputDevice = -1;
    int outputDevice = -1;
    double sampleRate = 44100;
    int bufferSize = 512;
    int inputChannels = 2;
    int outputChannels = 2;
};

class AudioDeviceManager : public QObject {
    Q_OBJECT
public:
    using Callback = std::function<void(const float* in, float* out, int frames, int inCh, int outCh, bool xrun)>;
    explicit AudioDeviceManager(QObject* parent = nullptr);
    ~AudioDeviceManager();
    bool initialize();
    std::vector<AudioDeviceInfo> devices() const;
    QStringList hostApis() const;
    AudioDeviceInfo defaultOutput() const;
    AudioDeviceInfo defaultInput() const;
    bool open(const AudioConfig& cfg, Callback cb, QString* error = nullptr);
    void close();
    bool isOpen() const { return m_stream != nullptr; }
    const AudioConfig& config() const { return m_cfg; }
    double actualSampleRate() const { return m_actualSr; }
    double reportedOutputLatency() const { return m_outLatency; }
    int xruns() const { return m_xruns.load(); }
    void resetXruns() { m_xruns.store(0); }
    static std::vector<int> standardBufferSizes() { return {32, 64, 128, 256, 512, 1024, 2048}; }
    static std::vector<double> standardSampleRates() { return {44100, 48000, 88200, 96000, 176400, 192000}; }
    void loadConfig();
    void saveConfig() const;
    AudioConfig& editableConfig() { return m_cfg; }
    void rtDispatch(const void* in, void* out, int frames, bool xrun);   // called from the PortAudio callback
Q_SIGNALS:
    void streamStateChanged(bool open);

private:
    void* m_stream = nullptr;
    AudioConfig m_cfg;
    Callback m_cb;
    bool m_paInit = false;
    double m_actualSr = 44100;
    double m_outLatency = 0;
    std::atomic<int> m_xruns{0};
    std::vector<float> m_silence;
};

}   // namespace mt
