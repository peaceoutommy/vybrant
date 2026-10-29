// Streaming sample preview: a loader thread decodes the file progressively into a preallocated buffer while the RT
// thread already plays the decoded part (rate-converted on the fly), so previews start instantly for any file size.
#pragma once
#include <QString>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace mt {

struct PreviewBuffer {
    QString path;
    double fileRate = 44100;
    int channels = 1;
    int64_t frames = 0;   // fileRate / channels / frames are set by the loader thread before 'available' becomes > 0
    std::vector<float> left, right;   // preallocated to 'frames'; valid up to 'available'
    std::atomic<int64_t> available{0};
    std::atomic<bool> failed{false}, cancel{false}, done{false};
    static constexpr int kBins = 1024;   // coarse waveform for the UI, filled progressively
    std::vector<float> minPk, maxPk;
    std::atomic<int> binsReady{0};
    int64_t createdUs = 0;
    std::atomic<int64_t> firstAudioUs{
        -1};   // diagnostics: when load() was called / when the first sample reached the output (steady clock, microseconds)
    const float* L() const { return left.data(); }
    const float* R() const { return channels > 1 ? right.data() : left.data(); }
    double seconds() const { return fileRate > 0 ? frames / fileRate : 0; }
};

class PreviewLoader {
public:
    ~PreviewLoader() { cancel(); }
    // Returns a buffer immediately; the file is opened, allocated and decoded on a background thread (opening an mp3/flac
    // can take tens of ms - that never blocks the click). A file that cannot be opened flags 'failed'.
    std::shared_ptr<PreviewBuffer> load(const QString& path, QString* error = nullptr);
    void cancel();

private:
    std::shared_ptr<PreviewBuffer> m_current;
};

}   // namespace mt
