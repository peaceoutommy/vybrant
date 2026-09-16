// Streaming sample preview: a loader thread decodes the file progressively into a preallocated buffer while the RT
// thread already plays the decoded part (rate-converted on the fly), so previews start instantly for any file size.
#pragma once
#include <QString>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace mt {

struct PreviewBuffer {
    QString path; double fileRate = 44100; int channels = 1; int64_t frames = 0;
    std::vector<float> left, right;                  // preallocated to 'frames'; valid up to 'available'
    std::atomic<int64_t> available{0}; std::atomic<bool> failed{false}, cancel{false}, done{false};
    static constexpr int kBins = 1024;               // coarse waveform for the UI, filled progressively
    std::vector<float> minPk, maxPk; std::atomic<int> binsReady{0};
    const float* L() const { return left.data(); } const float* R() const { return channels > 1 ? right.data() : left.data(); }
    double seconds() const { return fileRate > 0 ? frames / fileRate : 0; }
};

class PreviewLoader {
public:
    ~PreviewLoader() { cancel(); }
    // Opens the file, allocates the buffer and starts decoding in the background. Returns nullptr when the file cannot be opened.
    std::shared_ptr<PreviewBuffer> load(const QString& path, QString* error = nullptr);
    void cancel();
private:
    std::shared_ptr<PreviewBuffer> m_current; std::thread m_thread;
};

} // namespace mt
