#include "audio/AudioDeviceManager.h"
#include "core/Log.h"
#include <portaudio.h>
#include <QSettings>

namespace mt {

AudioDeviceManager::AudioDeviceManager(QObject* parent)
    : QObject(parent) {}
AudioDeviceManager::~AudioDeviceManager() {
    close();
    if (m_paInit) Pa_Terminate();
}

bool AudioDeviceManager::initialize() {
    if (m_paInit) return true;
    PaError e = Pa_Initialize();
    if (e != paNoError) {
        Log::error(QString("PortAudio init failed: %1").arg(Pa_GetErrorText(e)));
        return false;
    }
    m_paInit = true;
    return true;
}

std::vector<AudioDeviceInfo> AudioDeviceManager::devices() const {
    std::vector<AudioDeviceInfo> v;
    if (!m_paInit) return v;
    int n = Pa_GetDeviceCount();
    for (int i = 0; i < n; ++i) {
        const PaDeviceInfo* d = Pa_GetDeviceInfo(i);
        if (!d) continue;
        AudioDeviceInfo info;
        info.index = i;
        info.name = QString::fromUtf8(d->name);
        info.hostApi = QString::fromUtf8(Pa_GetHostApiInfo(d->hostApi)->name);
        info.maxInputs = d->maxInputChannels;
        info.maxOutputs = d->maxOutputChannels;
        info.defaultSampleRate = d->defaultSampleRate;
        info.lowLatency = d->defaultLowOutputLatency;
        v.push_back(info);
    }
    return v;
}
QStringList AudioDeviceManager::hostApis() const {
    QStringList l;
    if (!m_paInit) return l;
    for (int i = 0; i < Pa_GetHostApiCount(); ++i) l << QString::fromUtf8(Pa_GetHostApiInfo(i)->name);
    return l;
}

static int paCallback(
    const void* in, void* out, unsigned long frames, const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags statusFlags, void* user) {
    auto* self = static_cast<AudioDeviceManager*>(user);
    bool xrun = (statusFlags & (paOutputUnderflow | paInputOverflow)) != 0;
    self->rtDispatch(in, out, (int)frames, xrun);
    return paContinue;
}

void AudioDeviceManager::rtDispatch(const void* in, void* out, int frames, bool xrun) {
    if (xrun) m_xruns.fetch_add(1, std::memory_order_relaxed);
    const float* inF = in ? static_cast<const float*>(in) : m_silence.data();
    m_cb(inF, static_cast<float*>(out), frames, in ? m_cfg.inputChannels : 0, m_cfg.outputChannels, xrun);
}

bool AudioDeviceManager::open(const AudioConfig& cfgIn, Callback cb, QString* error) {
    close();
    if (!initialize()) {
        if (error) *error = "PortAudio not initialised";
        return false;
    }
    AudioConfig cfg = cfgIn;
    m_cb = std::move(cb);
    if (cfg.outputDevice < 0 || cfg.outputDevice >= Pa_GetDeviceCount()) cfg.outputDevice = Pa_GetDefaultOutputDevice();
    if (cfg.outputDevice < 0) {
        if (error) *error = "No output device available";
        return false;
    }
    const PaDeviceInfo* od = Pa_GetDeviceInfo(cfg.outputDevice);
    cfg.outputChannels = std::min(std::max(cfg.outputChannels, 1), std::max(1, od->maxOutputChannels));
    if (cfg.outputChannels < 2 && od->maxOutputChannels >= 2) cfg.outputChannels = 2;
    PaStreamParameters outP{};
    outP.device = cfg.outputDevice;
    outP.channelCount = cfg.outputChannels;
    outP.sampleFormat = paFloat32;
    outP.suggestedLatency = od->defaultLowOutputLatency;
    PaStreamParameters inP{};
    bool useIn = false;
    if (cfg.inputDevice >= 0 && cfg.inputDevice < Pa_GetDeviceCount()) {
        const PaDeviceInfo* id = Pa_GetDeviceInfo(cfg.inputDevice);
        if (id->maxInputChannels > 0) {
            useIn = true;
            cfg.inputChannels = std::min(std::max(cfg.inputChannels, 1), id->maxInputChannels);
            inP.device = cfg.inputDevice;
            inP.channelCount = cfg.inputChannels;
            inP.sampleFormat = paFloat32;
            inP.suggestedLatency = id->defaultLowInputLatency;
        }
    }
    if (!useIn) cfg.inputChannels = 0;
    m_silence.assign((size_t)cfg.bufferSize * std::max(1, cfg.inputChannels) * 4, 0.f);
    m_cfg = cfg;
    PaStream* s = nullptr;
    PaError e = Pa_OpenStream(
        &s, useIn ? &inP : nullptr, &outP, cfg.sampleRate, (unsigned long)cfg.bufferSize, paClipOff, &paCallback, this);
    if (e != paNoError && useIn) {   // retry without input
        Log::warn(QString("Opening with input failed (%1); retrying output only").arg(Pa_GetErrorText(e)));
        m_cfg.inputChannels = 0;
        e = Pa_OpenStream(&s, nullptr, &outP, cfg.sampleRate, (unsigned long)cfg.bufferSize, paClipOff, &paCallback, this);
    }
    if (e != paNoError) {
        if (error) *error = QString::fromUtf8(Pa_GetErrorText(e));
        Log::error("Audio open failed: " + QString::fromUtf8(Pa_GetErrorText(e)));
        return false;
    }
    const PaStreamInfo* si = Pa_GetStreamInfo(s);
    m_actualSr = si ? si->sampleRate : cfg.sampleRate;
    m_outLatency = si ? si->outputLatency : 0;
    e = Pa_StartStream(s);
    if (e != paNoError) {
        Pa_CloseStream(s);
        if (error) *error = QString::fromUtf8(Pa_GetErrorText(e));
        return false;
    }
    m_stream = s;
    Log::info(QString("Audio stream open: %1 @ %2 Hz, %3 frames, in=%4 out=%5")
            .arg(QString::fromUtf8(od->name))
            .arg(m_actualSr)
            .arg(cfg.bufferSize)
            .arg(m_cfg.inputChannels)
            .arg(m_cfg.outputChannels));
    Q_EMIT streamStateChanged(true);
    return true;
}

void AudioDeviceManager::close() {
    if (!m_stream) return;
    Pa_StopStream((PaStream*)m_stream);
    Pa_CloseStream((PaStream*)m_stream);
    m_stream = nullptr;
    Q_EMIT streamStateChanged(false);
}

void AudioDeviceManager::loadConfig() {
    QSettings s;
    s.beginGroup("audio");
    m_cfg.hostApi = s.value("hostApi").toString();
    m_cfg.inputDevice = s.value("inputDevice", -1).toInt();
    m_cfg.outputDevice = s.value("outputDevice", -1).toInt();
    m_cfg.sampleRate = s.value("sampleRate", 44100.0).toDouble();
    m_cfg.bufferSize = s.value("bufferSize", 512).toInt();
    m_cfg.inputChannels = s.value("inputChannels", 2).toInt();
    m_cfg.outputChannels = s.value("outputChannels", 2).toInt();
    // Device indexes are not stable across sessions; match by name
    QString outName = s.value("outputName").toString(), inName = s.value("inputName").toString();
    if (m_paInit) {
        for (auto& d : devices()) {
            if (!outName.isEmpty() && d.name == outName && d.maxOutputs > 0) m_cfg.outputDevice = d.index;
            if (!inName.isEmpty() && d.name == inName && d.maxInputs > 0) m_cfg.inputDevice = d.index;
        }
    }
}
void AudioDeviceManager::saveConfig() const {
    QSettings s;
    s.beginGroup("audio");
    s.setValue("hostApi", m_cfg.hostApi);
    s.setValue("inputDevice", m_cfg.inputDevice);
    s.setValue("outputDevice", m_cfg.outputDevice);
    s.setValue("sampleRate", m_cfg.sampleRate);
    s.setValue("bufferSize", m_cfg.bufferSize);
    s.setValue("inputChannels", m_cfg.inputChannels);
    s.setValue("outputChannels", m_cfg.outputChannels);
    for (auto& d : devices()) {
        if (d.index == m_cfg.outputDevice) s.setValue("outputName", d.name);
        if (d.index == m_cfg.inputDevice) s.setValue("inputName", d.name);
    }
}

}   // namespace mt
