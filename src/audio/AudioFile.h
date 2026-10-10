// Audio asset loading (libsndfile), resampling (libsamplerate) and multiresolution waveform peak caches.
// Loading happens on worker threads; the RT thread only ever reads immutable AudioAsset data.
#pragma once
#include <string>
#include <QString>
#include <QObject>
#include <vector>
#include <memory>
#include <mutex>
#include <map>
#include <functional>
#include <atomic>

namespace mt {

struct WaveformLevel {
    int samplesPerBin;
    std::vector<float> minL, maxL, minR, maxR;
};

struct AudioAsset {
    QString path;
    double sampleRate = 44100;   // sample rate of 'data' (after conversion)
    double originalSampleRate = 44100;
    int channels = 0;   // 1 or 2
    int64_t frames = 0;
    std::vector<float> left, right;   // right aliases left content for mono
    std::vector<WaveformLevel> peaks;   // e.g. 64 / 512 / 4096 / 32768 samples per bin
    float peakValue = 1.f;
    const float* L() const { return left.data(); }
    const float* R() const { return channels > 1 ? right.data() : left.data(); }
    const WaveformLevel& levelFor(double samplesPerPixel) const;
    void buildPeaks();
    double seconds() const { return sampleRate > 0 ? frames / sampleRate : 0; }
};
using AudioAssetPtr = std::shared_ptr<const AudioAsset>;

// How a sample changes pitch / length (after FL Studio's time-stretch modes). Resample links them like a turntable; the
// stretch modes keep them independent (Rubber Band, offline) with settings tuned for the material.
enum StretchMode : int { StretchResample = 0, StretchGeneral, StretchDrums, StretchPads, StretchVocals };
inline const std::vector<std::string>& stretchModeNames() {
    static const std::vector<std::string> n = {"Resample", "Stretch", "Stretch: drums", "Stretch: pads", "Stretch: vocals"};
    return n;
}

// Static helpers
struct AudioFile {
    static std::shared_ptr<AudioAsset> load(const QString& path, double targetSampleRate, QString* error = nullptr);
    static bool write(const QString& path, const float* interleaved, int channels, int64_t frames, double sampleRate, int bitDepth,
        const QString& format /*wav|aiff|flac|ogg|mp3*/, QString* error = nullptr);
    static std::shared_ptr<AudioAsset> resample(const AudioAsset& in, double targetRate);
    // Rubber Band offline; mode is one of the stretch modes (not StretchResample)
    static std::shared_ptr<AudioAsset> stretch(const AudioAsset& in, double timeRatio, double pitchSemitones, int mode = StretchGeneral);
    static double bpmFromName(const QString& path);   // "loop 83 bpm.wav" -> 83; 0 when the name carries no tempo
    static bool probe(const QString& path, double* sr, int* channels, int64_t* frames);
    static bool isAudioFile(const QString& path);
};

// Cache of loaded assets keyed by (path, sampleRate). Loads asynchronously and notifies on the main thread.
class SampleCache : public QObject {
    Q_OBJECT
public:
    explicit SampleCache(QObject* parent = nullptr);
    ~SampleCache();
    void setSampleRate(double sr);
    double sampleRate() const { return m_sr; }
    // Returns the asset if already loaded, otherwise schedules a load and returns nullptr.
    AudioAssetPtr get(const QString& path);
    AudioAssetPtr getSync(const QString& path);   // blocking load (UI thread only, small files / preview)
    // async; nullptr until ready. One render per file at a time: requests made meanwhile return nullptr and are re-made after
    // assetLoaded, so a knob or tempo drag renders only its settled value instead of queueing a render per step
    AudioAssetPtr getStretched(const QString& path, double ratio, double pitch, int mode = StretchGeneral);
    void clear();
Q_SIGNALS:
    void assetLoaded(const QString& path);

private:
    QString keyFor(const QString& path) const;
    double m_sr = 44100;
    mutable std::mutex m_mutex;
    std::map<QString, AudioAssetPtr> m_assets;
    std::map<QString, bool> m_loading;
    std::atomic<int> m_pending{0};
};

}   // namespace mt
